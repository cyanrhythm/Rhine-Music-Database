#include "MusicPlayer.h"
#include <QJsonDocument>
#include <QFileInfo>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>

QString MusicPlayer::identity(const QJsonObject &track) {
    return track["provider"].toString("netease") + ":" + track["id"].toString();
}

/** 只识别本次可播放地址及其响应元数据；不读取偏好或歌曲可用音质列表。 */
QString MusicPlayer::sourceQuality(const QString &provider, const QJsonObject &audio, bool trial) {
    if (trial || audio["url"].toString().isEmpty()) {
        return {};
    }
    const auto url = QUrl(audio["url"].toString());
    const auto file = url.fileName().toUpper();
    auto format = audio["type"].toString().toLower();
    if (format.isEmpty()) {
        format = audio["extName"].toString().toLower();
    }
    if (format.isEmpty()) {
        format = QFileInfo(url.path()).suffix().toLower();
    }
    if (provider == "qq") {
        // 登录接口可能没有 quality；前缀是实际获准播放的媒体文件类型。
        if (file.startsWith("F000") || file.startsWith("A000")) {
            return "sq";
        }
        if (file.startsWith("M800")) {
            return "hq";
        }
        if (file.startsWith("M500") || file.startsWith("C400")) {
            return "standard";
        }
        return {};
    }
    const auto bitrate = audio["br"].toDouble();
    const bool lossy = format == "mp3" || format == "m4a" || format == "aac";
    if (lossy && bitrate > 0) {
        // 替代音源可能保留原等级；已返回的有损格式与码率优先于等级标签。
        return bitrate >= 192000 ? "hq" : "standard";
    }
    if (provider == "netease") {
        const auto level = audio["level"].toString();
        const QHash<QString, QString> levels{
            {"standard", "standard"}, {"higher", "hq"},         {"exhigh", "hq"}, {"lossless", "sq"},
            {"hires", "hires"},       {"jyeffect", "jyeffect"}, {"sky", "sky"},   {"jymaster", "jymaster"}};
        if (levels.contains(level)) {
            const auto resolved = levels.value(level);
            if (!lossy || resolved == "standard" || resolved == "hq") {
                return resolved;
            }
        }
    }
    const bool lossless = format == "flac" || format == "ape" || format == "alac";
    if (lossless) {
        // 无损码率高不能证明 Hi-Res；必须同时取得采样率与位深。
        if (audio["sampleRate"].toDouble() > 48000 && audio["bitDepth"].toInt() > 16) {
            return "hires";
        }
        return "sq";
    }
    return {};
}

QVector<QPair<QString, QString>> MusicPlayer::qualityOptions(const QString &provider) {
    if (provider == "qq") {
        return {{"best", "自动最高可用"},
                {"standard", "标准"},
                {"exhigh", "HQ 高品质"},
                {"lossless", "SQ 无损 / FLAC"},
                {"ape", "SQ 无损 / APE"}};
    }
    if (provider == "kugou") {
        return {{"standard", "标准"},
                {"exhigh", "HQ 高品音质"},
                {"lossless", "SQ 无损音质"},
                {"hires", "Hi-Res 高解析音质"}};
    }
    return {
        {"standard", "标准"},           {"higher", "较高"},      {"exhigh", "极高"}, {"lossless", "SQ 无损"},
        {"hires", "Hi-Res 高解析音质"}, {"jymaster", "超清母带"}};
}

/** 共享音质偏好；换源保留队列、歌词、播放位置和暂停意图，迟到响应由播放代数隔离。 */
void MusicPlayer::setQuality(const QString &value) {
    const auto options = qualityOptions(m_api->provider);
    const auto found = std::find_if(options.begin(), options.end(),
                                    [&value](const auto &option) { return option.first == value; });
    if (found == options.end() || quality == value) {
        return;
    }
    quality = value;
    m_host->setPreference("quality", value);
    if (current.isEmpty() || (loading && !m_resumePosition)) {
        emit changed();
        return;
    }
    if (!m_resumePosition) {
        m_resumePosition = state["position"].toDouble();
        m_resumePaused = !state["playing"].toBool();
    }
    const auto generation = ++m_generation;
    m_host->mediaControl("stop");
    actualQuality.clear();
    failed = false;
    trial = false;
    loading = true;
    status = QStringLiteral("正在切换音质…");
    emit changed();
    prepare(current, generation, true);
}

MusicPlayer::MusicPlayer(Host *host, MusicApi *api, QObject *parent)
    : QObject(parent), lyrics(host), m_host(host), m_api(api) {
    state = {{"playing", false}, {"position", 0}, {"duration", 0}, {"volume", 0.7}, {"seekable", false}};
    quality = host->preference("quality");
    if (!QStringList{"standard", "higher", "exhigh", "lossless", "hires", "jymaster", "best", "ape"}.contains(
            quality)) {
        quality = "exhigh";
    }
    alternative = host->preference("alternate-source") == "on";
    const auto saved = QJsonDocument::fromJson(host->preference("play-history").toUtf8()).array();
    const QRegularExpression identifier(QStringLiteral("^[a-zA-Z0-9_-]{1,80}$"));
    for (const auto &value : saved) {
        const auto track = value.toObject();
        if (track["kind"] == "song" && identifier.match(track["id"].toString()).hasMatch()) {
            history.append(track);
            if (history.size() == 200) {
                break;
            }
        }
    }
    connect(&lyrics, &LyricsController::changed, this, &MusicPlayer::changed);
}

