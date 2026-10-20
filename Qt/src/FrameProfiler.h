#pragma once
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QTextStream>
#include <QTimer>
#include <QVariantMap>
#include <atomic>

class Engine;
class QQuickWindow;

class FrameProfiler : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool visible READ visible NOTIFY changed)
    Q_PROPERTY(bool recording READ recording NOTIFY changed)
    Q_PROPERTY(QString headline READ headline NOTIFY changed)
    Q_PROPERTY(QString details READ details NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString recordingAction READ recordingAction NOTIFY changed)
    Q_PROPERTY(QVariantMap metrics READ metrics NOTIFY changed)
  public:
    explicit FrameProfiler(Engine *engine, const QString &path = {}, QObject *parent = nullptr);
    ~FrameProfiler() override;
    void attach(QQuickWindow *window);
    bool visible() const {
        return m_visible;
    }
    bool recording() const {
        return m_recording;
    }
    QString headline() const {
        return m_headline;
    }
    QString details() const {
        return m_details;
    }
    QString status() const {
        return m_status;
    }
    QString recordingAction() const {
        if (m_recording) {
            return QStringLiteral("■ 停止并导出");
        }
        return m_samples.isEmpty() ? QStringLiteral("开始收集") : QStringLiteral("重试导出");
    }
    QVariantMap metrics() const {
        return m_metrics;
    }
    Q_INVOKABLE void toggleVisible();
    Q_INVOKABLE void toggleRecording();
    Q_INVOKABLE void openReportFolder();
    bool startRecording();
    bool saveRecording();
    // 测试和原生命令行入口可指定目录；正常界面使用文稿目录。
    void setReportDirectory(const QString &directory);
  signals:
    void changed();

  private:
    void updateSampling();
    void sample();
    void collectStep();
    void formatMetrics();
    QJsonObject environment() const;
    Engine *m_engine;
    QPointer<QQuickWindow> m_window;
    bool m_visible = false;
    bool m_recording = false;
    bool m_saving = false;
    bool m_previousProfiling = false;
    struct FrameEvent {
        qint64 timestampNs;
        qint64 gapNs;
        qint64 syncCpuNs;
        qint64 submitCpuNs;
    };
    QList<FrameEvent> m_trace;
    quint64 m_omittedTraceFrames = 0;
    QString m_adapterName;
    std::atomic<bool> m_capture{false};
    QString m_headline = QStringLiteral("等待采样");
    QString m_details;
    QString m_status;
    QString m_directory;
    QVariantMap m_metrics;
    QJsonArray m_samples;
    QTimer m_sampleTimer;
    qint64 m_recordStartedMs = 0;
    qint64 m_recordStoppedMs = 0;
    qint64 m_previousSampleNs = 0;
    qint64 m_previousCpuNs = -1;
    quint64 m_previousPresents = 0;
    quint64 m_previousSceneFrames = 0;
    quint64 m_stepCount = 0;
    double m_stepTotalMs = 0;
    double m_stepMaxMs = 0;
    QFile file;
    QTextStream stream;
    QElapsedTimer clock;
    QElapsedTimer syncTimer;
    QElapsedTimer renderTimer;
    std::atomic<bool> m_enabled{false};
    std::atomic<qint64> syncNs{0};
    std::atomic<qint64> submitNs{0};
    std::atomic<qint64> presentNs{0};
    std::atomic<qint64> presentGapNs{0};
    std::atomic<quint64> presents{0};
    QMutex m_gapMutex;
    QList<double> m_gapsMs;
    qint64 epochMs = 0;
};
