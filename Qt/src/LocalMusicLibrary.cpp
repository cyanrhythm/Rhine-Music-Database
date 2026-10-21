#include "LocalMusicLibrary.h"
#include "AppStorage.h"
#include <QCryptographicHash>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QtConcurrentRun>
#include <fileref.h>
#include <tfilestream.h>
#include <tag.h>
#include <tpropertymap.h>
#include <tvariant.h>
#include <algorithm>

namespace {
QString digest(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}
QString normalizedPath(const QString &path) {
    const QFileInfo file(path);
    const auto canonical = file.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? file.absoluteFilePath() : canonical);
}
QString string(const TagLib::String &value) {
    return QString::fromUtf8(value.toCString(true));
}
struct IgnoreRule {
    QString base;
    QRegularExpression expression;
    bool negate = false;
    bool directory = false;
};
QVector<IgnoreRule> readRules(const QString &directory, QVector<IgnoreRule> rules) {
    for (const auto &name : {".rhineignore", ".foliaignore"}) {
        QFile file(directory + "/" + name);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
            continue;
        }
        for (auto line : QString::fromUtf8(file.readAll()).split('\n')) {
            line = line.trimmed();
            if (line.isEmpty() || line.startsWith('#')) {
                continue;
            }
            IgnoreRule rule;
            rule.base = directory;
            rule.negate = line.startsWith('!');
            if (rule.negate) {
                line.remove(0, 1);
            }
            rule.directory = line.endsWith('/');
            if (rule.directory) {
                line.chop(1);
            }
            const bool anchored = line.startsWith('/');
            if (anchored) {
                line.remove(0, 1);
            }
            const bool basename = !line.contains('/') && !anchored;
            QString expression = basename ? "(?:^|/)" : "^";
            for (qsizetype i = 0; i < line.size(); ++i) {
                const auto character = line.at(i);
                if (character == '\\' && i + 1 < line.size()) {
                    expression += QRegularExpression::escape(QString(line.at(++i)));
                } else if (character == '*') {
                    if (i + 1 < line.size() && line.at(i + 1) == '*') {
                        ++i;
                        if (i + 1 < line.size() && line.at(i + 1) == '/') {
                            ++i;
                            expression += "(?:.*/)?";
                        } else {
                            expression += ".*";
                        }
                    } else {
                        expression += "[^/]*";
                    }
                } else if (character == '?') {
                    expression += "[^/]";
                } else {
                    expression += QRegularExpression::escape(QString(character));
                }
            }
            rule.expression = QRegularExpression(expression + '$');
            rules.append(rule);
        }
    }
    return rules;
}
bool ignored(const QFileInfo &file, const QVector<IgnoreRule> &rules) {
    bool excluded = false;
    for (const auto &rule : rules) {
        const auto relative = QDir(rule.base).relativeFilePath(file.absoluteFilePath());
        if ((!rule.directory || file.isDir()) && rule.expression.match(relative).hasMatch()) {
            excluded = !rule.negate;
        }
    }
    return excluded;
}
QString folderCover(const QString &directory) {
    const QDir folder(directory);
    const auto entries = folder.entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    for (const auto &name : {"cover", "folder", "front"}) {
        for (const auto &file : entries) {
            if (file.completeBaseName().compare(name, Qt::CaseInsensitive) == 0 &&
                QStringList{"jpg", "jpeg", "png", "webp"}.contains(file.suffix().toLower())) {
                return file.absoluteFilePath();
            }
        }
    }
    return {};
}
QString fingerprint(const QFileInfo &file, const QString &cover) {
    const QFileInfo image(cover);
    return QString::number(file.size()) + ':' + QString::number(file.lastModified().toMSecsSinceEpoch()) +
           ':' + cover + ':' + QString::number(image.size()) + ':' +
           QString::number(image.lastModified().toMSecsSinceEpoch());
}
QString storeCover(const QByteArray &bytes, const QString &storage) {
    if (bytes.isEmpty() || bytes.size() > 16 * 1024 * 1024) {
        return {};
    }
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 8192 || size.height() > 8192) {
        return {};
    }
    const auto path = storage + "/covers/" + digest(bytes);
    if (!QFileInfo::exists(path)) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            return {};
        }
    }
    return QUrl::fromLocalFile(path).toString();
}
QJsonObject readTrack(const QFileInfo &file, const QString &cover, const QString &storage) {
#ifdef Q_OS_WIN
    const auto filename = file.absoluteFilePath().toStdWString();
#else
    const auto filename = QFile::encodeName(file.absoluteFilePath());
#endif
    TagLib::FileStream stream(filename.data(), true);
    const TagLib::FileRef reference(&stream, true, TagLib::AudioProperties::Average);
    if (reference.isNull() || !reference.file()->isValid() || !reference.audioProperties() ||
        reference.audioProperties()->sampleRate() <= 0) {
        return {};
    }
    const auto tag = reference.tag();
    const auto properties = reference.audioProperties();
    const auto path = normalizedPath(file.absoluteFilePath());
    const auto id = digest(path.toUtf8());
    auto title = tag ? string(tag->title()) : QString();
    auto artist = tag ? string(tag->artist()) : QString();
    // 无标签下载文件常用“歌手 - 歌名”，拆出可靠的检索词供既有匹配流程使用。
    if (title.isEmpty()) {
        const auto basename = file.completeBaseName();
        const auto separator = basename.indexOf(" - ");
        if (separator > 0 && separator + 3 < basename.size()) {
            title = basename.mid(separator + 3).trimmed();
            if (artist.isEmpty()) {
                artist = basename.left(separator).trimmed();
            }
        } else {
            title = basename;
        }
    }
    const auto album = tag ? string(tag->album()) : QString();
    QJsonObject track{{"provider", "local"},
                      {"kind", "song"},
                      {"key", "local:song:" + id},
                      {"id", id},
                      {"path", path},
                      {"url", QUrl::fromLocalFile(path).toString()},
                      {"title", title},
                      {"artist", artist.isEmpty() ? QStringLiteral("未知歌手") : artist},
                      {"album", album},
                      {"duration", properties->lengthInMilliseconds()},
                      {"sampleRate", properties->sampleRate()},
                      {"bitrate", properties->bitrate()},
                      {"format", file.suffix().toUpper()},
                      {"fingerprint", fingerprint(file, cover)},
                      {"date", tag && tag->year() ? QString::number(tag->year()) : QString()}};
    const auto tags = reference.properties();
    const auto albumArtist =
        tags.contains("ALBUMARTIST") ? string(tags.value("ALBUMARTIST").toString()) : artist;
    track["albumID"] = digest((album + '\n' + albumArtist).toUtf8());
    QJsonArray trackArtists;
    const auto names =
        artist.split(QRegularExpression(QStringLiteral("\\s*[,;/、]\\s*")), Qt::SkipEmptyParts);
    for (const auto &name : names) {
        trackArtists.append(QJsonObject{{"id", digest(name.toUtf8())}, {"name", name}});
    }
    track["artists"] = trackArtists;
    QByteArray bytes;
    if (!cover.isEmpty()) {
        QFile image(cover);
        if (image.open(QIODevice::ReadOnly) && image.size() <= 16 * 1024 * 1024) {
            bytes = image.readAll();
        }
    }
    if (bytes.isEmpty()) {
        for (const auto &picture : reference.complexProperties("PICTURE")) {
            const auto image = picture.value("data").toByteVector();
            if (!image.isEmpty() && image.size() <= 16 * 1024 * 1024) {
                bytes = QByteArray(image.data(), int(image.size()));
                break;
            }
        }
    }
    track["cover"] = storeCover(bytes, storage);
    if (tags.contains("LYRICS")) {
        track["localLyrics"] = string(tags.value("LYRICS").toString()).left(1024 * 1024);
    }
    return track;
}
struct ScanResult {
    QJsonObject index;
    int added = 0;
    int reused = 0;
    int skipped = 0;
    QStringList unavailable;
};
/** 工作者只使用值快照；取消时不发布半成品，离线目录保留旧索引以便恢复。 */
ScanResult scanRoots(const QJsonArray &roots, const QJsonArray &previous, const QString &storage,
                     const std::shared_ptr<std::atomic_bool> &cancelled) {
    QHash<QString, QJsonObject> old;
    for (const auto &entry : previous) {
        old.insert(entry.toObject()["path"].toString(), entry.toObject());
    }
    ScanResult result;
    QJsonArray songs;
    QSet<QString> visited;
    QSet<QString> indexed;
    QHash<QString, QString> folderCovers;
    const QSet<QString> formats{"mp3", "flac", "m4a",  "mp4", "aac", "ogg", "opus",
                                "ape", "wav",  "aiff", "aif", "wv",  "wma", "mpc"};
    std::function<void(QFileInfo, QVector<IgnoreRule>)> walk;
    walk = [&](QFileInfo file, QVector<IgnoreRule> rules) {
        if (cancelled->load() || ignored(file, rules) || file.isSymLink()) {
            return;
        }
        const auto path = normalizedPath(file.absoluteFilePath());
        if (file.isDir()) {
            if (visited.contains(path)) {
                return;
            }
            visited.insert(path);
            if (!file.isReadable()) {
                result.unavailable.append(path);
                for (auto it = old.cbegin(); it != old.cend(); ++it) {
                    if (it.key().startsWith(path + '/') && !indexed.contains(it.key())) {
                        songs.append(it.value());
                        indexed.insert(it.key());
                    }
                }
                return;
            }
            rules = readRules(path, rules);
            for (const auto &child :
                 QDir(path).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                walk(child, rules);
            }
            return;
        }
        if (!formats.contains(file.suffix().toLower()) || indexed.contains(path)) {
            return;
        }
        indexed.insert(path);
        const auto directory = file.absolutePath();
        if (!folderCovers.contains(directory)) {
            folderCovers.insert(directory, folderCover(directory));
        }
        const auto cover = folderCovers.value(directory);
        const auto cached = old.value(path);
        if (!cached.isEmpty() && cached["fingerprint"] == fingerprint(file, cover) &&
            (cached["cover"].toString().isEmpty() ||
             QFileInfo::exists(QUrl(cached["cover"].toString()).toLocalFile()))) {
            songs.append(cached);
            ++result.reused;
            return;
        }
        auto track = readTrack(file, cover, storage);
        if (!track.isEmpty()) {
            songs.append(track);
            ++result.added;
        } else {
            ++result.skipped;
        }
    };
    for (const auto &entry : roots) {
        const auto root = entry.toObject();
        const auto path = root["path"].toString();
        const QFileInfo file(path);
        if (!file.exists() || !file.isReadable()) {
            result.unavailable.append(path);
            for (const auto &song : previous) {
                const auto track = song.toObject();
                const auto trackPath = track["path"].toString();
                if ((trackPath == path || trackPath.startsWith(path + '/')) && !indexed.contains(trackPath)) {
                    songs.append(track);
                    indexed.insert(trackPath);
                }
            }
        } else {
            walk(file, {});
        }
    }
    result.index = {{"version", 1}, {"roots", roots}, {"songs", songs}};
    return result;
}
}