MusicApi::Valid MusicPlayer::valid(quint64 generation) {
    return [owner = QPointer(this), generation] { return owner && owner->m_generation == generation; };
}

void MusicPlayer::enqueue(const QJsonObject &track, bool next) {
    if (track["kind"] != "song") {
        return;
    }
    for (qsizetype i = 0; i < queue.size(); ++i) {
        if (i != index && identity(queue.at(i).toObject()) == identity(track)) {
            queue.removeAt(i);
            if (i < index) {
                --index;
            }
            break;
        }
    }
    queue.insert(next ? std::max(0, index + 1) : queue.size(), track);
    emit changed();
}

/** 停止旧音源并推进代数，先完成元数据再交付播放；迟到的地址不能恢复旧曲目。 */
void MusicPlayer::play(QJsonObject track, std::optional<QJsonArray> source, std::optional<int> queueIndex,
                       bool preview) {
    if (track["kind"] != "song") {
        return;
    }
    if (source) {
        QSet<QString> seen;
        queue = {};
        for (const auto &value : *source) {
            const auto song = value.toObject();
            const auto key = identity(song);
            if (song["kind"] == "song" && !seen.contains(key)) {
                seen.insert(key);
                queue.append(song);
            }
        }
    }
    int selected = -1;
    for (qsizetype i = 0; i < queue.size(); ++i) {
        if (identity(queue.at(i).toObject()) == identity(track)) {
            selected = int(i);
            break;
        }
    }
    if (selected < 0) {
        selected = int(queue.size());
        queue.append(track);
    }
    index = queueIndex.value_or(selected);
    const auto generation = ++m_generation;
    m_resumePosition.reset();
    previewRequested = preview && m_api->provider == "qq";
    m_host->mediaControl("stop");
    current = track;
    actualQuality.clear();
    lyrics.clear();
    coverImage.clear();
    loading = true;
    failed = false;
    trial = false;
    status = QStringLiteral("正在准备：") + track["title"].toString();
    emit changed();
    if (!track["deferred"].toBool()) {
        prepare(track, generation);
        return;
    }
    m_api->call(
        "songs", {QJsonValue(QJsonArray{track["id"]})},
        [this, generation](MusicReply result) {
            const auto songs = result.value.toArray();
            if (!result.ok() || songs.isEmpty()) {
                fail(result.ok() ? QStringLiteral("此曲目暂不可读取") : result.error);
                return;
            }
            current = songs.first().toObject();
            if (index >= 0 && index < queue.size()) {
                queue.replace(index, current);
            }
            emit changed();
            prepare(current, generation);
        },
        valid(generation));
}

void MusicPlayer::prepare(QJsonObject track, quint64 generation, bool reload) {
    if (!reload) {
        if (!track["cover"].toString().isEmpty()) {
            m_host->loadCover(track["cover"].toString(), [this, alive = valid(generation)](QString key) {
                if (alive()) {
                    coverImage = key;
                    emit changed();
                }
            });
        }
        emit trackSelected(track);
        lyrics.load(track);
    }
    const auto qqQuality = quality == "best" || quality == "ape";
    const auto effectiveQuality = qqQuality && m_api->provider != "qq" ? "lossless" : quality;
    m_api->call(
        previewRequested ? "preview" : "playback", {track, effectiveQuality, alternative},
        [this, track](MusicReply result) {
            if (!result.ok()) {
                fail(result.error);
                return;
            }
            const auto audio = result.value.toObject();
            const auto url = audio["url"].toString();
            if (url.isEmpty()) {
                fail(QStringLiteral("此歌曲暂无可用音源，请检查登录和歌曲权限"));
                return;
            }
            m_host->mediaMetadata(track["title"].toString(), track["artist"].toString(),
                                  track["album"].toString());
            if (!m_host->mediaLoad(url, m_api->provider == "netease" && alternative)) {
                fail(QStringLiteral("音频地址不可用"));
                return;
            }
            if (m_resumePosition && m_resumePaused) {
                m_host->mediaControl("pause");
            }
            m_endSerial = m_host->mediaState().value("ended").toLongLong();
            auto entry = track;
            entry["wiki"] = "";
            entry["description"] = "";
            QJsonArray recent{entry};
            for (const auto &value : history) {
                if (identity(value.toObject()) != identity(track) && recent.size() < 200) {
                    recent.append(value);
                }
            }
            history = recent;
            m_host->setPreference("play-history",
                                  QString::fromUtf8(QJsonDocument(history).toJson(QJsonDocument::Compact)));
            const auto preview = audio["freeTrialInfo"];
            trial = audio["preview"].toBool() || preview.isObject() || preview.isArray() ||
                    (preview.isString() && !preview.toString().isEmpty() && preview.toString() != "null") ||
                    (preview.isBool() && preview.toBool()) || (preview.isDouble() && preview.toDouble() != 0);
            actualQuality = sourceQuality(m_api->provider, audio, trial);
            status = trial ? QStringLiteral("当前音源为试听片段")
                           : QStringLiteral("正在播放：") + track["title"].toString();
            const QHash<QString, QString> qualities{{"128", "标准"}, {"320", "高品质"}, {"flac", "无损"}};
            if (!trial && qualities.contains(audio["quality"].toString())) {
                status += QStringLiteral("（") + qualities.value(audio["quality"].toString()) +
                          QStringLiteral("）");
            }
            if (!audio["qualityFallback"].toString().isEmpty()) {
                status += QStringLiteral("（已切换为") + audio["qualityFallback"].toString() +
                          QStringLiteral("音质）");
            }
            loading = false;
            emit changed();
        },
        valid(generation));
}

