#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTemporaryDir>
#include <atomic>
#include <memory>

/** 本地索引 */
class LocalMusicLibrary : public QObject {
    Q_OBJECT
  public:
    explicit LocalMusicLibrary(QString storage = {}, bool temporary = false, QObject *parent = nullptr);
    ~LocalMusicLibrary() override;
    void importPaths(const QStringList &paths);
    void rescan();
    void cancel();
    void removeRoot(int index);
    QJsonArray roots() const;
    QJsonArray songs() const;
    QJsonArray collections(QString kind) const;
    QJsonArray tracks(const QJsonObject &collection) const;
    QJsonArray search(QString query, QString kind) const;
    bool contains(const QString &path) const;
    bool busy = false;
    QString status;

  signals:
    void changed();

  private:
    std::unique_ptr<QTemporaryDir> m_temporary;
    QString m_storage;
    QJsonObject m_index;
    quint64 m_generation = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
    void scan(QJsonArray roots);
    bool save(const QJsonObject &index);
};
