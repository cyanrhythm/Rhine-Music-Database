#include "AppUpdater.h"
#include "AppStorage.h"
#include "Host.h"
#include <QGuiApplication>
#include <QDebug>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QTimer>
#include <cstring>

namespace {
const QByteArray PACKAGE = "offline update archive fixture";
const QString TAG = "v9.99.0";
const QString BASE = "https://github.com/MikeQu223/Rhine-Music-Database/releases/";

QJsonObject release(const QString &platform) {
    const auto name = "RhineMusicDatabase-" + TAG + '-' + platform;
    const auto digest = QCryptographicHash::hash(PACKAGE, QCryptographicHash::Sha256).toHex();
    return {
        {"tag_name", TAG},
        {"html_url", BASE + "tag/" + TAG},
        {"draft", false},
        {"prerelease", false},
        {"assets", QJsonArray{QJsonObject{{"name", name},
                                          {"state", "uploaded"},
                                          {"size", PACKAGE.size()},
                                          {"digest", "sha256:" + QString::fromLatin1(digest)},
                                          {"browser_download_url", BASE + "download/" + TAG + '/' + name}}}}};
}

// 复用真实 QNetworkAccessManager 请求入口，仅将传输替换成异步本地响应。
class Reply : public QNetworkReply {
  public:
    Reply(const QNetworkRequest &request, QByteArray body, int status,
          const QHash<QByteArray, QByteArray> &headers, QObject *parent)
        : QNetworkReply(parent), m_body(std::move(body)) {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
            setRawHeader(it.key(), it.value());
        }
        if (status >= 400) {
            setError(QNetworkReply::ContentAccessDenied, "fixture HTTP error");
        }
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] {
            if (isFinished()) {
                return;
            }
            emit readyRead();
            if (!isFinished()) {
                setFinished(true);
                emit finished();
            }
        });
    }
    void abort() override {
        if (!isFinished()) {
            setError(OperationCanceledError, "cancelled");
            setFinished(true);
            emit finished();
        }
    }
    qint64 bytesAvailable() const override {
        return m_body.size() - m_offset + QNetworkReply::bytesAvailable();
    }

  protected:
    qint64 readData(char *buffer, qint64 maximum) override {
        const auto count = qMin(maximum, qint64(m_body.size()) - m_offset);
        if (count <= 0) {
            return -1;
        }
        std::memcpy(buffer, m_body.constData() + m_offset, size_t(count));
        m_offset += count;
        return count;
    }

  private:
    QByteArray m_body;
    qint64 m_offset = 0;
};
class Network : public QNetworkAccessManager {
  public:
    QJsonObject metadata = release(AppUpdater::platformAsset());
    QByteArray package = PACKAGE;
    QByteArray responseOverride;
    int status = 200;
    int feedStatus = 200;
    QHash<QByteArray, QByteArray> headers;
    int requests = 0;

  protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        ++requests;
        const bool isFeed = request.url().path().endsWith("/update.json");
        const bool isMetadata = isFeed || request.url().host() == "api.github.com";
        const auto response =
            isMetadata ? (responseOverride.isEmpty() ? QJsonDocument(metadata).toJson() : responseOverride)
                       : package;
        const auto responseStatus = isFeed && feedStatus != 200 ? feedStatus : status;
        return new Reply(request, response, responseStatus, headers, this);
    }
};
bool awaitIdle(AppUpdater &updater) {
    if (!updater.busy()) {
        return true;
    }
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&updater, &AppUpdater::changed, &loop, [&] {
        if (!updater.busy()) {
            loop.quit();
        }
    });
    timeout.start(1000);
    loop.exec();
    return !updater.busy();
}
}

