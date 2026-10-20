#include "AppUpdater.h"
#include "AppStorage.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSysInfo>
#include <QVersionNumber>

namespace {
const QString REPOSITORY = "MikeQu223/Rhine-Music-Database";
constexpr qint64 MAX_PACKAGE_BYTES = 2LL * 1024 * 1024 * 1024;

QVersionNumber parseVersion(QString value) {
    static const QRegularExpression pattern("^v?(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$");
    if (pattern.match(value).capturedLength() != value.size()) {
        return {};
    }
    if (value.startsWith('v')) {
        value.remove(0, 1);
    }
    const auto version = QVersionNumber::fromString(value);
    if (version.segmentCount() != 3) {
        return {};
    }
    for (const auto segment : version.segments()) {
        if (segment > 65535) {
            return {};
        }
    }
    return version;
}

bool officialUrl(const QUrl &url, const QString &prefix) {
    return url.scheme() == "https" && url.host() == "github.com" && url.userInfo().isEmpty() &&
           url.port(-1) == -1 && !url.hasQuery() && !url.hasFragment() &&
           url.path().startsWith('/' + REPOSITORY + prefix);
}

QNetworkRequest request(const QUrl &url) {
    QNetworkRequest value(url);
    value.setHeader(QNetworkRequest::UserAgentHeader, "RhineMusicDatabase/" + AppUpdater::currentVersion());
    value.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    value.setTransferTimeout(30000);
    return value;
}
}

AppUpdater::AppUpdater(bool offline, QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), m_offline(offline), m_network(network ? network : &m_defaultNetwork) {
}
AppUpdater::~AppUpdater() {
    // 网络回调引用本对象的文件与哈希，析构前必须断开并终止请求。
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
    }
}
QString AppUpdater::currentVersion() {
    return QStringLiteral(RHINE_VERSION);
}
QString AppUpdater::platformAsset() {
    const auto arch = QSysInfo::buildCpuArchitecture();
#ifdef Q_OS_MACOS
    return arch == "arm64" ? "macOS-arm64.zip" : QString();
#elif defined(Q_OS_WIN)
    return arch == "x86_64" ? "Windows-x64.zip" : QString();
#else
    return arch == "x86_64" ? "Linux-x86_64.AppImage" : QString();
#endif
}
bool AppUpdater::isNewer(const QString &candidate, const QString &current) {
    const auto version = parseVersion(candidate);
    const auto installed = parseVersion(current);
    return !version.isNull() && !installed.isNull() && QVersionNumber::compare(version, installed) > 0;
}

/**
 * 验证正式版本与当前架构的完整发布包；不依据日期或字符串顺序判断版本。
 * 没有 SHA-256、包大小或官方仓库下载地址时拒绝下载，避免拿到源码包或其他平台文件。
 */
QJsonObject AppUpdater::selectRelease(const QJsonObject &release, const QString &platform,
                                      const QString &current, QString *error) {
    error->clear();
    const auto tag = release["tag_name"].toString();
    if (release["draft"].toBool() || release["prerelease"].toBool() || parseVersion(tag).isNull()) {
        *error = "发布版本格式无效，或不是正式版本";
        return {};
    }
    if (!isNewer(tag, current)) {
        return {};
    }
    const QUrl page(release["html_url"].toString());
    if (!officialUrl(page, "/releases/tag/") || page.path() != '/' + REPOSITORY + "/releases/tag/" + tag) {
        *error = "发布页面地址无效";
        return {};
    }
    if (!platform.isEmpty()) {
        const auto expectedName = "RhineMusicDatabase-" + tag + '-' + platform;
        for (const auto &value : release["assets"].toArray()) {
            const auto asset = value.toObject();
            if (asset["name"].toString() != expectedName) {
                continue;
            }
            const auto digest = asset["digest"].toString();
            const QUrl url(asset["browser_download_url"].toString());
            const auto expectedPath = '/' + REPOSITORY + "/releases/download/" + tag + '/' + expectedName;
            const auto size = asset["size"].toDouble();
            static const QRegularExpression hashPattern("^sha256:[0-9a-f]{64}$");
            if (!officialUrl(url, "/releases/download/") || url.path() != expectedPath ||
                !hashPattern.match(digest).hasMatch() || size <= 0 || size > MAX_PACKAGE_BYTES ||
                size != qint64(size) || asset["state"].toString() != "uploaded") {
                *error = "更新包信息不完整或校验信息无效，请查看发布页面";
                return {};
            }
            return {{"version", tag}, {"page", page.toString()}, {"asset", asset}};
        }
    }
    *error = "新版本尚无适合当前架构的更新包，请查看发布页面";
    return {};
}
bool AppUpdater::busy() const {
    return !m_reply.isNull();
}
bool AppUpdater::available() const {
    return !m_release.isEmpty();
}
QString AppUpdater::latestVersion() const {
    return m_release["version"].toString();
}
QString AppUpdater::status() const {
    return m_status;
}
QString AppUpdater::downloadedPath() const {
    return m_downloadedPath;
}
void AppUpdater::fail(const QString &message) {
    if (m_file) {
        m_file->cancelWriting();
        m_file.reset();
    }
    m_status = message;
    emit changed();
}

