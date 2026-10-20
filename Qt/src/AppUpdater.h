#pragma once
#include <QCryptographicHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSaveFile>
#include <memory>

// 更新网络与音乐账号完全隔离；只下载官方仓库的完整便携包，不修改运行中的应用。
class AppUpdater : public QObject {
    Q_OBJECT
  public:
    explicit AppUpdater(bool offline, QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    ~AppUpdater() override;
    void check();
    void download();
    void cancel();
    void openDownload();
    void openRelease();
    bool busy() const;
    QString status() const;
    QString latestVersion() const;
    QString downloadedPath() const;
    bool available() const;
    static QString currentVersion();
    static QString platformAsset();
    static bool isNewer(const QString &candidate, const QString &current);
    static QJsonObject selectRelease(const QJsonObject &release, const QString &platform,
                                     const QString &current, QString *error);

  signals:
    void changed();
    void updateAvailable();

  private:
    bool m_offline;
    QNetworkAccessManager m_defaultNetwork;
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QJsonObject m_release;
    QString m_status = "尚未检查更新";
    QString m_downloadedPath;
    std::unique_ptr<QSaveFile> m_file;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    qint64 m_received = 0;
    qint64 m_retryAtUnix = 0;
    void fail(const QString &message);
    void fetchRelease(bool useApi);
    QString responseError(QNetworkReply *reply, int code, const QByteArray &body);
    QString rateLimitStatus() const;
    void consumeDownload(QNetworkReply *reply);
};