void MusicPlayer::fail(QString error) {
    m_resumePosition.reset();
    failed = true;
    actualQuality.clear();
    status = error;
    loading = false;
    emit changed();
}

void MusicPlayer::tick(double time, bool force) {
    if (!force && time - m_lastPoll < 0.1) {
        return;
    }
    m_lastPoll = time;
    const auto previous = state;
    const auto update = QJsonObject::fromVariantMap(m_host->mediaState());
    for (auto it = update.begin(); it != update.end(); ++it) {
        state[it.key()] = it.value();
    }
    if (m_resumePosition && !loading && state["seekable"].toBool()) {
        const auto position = std::clamp(*m_resumePosition, 0., state["duration"].toDouble());
        m_resumePosition.reset();
        m_host->mediaControl("seek", position);
        if (m_resumePaused) {
            m_host->mediaControl("pause");
        }
    }
    lyrics.sync(state["position"].toDouble());
    const auto playbackError = state["error"].toString();
    if (!playbackError.isEmpty() && (!failed || !actualQuality.isEmpty() || status != playbackError)) {
        // URL 获取成功不代表后续解码成功；失败后立即撤下旧音质，避免面板继续显示可用音源。
        fail(playbackError);
    } else if (state != previous) {
        emit changed();
    }
    if (!current.isEmpty() && !loading && state.contains("ended") &&
        state["ended"].toInteger() != m_endSerial) {
        m_endSerial = state["ended"].toInteger();
        if (onEnd) {
            onEnd();
        } else {
            next(1, true);
        }
    }
}

void MusicPlayer::toggle() {
    if (!loading && !current.isEmpty()) {
        m_host->mediaControl(state["playing"].toBool() ? "pause" : "resume");
    }
}

void MusicPlayer::seek(double fraction) {
    if (state["seekable"].toBool()) {
        m_host->mediaControl("seek", std::clamp(fraction, 0., 1.) * state["duration"].toDouble());
    }
}

void MusicPlayer::volume(double value) {
    const auto volume = std::clamp(value, 0., 1.);
    m_host->mediaControl("volume", volume);
    m_host->setPreference("volume", QString::number(volume));
}

void MusicPlayer::next(int direction, bool ended) {
    if (queue.isEmpty()) {
        return;
    }
    if (ended && mode == "one") {
        play(current, std::nullopt, index);
        return;
    }
    auto selected = index + direction;
    if (mode == "shuffle" && queue.size() > 1) {
        selected = (index + 1 + QRandomGenerator::global()->bounded(int(queue.size() - 1))) % queue.size();
    }
    if (selected >= queue.size() && mode == "off") {
        status = QStringLiteral("队列已播放完");
        emit changed();
        return;
    }
    selected = ((selected % queue.size()) + queue.size()) % queue.size();
    play(queue.at(selected).toObject(), std::nullopt, selected);
}

void MusicPlayer::remove(int selected) {
    if (selected < 0 || selected >= queue.size() || selected == index) {
        return;
    }
    queue.removeAt(selected);
    if (selected < index) {
        --index;
    }
    emit changed();
}

void MusicPlayer::clearUpcoming() {
    queue = current.isEmpty() ? QJsonArray{} : QJsonArray{current};
    index = current.isEmpty() ? -1 : 0;
    emit changed();
}

/** 停止播放并作废音源与歌词请求；保留音量和历史等可恢复偏好。 */
void MusicPlayer::stop() {
    ++m_generation;
    m_resumePosition.reset();
    lyrics.clear();
    loading = false;
    failed = false;
    status.clear();
    actualQuality.clear();
    coverImage.clear();
    current = {};
    queue = {};
    index = -1;
    m_host->mediaControl("stop");
    m_host->mediaMetadata({}, {}, {});
    state["playing"] = false;
    state["position"] = 0;
    state["duration"] = 0;
    emit changed();
}