QString AppUpdater::rateLimitStatus() const {
    const auto time = QDateTime::fromSecsSinceEpoch(m_retryAtUnix).toLocalTime().toString("HH:mm:ss");
    return "当前网络出口的 GitHub 更新检查额度已用尽，请在 " + time + " 后重试或查看发布页面";
}

/** 根据服务端限流证据和恢复时间退避，普通 403 不再误报为用户点击频繁。 */
QString AppUpdater::responseError(QNetworkReply *reply, int code, const QByteArray &body) {
    const auto message = QJsonDocument::fromJson(body).object()["message"].toString();
    const auto retryAfter = reply->rawHeader("Retry-After");
    const bool exhausted = reply->rawHeader("X-RateLimit-Remaining") == "0";
    const bool limited =
        code == 429 || (code == 403 && (exhausted || !retryAfter.isEmpty() ||
                                        message.contains("rate limit", Qt::CaseInsensitive)));
    if (limited) {
        constexpr qint64 DEFAULT_RETRY_SECONDS = 60;
        const auto now = QDateTime::currentSecsSinceEpoch();
        m_retryAtUnix = now + DEFAULT_RETRY_SECONDS;
        const auto seconds = retryAfter.toLongLong();
        const auto reset = reply->rawHeader("X-RateLimit-Reset").toLongLong();
        if (seconds > 0) {
            m_retryAtUnix = now + qMin(seconds, qint64(86400));
        } else if (exhausted && reset > now) {
            m_retryAtUnix = reset;
        }
        return rateLimitStatus();
    }
    if (code == 403) {
        return "GitHub 拒绝访问更新服务（HTTP 403），请检查代理或查看发布页面";
    }
    return "检查失败，请检查网络后重试";
}

/** 查询公开正式版本；优先读取静态清单，旧版 Release 缺少清单时才回退 REST API。 */
void AppUpdater::check() {
    if (busy()) {
        return;
    }
    if (m_offline) {
        fail("离线模式不检查更新");
        return;
    }
    if (QDateTime::currentSecsSinceEpoch() < m_retryAtUnix) {
        fail(rateLimitStatus());
        return;
    }
    m_status = "正在检查更新…";
    fetchRelease(false);
}

void AppUpdater::fetchRelease(bool useApi) {
    const auto endpoint = useApi
                              ? "https://api.github.com/repos/" + REPOSITORY + "/releases/latest"
                              : "https://github.com/" + REPOSITORY + "/releases/latest/download/update.json";
    auto query = request(QUrl(endpoint));
    if (useApi) {
        query.setRawHeader("Accept", "application/vnd.github+json");
        query.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    }
    auto reply = m_network->get(query);
    m_reply = reply;
    reply->setReadBufferSize(1024 * 1024 + 1);
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > 1024 * 1024) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, useApi] {
        m_reply.clear();
        reply->deleteLater();
        const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto body = reply->readAll();
        if (code == 404 && !useApi) {
            fetchRelease(true);
            return;
        }
        if (code == 404) {
            fail("尚无正式发布版本");
            return;
        }
        if (reply->error() != QNetworkReply::NoError || code != 200) {
            fail(responseError(reply, code, body));
            return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            fail("更新服务返回了无效数据");
            return;
        }
        QString error;
        const auto release = selectRelease(document.object(), platformAsset(), currentVersion(), &error);
        // 重新检查成功后才替换旧信息，网络失败仍允许重试先前已验证的更新包。
        if (m_release != release) {
            m_downloadedPath.clear();
        }
        m_release = release;
        if (!error.isEmpty()) {
            fail(error);
            return;
        }
        if (available()) {
            m_status = "发现新版本 " + latestVersion();
            emit updateAvailable();
        } else {
            m_status = "当前已是最新版本";
        }
        emit changed();
    });
    emit changed();
}