LocalMusicLibrary::LocalMusicLibrary(QString storage, bool temporary, QObject *parent)
    : QObject(parent), m_temporary(temporary ? std::make_unique<QTemporaryDir>() : nullptr),
      m_storage(temporary ? m_temporary->path()
                          : (storage.isEmpty() ? AppStorage::dataDirectory() + "/local-library" : storage)) {
    QDir().mkpath(m_storage + "/covers");
    QFile file(m_storage + "/index.json");
    if (file.open(QIODevice::ReadOnly)) {
        const auto index = QJsonDocument::fromJson(file.readAll()).object();
        if (index["version"].toInt() == 1) {
            m_index = index;
        }
    }
}
LocalMusicLibrary::~LocalMusicLibrary() {
    if (m_cancel) {
        m_cancel->store(true);
    }
}
QJsonArray LocalMusicLibrary::roots() const {
    return m_index["roots"].toArray();
}
QJsonArray LocalMusicLibrary::songs() const {
    return m_index["songs"].toArray();
}
bool LocalMusicLibrary::contains(const QString &path) const {
    for (const auto &song : songs()) {
        if (song.toObject()["path"] == path) {
            return true;
        }
    }
    return false;
}
bool LocalMusicLibrary::save(const QJsonObject &index) {
    QSaveFile file(m_storage + "/index.json");
    const auto bytes = QJsonDocument(index).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
void LocalMusicLibrary::cancel() {
    if (!busy) {
        return;
    }
    ++m_generation;
    if (m_cancel) {
        m_cancel->store(true);
    }
    busy = false;
    status = QStringLiteral("已取消扫描，保留上次完成的索引");
    emit changed();
}
void LocalMusicLibrary::importPaths(const QStringList &paths) {
    if (busy) {
        return;
    }
    auto entries = roots();
    QSet<QString> known;
    for (const auto &root : entries) {
        known.insert(root.toObject()["path"].toString());
    }
    for (const auto &input : paths) {
        const auto path = normalizedPath(input);
        if (QFileInfo::exists(path) && !known.contains(path)) {
            entries.append(QJsonObject{{"path", path}, {"name", QFileInfo(path).fileName()}});
            known.insert(path);
        }
    }
    scan(entries);
}
void LocalMusicLibrary::rescan() {
    if (!busy) {
        scan(roots());
    }
}
/** 主线程仅提交完成且可原子保存的索引，旧代数及取消任务不会覆盖后续操作。 */
void LocalMusicLibrary::scan(QJsonArray entries) {
    const auto generation = ++m_generation;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    busy = true;
    status = QStringLiteral("正在扫描文件并读取标签与封面…");
    emit changed();
    auto watcher = new QFutureWatcher<ScanResult>(this);
    connect(watcher, &QFutureWatcher<ScanResult>::finished, this, [this, watcher, generation] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation != m_generation) {
            return;
        }
        busy = false;
        if (!save(result.index)) {
            status = QStringLiteral("本地索引保存失败，保留原索引，请检查磁盘空间与权限");
        } else {
            m_index = result.index;
            status = QStringLiteral("索引完成：%1 首，读取 %2，复用 %3，跳过 %4")
                         .arg(songs().size())
                         .arg(result.added)
                         .arg(result.reused)
                         .arg(result.skipped);
            if (!result.unavailable.isEmpty()) {
                status += QStringLiteral("；%1 个目录不可访问，已保留其索引").arg(result.unavailable.size());
            }
        }
        emit changed();
    });
    watcher->setFuture(QtConcurrent::run(scanRoots, entries, songs(), m_storage, m_cancel));
}
void LocalMusicLibrary::removeRoot(int index) {
    if (index < 0 || index >= roots().size()) {
        return;
    }
    cancel();
    auto entries = roots();
    const auto path = entries.at(index).toObject()["path"].toString();
    entries.removeAt(index);
    QJsonArray retained;
    for (const auto &song : songs()) {
        const auto track = song.toObject();
        const auto location = track["path"].toString();
        bool included = false;
        for (const auto &root : entries) {
            const auto remaining = root.toObject()["path"].toString();
            included |= location == remaining || location.startsWith(remaining + '/');
        }
        if (included || (location != path && !location.startsWith(path + '/'))) {
            retained.append(track);
        }
    }
    const QJsonObject next{{"version", 1}, {"roots", entries}, {"songs", retained}};
    if (save(next)) {
        m_index = next;
        status = QStringLiteral("已移除索引，磁盘文件保持原样");
    } else {
        status = QStringLiteral("移除失败：无法保存索引");
    }
    emit changed();
}
QJsonArray LocalMusicLibrary::collections(QString kind) const {
    QMap<QString, QJsonObject> values;
    for (const auto &entry : songs()) {
        const auto song = entry.toObject();
        QString id;
        QString title;
        if (kind == "folder") {
            id = QFileInfo(song["path"].toString()).absolutePath();
            title = QFileInfo(id).fileName();
        } else if (kind == "album") {
            id = song["albumID"].toString();
            title = song["album"].toString();
            if (title.isEmpty()) {
                continue;
            }
        } else {
            id = song["artists"].toArray().first().toObject()["id"].toString();
            title = song["artist"].toString();
        }
        auto value = values.value(id, QJsonObject{{"provider", "local"},
                                                  {"kind", kind},
                                                  {"id", id},
                                                  {"key", "local:" + kind + ':' + digest(id.toUtf8())},
                                                  {"title", title},
                                                  {"cover", song["cover"]},
                                                  {"artist", song["artist"]}});
        value["trackCount"] = value["trackCount"].toInt() + 1;
        values[id] = value;
    }
    QJsonArray result;
    for (const auto &value : values) {
        result.append(value);
    }
    return result;
}
QJsonArray LocalMusicLibrary::tracks(const QJsonObject &collection) const {
    const auto kind = collection["kind"].toString();
    const auto id = collection["id"].toString();
    QJsonArray result;
    for (const auto &entry : songs()) {
        const auto song = entry.toObject();
        const auto path = song["path"].toString();
        const bool folder =
            kind == "folder" && (QFileInfo(path).absolutePath() == id || path.startsWith(id + '/'));
        const bool album = kind == "album" && song["albumID"] == id;
        const bool artist = kind == "artist" && song["artists"].toArray().first().toObject()["id"] == id;
        if (folder || album || artist) {
            result.append(song);
        }
    }
    return result;
}
QJsonArray LocalMusicLibrary::search(QString query, QString kind) const {
    const auto values = kind == "song" ? songs() : collections(kind == "playlist" ? "folder" : kind);
    const auto words = query.normalized(QString::NormalizationForm_KC)
                           .toCaseFolded()
                           .split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    QJsonArray result;
    for (const auto &entry : values) {
        const auto item = entry.toObject();
        const auto text = (item["title"].toString() + ' ' + item["artist"].toString() + ' ' +
                           item["album"].toString() + ' ' + item["path"].toString())
                              .normalized(QString::NormalizationForm_KC)
                              .toCaseFolded();
        if (std::all_of(words.cbegin(), words.cend(),
                        [&text](const auto &word) { return text.contains(word); })) {
            result.append(item);
        }
    }
    return result;
}