/** 验证真实更新控制器的版本、资产选择、网络失败、取消和落盘校验，不访问外网。 */
int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid() || !AppStorage::initializePortable(directory.path()).isEmpty()) {
        return 1;
    }
    int failures = 0;
    auto check = [&](bool condition, const char *description) {
        if (!condition) {
            qCritical() << description;
            ++failures;
        }
    };
    check(AppUpdater::isNewer("v1.10.0", "1.9.9"), "numeric version comparison");
    check(!AppUpdater::isNewer("v1.0.0", "1.0.0"), "same version must not update");
    check(!AppUpdater::isNewer("v1.0.0", "2.0.0"), "must not downgrade");
    check(!AppUpdater::isNewer("v3.0.0-beta", "1.0.0"), "prerelease must not update");
    check(!AppUpdater::isNewer("v999999999999999999999.0.0", "1.0.0"), "overflow rejected");
    check(!AppUpdater::isNewer("v2.0.0\n", "1.0.0"), "trailing newline rejected");
    check(!AppUpdater::isNewer("v01.0.0", "1.0.0"), "invalid version rejected");
    for (const auto &platform : {"macOS-arm64.zip", "Windows-x64.zip", "Linux-x86_64.AppImage"}) {
        QString error;
        auto metadata = release(platform);
        check(!AppUpdater::selectRelease(metadata, platform, "0.4.0", &error).isEmpty(), "platform asset");
        metadata["prerelease"] = true;
        check(AppUpdater::selectRelease(metadata, platform, "0.4.0", &error).isEmpty(),
              "prerelease rejected");
        metadata = release(platform);
        auto asset = metadata["assets"].toArray().first().toObject();
        asset["digest"] = "";
        metadata["assets"] = QJsonArray{asset};
        check(AppUpdater::selectRelease(metadata, platform, "0.4.0", &error).isEmpty(),
              "missing hash rejected");
        asset["digest"] = "sha256:" + QString(64, 'a');
        asset["browser_download_url"] = "https://example.com/archive.zip";
        metadata["assets"] = QJsonArray{asset};
        check(AppUpdater::selectRelease(metadata, platform, "0.4.0", &error).isEmpty(),
              "foreign URL rejected");
    }
    {
        Host preferences({}, true);
        // 构造阶段保持离线；只开放临时绿色目录内的真实配置读写，不启动账号或网络。
        preferences.offline = false;
        preferences.setPreference("auto-update", "off");
        check(preferences.preference("auto-update") == "off", "startup check can be disabled persistently");
        preferences.offline = true;
    }
    {
        Host preferences({}, true);
        preferences.offline = false;
        check(preferences.preference("auto-update") == "off", "startup setting survives host recreation");
        preferences.setPreference("auto-update", "on");
        check(preferences.preference("auto-update") == "on", "startup check can be enabled");
        preferences.offline = true;
    }
    Network network;
    AppUpdater offline(true, nullptr, &network);
    offline.check();
    check(network.requests == 0, "offline mode must not send requests");
    AppUpdater updater(false, nullptr, &network);
    updater.check();
    updater.check();
    check(network.requests == 1, "duplicate checks are coalesced");
    check(awaitIdle(updater) && updater.available(), "release check succeeds");
    updater.download();
    check(awaitIdle(updater) && !updater.downloadedPath().isEmpty(), "verified file committed");
    QFile file(updater.downloadedPath());
    check(file.open(QIODevice::ReadOnly) && file.readAll() == PACKAGE, "download bytes preserved");
    file.close();
    file.remove();
    network.package = QByteArray(PACKAGE.size(), 'x');
    updater.download();
    check(awaitIdle(updater) && updater.downloadedPath().isEmpty(), "hash mismatch discarded");
    check(QDir(directory.path() + "/data/updates").entryList(QDir::Files).isEmpty(), "no corrupt files");
    network.package = PACKAGE.left(PACKAGE.size() - 1);
    updater.download();
    check(awaitIdle(updater) && updater.downloadedPath().isEmpty(), "truncated package discarded");
    network.package = PACKAGE + "extra";
    updater.download();
    check(awaitIdle(updater) && updater.downloadedPath().isEmpty(), "oversized package discarded");
    updater.download();
    updater.cancel();
    check(!updater.busy() && updater.status().contains("取消"), "download cancellation");
    network.package = PACKAGE;
    updater.download();
    check(awaitIdle(updater) && !updater.downloadedPath().isEmpty(), "retry after cancellation");
    network.status = 403;
    updater.check();
    check(awaitIdle(updater) && updater.status().contains("拒绝") && !updater.status().contains("额度"),
          "ordinary 403 must not be mislabeled as rate limiting");
    network.status = 404;
    updater.check();
    check(awaitIdle(updater) && updater.status().contains("尚无"), "no releases yet");
    network.status = 200;
    network.responseOverride = "not json";
    updater.check();
    check(awaitIdle(updater) && updater.status().contains("无效"), "invalid JSON rejected");
    network.responseOverride.clear();
    network.metadata["tag_name"] = "v0.0.0";
    updater.check();
    check(awaitIdle(updater) && !updater.available() && updater.downloadedPath().isEmpty(),
          "old release clears stale update and downloaded file selection");
    network.metadata = release(AppUpdater::platformAsset());
    updater.check();
    updater.cancel();
    application.processEvents();
    check(!updater.available() && updater.status().contains("取消"),
          "cancelled check cannot publish late result");
    network.feedStatus = 404;
    network.metadata = release(AppUpdater::platformAsset());
    const auto beforeLegacy = network.requests;
    AppUpdater legacy(false, nullptr, &network);
    legacy.check();
    check(awaitIdle(legacy) && legacy.available() && network.requests == beforeLegacy + 2,
          "legacy release falls back from absent static feed to REST API");
    network.status = 403;
    network.headers.insert("X-RateLimit-Remaining", "0");
    network.headers.insert("X-RateLimit-Reset",
                           QByteArray::number(QDateTime::currentSecsSinceEpoch() + 3600));
    AppUpdater limited(false, nullptr, &network);
    limited.check();
    check(awaitIdle(limited) && limited.status().contains("网络出口") && limited.status().contains("后重试"),
          "shared IP rate limit reports reset time");
    const auto beforeRepeat = network.requests;
    limited.check();
    check(network.requests == beforeRepeat, "clicking during rate limit cooldown sends no request");
    network.feedStatus = 200;
    network.status = 429;
    network.headers.clear();
    network.headers.insert("Retry-After", "120");
    AppUpdater secondary(false, nullptr, &network);
    secondary.check();
    check(awaitIdle(secondary) && secondary.status().contains("额度"), "secondary rate limit recognized");
    network.status = 200;
    network.headers.clear();
    const auto beforeStatic = network.requests;
    AppUpdater staticFeed(false, nullptr, &network);
    staticFeed.check();
    check(awaitIdle(staticFeed) && staticFeed.available() && network.requests == beforeStatic + 1,
          "static feed succeeds without an API request");
    qInfo() << "App updater regressions:" << failures << "failures";
    return failures ? 1 : 0;
}