/** 流式写入临时文件并计算哈希；只有完整、校验匹配的文件才能出现在下载目录。 */
void AppUpdater::download() {
    if (busy() || !available() || m_offline) {
        return;
    }
    const auto asset = m_release["asset"].toObject();
    const auto directory = AppStorage::dataDirectory() + "/updates";
    if (!QDir().mkpath(directory)) {
        fail("无法创建更新下载目录");
        return;
    }
    m_downloadedPath.clear();
    m_file = std::make_unique<QSaveFile>(directory + '/' + asset["name"].toString());
    if (!m_file->open(QIODevice::WriteOnly)) {
        fail("无法写入更新包，请检查空间和目录权限");
        return;
    }
    m_hash.reset();
    m_received = 0;
    m_status = "正在下载更新…";
    auto reply = m_network->get(request(QUrl(asset["browser_download_url"].toString())));
    m_reply = reply;
    reply->setReadBufferSize(1024 * 1024);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] { consumeDownload(reply); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, asset] {
        consumeDownload(reply);
        m_reply.clear();
        reply->deleteLater();
        if (!m_file) {
            emit changed();
            return;
        }
        if (reply->error() != QNetworkReply::NoError ||
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
            fail("下载未完成，请检查网络后重试");
            return;
        }
        const auto digest = "sha256:" + QString::fromLatin1(m_hash.result().toHex());
        if (m_received != qint64(asset["size"].toDouble()) || digest != asset["digest"].toString()) {
            fail("更新包校验失败，文件已丢弃，请重新下载");
            return;
        }
        const auto filename = m_file->fileName();
        if (!m_file->commit()) {
            fail("无法保存更新包，请检查磁盘空间");
            return;
        }
        m_file.reset();
#ifdef Q_OS_LINUX
        QFile::setPermissions(filename, QFile::permissions(filename) | QFileDevice::ExeOwner);
#endif
        m_downloadedPath = filename;
        m_status = "下载完成，SHA-256 校验通过；请退出后手动替换";
        emit changed();
    });
    emit changed();
}
void AppUpdater::consumeDownload(QNetworkReply *reply) {
    if (!m_file) {
        return;
    }
    const auto chunk = reply->readAll();
    m_received += chunk.size();
    const auto size = qint64(m_release["asset"].toObject()["size"].toDouble());
    if (m_received > size || m_file->write(chunk) != chunk.size()) {
        fail("下载大小异常或磁盘写入失败，文件已丢弃");
        reply->abort();
        return;
    }
    m_hash.addData(chunk);
    const auto status = QString("正在下载更新… %1%").arg(m_received * 100 / size);
    if (status != m_status) {
        m_status = status;
        emit changed();
    }
}
void AppUpdater::cancel() {
    if (!m_reply) {
        return;
    }
    auto reply = m_reply.data();
    m_reply.clear();
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
    fail("已取消，可重新检查或下载");
}
void AppUpdater::openDownload() {
    if (!m_downloadedPath.isEmpty() && !QFileInfo::exists(m_downloadedPath)) {
        m_downloadedPath.clear();
        fail("更新包已被移动或删除，请重新下载");
        return;
    }
    if (!m_downloadedPath.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_downloadedPath).absolutePath()));
    }
}
void AppUpdater::openRelease() {
    const auto page = m_release["page"].toString();
    QDesktopServices::openUrl(
        QUrl(page.isEmpty() ? "https://github.com/" + REPOSITORY + "/releases/latest" : page));
}
