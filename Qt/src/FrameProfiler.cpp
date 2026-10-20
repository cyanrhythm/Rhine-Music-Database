#include "FrameProfiler.h"
#include "Engine.h"
#include "ProcessMetrics.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScreen>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QTimeZone>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <rhi/qrhi.h>
#include <QDateTime>
#include <QJsonArray>
#include <QQuickWindow>

/**
 * 复用原生计时与 Qt 渲染信号；关闭面板且没有录制时不运行采样定时器。
 * 命令行 CSV 保留原格式，用户诊断只记录白名单性能数值，不采集业务内容。
 */
FrameProfiler::FrameProfiler(Engine *engine, const QString &path, QObject *parent)
    : QObject(parent), m_engine(engine), file(path), stream(&file) {
    clock.start();
    epochMs = QDateTime::currentMSecsSinceEpoch();
    m_directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/RHINE Diagnostics";
    m_sampleTimer.setInterval(500);
    connect(&m_sampleTimer, &QTimer::timeout, this, &FrameProfiler::sample);
    connect(engine, &Engine::updated, this, &FrameProfiler::collectStep);
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (m_recording) {
            saveRecording();
        }
    });
    if (!path.isEmpty()) {
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            stream << "timestamp_ms,step_cpu_ms,scene_native_ms,scene_metadata_ms,scene_buffers_ms,"
                      "latest_present_gap_ms,latest_sync_cpu_ms,latest_submit_cpu_ms,presents,draws,"
                      "returning_cards,culled_cards,shadow_only_cards,latest_gpu_draw_calls\n";
        } else {
            qWarning() << "Cannot open frame profile:" << file.errorString();
        }
    }
    updateSampling();
}

/** 渲染线程只写原子计数与有界帧间隔队列，文件和业务状态始终由 GUI 线程访问。 */
void FrameProfiler::attach(QQuickWindow *window) {
    Q_ASSERT(!m_window);
    m_window = window;
    connect(
        window, &QQuickWindow::beforeSynchronizing, this,
        [this] {
            syncTimer.invalidate();
            if (m_enabled.load(std::memory_order_relaxed)) {
                syncTimer.start();
            }
        },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::afterSynchronizing, this,
        [this] {
            if (m_enabled.load(std::memory_order_relaxed) && syncTimer.isValid()) {
                syncNs.store(syncTimer.nsecsElapsed(), std::memory_order_relaxed);
            }
        },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::beforeRendering, this,
        [this] {
            renderTimer.invalidate();
            if (m_enabled.load(std::memory_order_relaxed)) {
                renderTimer.start();
                QMutexLocker lock(&m_gapMutex);
                if (m_adapterName.isEmpty() && m_window) {
                    auto *renderer = m_window->rendererInterface();
                    auto *rhi = static_cast<QRhi *>(
                        renderer->getResource(m_window, QSGRendererInterface::RhiResource));
                    if (rhi) {
                        m_adapterName = QString::fromUtf8(rhi->driverInfo().deviceName);
                    }
                }
            }
        },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::afterRendering, this,
        [this] {
            if (m_enabled.load(std::memory_order_relaxed) && renderTimer.isValid()) {
                submitNs.store(renderTimer.nsecsElapsed(), std::memory_order_relaxed);
            }
        },
        Qt::DirectConnection);
    connect(
        window, &QQuickWindow::frameSwapped, this,
        [this] {
            if (!m_enabled.load(std::memory_order_relaxed)) {
                return;
            }
            const auto now = clock.nsecsElapsed();
            const auto previous = presentNs.exchange(now, std::memory_order_relaxed);
            presentGapNs.store(previous ? now - previous : 0, std::memory_order_relaxed);
            presents.fetch_add(1, std::memory_order_relaxed);
            QMutexLocker lock(&m_gapMutex);
            if (previous && m_gapsMs.size() < 4096) {
                m_gapsMs.append((now - previous) / 1000000.);
            }
            if (m_capture.load(std::memory_order_relaxed)) {
                if (m_trace.size() < 36000) {
                    m_trace.append({now, previous ? now - previous : 0,
                                    syncNs.load(std::memory_order_relaxed),
                                    submitNs.load(std::memory_order_relaxed)});
                } else {
                    ++m_omittedTraceFrames;
                }
            }
        },
        Qt::DirectConnection);
    // 隐藏窗口暂停实时面板刷新；录制仍每秒两次采样，保留后台资源释放证据。
    connect(window, &QWindow::visibilityChanged, this, [this] { updateSampling(); });
    updateSampling();
}

void FrameProfiler::collectStep() {
    if (!m_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    const auto &timings = m_engine->scene.timings;
    ++m_stepCount;
    m_stepTotalMs += timings.engineMs;
    m_stepMaxMs = qMax(m_stepMaxMs, timings.engineMs);
    if (!file.isOpen()) {
        return;
    }
    // GUI 和渲染线程采样不同步，最近一次呈现间隔不是 GPU 耗时。
    const auto frame = m_engine->frame();
    stream << epochMs + clock.elapsed() << ',' << timings.engineMs << ',' << timings.simulateMs << ','
           << timings.metadataMs << ',' << timings.buffersMs << ','
           << presentGapNs.load(std::memory_order_relaxed) / 1000000. << ','
           << syncNs.load(std::memory_order_relaxed) / 1000000. << ','
           << submitNs.load(std::memory_order_relaxed) / 1000000. << ','
           << presents.load(std::memory_order_relaxed) << ',' << frame["draws"].toArray().size() << ','
           << frame["returningCards"].toInt() << ',' << frame["culledCards"].toInt() << ','
           << frame["shadowCards"].toInt() << ','
           << m_engine->performance().value("gpuDrawCalls").toULongLong() << '\n';
}

FrameProfiler::~FrameProfiler() {
    m_enabled.store(false, std::memory_order_relaxed);
    m_capture.store(false, std::memory_order_relaxed);
    if (file.isOpen()) {
        stream.flush();
    }
}

void FrameProfiler::toggleVisible() {
    m_visible = !m_visible;
    updateSampling();
    emit changed();
}

void FrameProfiler::toggleRecording() {
    if (m_recording) {
        saveRecording();
    } else if (!m_samples.isEmpty()) {
        saveRecording();
    } else {
        startRecording();
    }
}

void FrameProfiler::setReportDirectory(const QString &directory) {
    if (!m_recording) {
        m_directory = directory;
    }
}

void FrameProfiler::openReportFolder() {
    if (QDir().mkpath(m_directory)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_directory));
    }
}

/** 只在需要时开启原生计时，重置统计基线，避免把暂停期间的时间当成卡顿。 */
void FrameProfiler::updateSampling() {
    const bool shown = m_window && m_window->isVisible() && m_window->visibility() != QWindow::Minimized;
    const bool enabled = (m_visible && shown) || m_recording || file.isOpen();
    const bool previous = m_enabled.exchange(enabled, std::memory_order_relaxed);
    if (enabled != previous) {
        if (enabled) {
            m_previousProfiling = m_engine->scene.isProfiling();
            m_engine->scene.profileFrames(true);
            m_previousSampleNs = clock.nsecsElapsed();
            m_previousCpuNs = readProcessMetrics().cpuNanoseconds;
            m_previousPresents = presents.load(std::memory_order_relaxed);
            m_previousSceneFrames = m_engine->performance().value("renderFrames").toULongLong();
        } else {
            m_engine->scene.profileFrames(m_previousProfiling);
        }
        presentNs.store(0, std::memory_order_relaxed);
        QMutexLocker lock(&m_gapMutex);
        m_gapsMs.clear();
        m_stepCount = 0;
        m_stepTotalMs = m_stepMaxMs = 0;
    }
    // 纯命令行 CSV 沿用事件驱动写入，不额外唤醒窗口。
    if ((m_visible && shown) || m_recording) {
        m_sampleTimer.start();
    } else {
        m_sampleTimer.stop();
    }
}

/** 聚合半秒内的实际呈现事件，保留帧间隔尾部和峰值；采样本身不请求场景绘制。 */
void FrameProfiler::sample() {
    const auto now = clock.nsecsElapsed();
    const auto elapsedNs = qMax<qint64>(1, now - m_previousSampleNs);
    const auto process = readProcessMetrics();
    const auto count = presents.load(std::memory_order_relaxed);
    const auto resources = m_engine->performance();
    const auto sceneFrames = resources.value("renderFrames").toULongLong();
    QList<double> gaps;
    {
        QMutexLocker lock(&m_gapMutex);
        gaps.swap(m_gapsMs);
    }
    std::sort(gaps.begin(), gaps.end());
    const auto percentile = [&gaps](double fraction) {
        if (gaps.isEmpty()) {
            return 0.;
        }
        return gaps[qBound<qsizetype>(0, qsizetype(std::ceil(gaps.size() * fraction)) - 1, gaps.size() - 1)];
    };
    const auto over = [&gaps](double milliseconds) {
        return int(std::count_if(gaps.begin(), gaps.end(),
                                 [milliseconds](double gap) { return gap > milliseconds; }));
    };
    const double cpu = process.cpuNanoseconds >= 0 && m_previousCpuNs >= 0
                           ? qMax(0., (process.cpuNanoseconds - m_previousCpuNs) * 100. / elapsedNs)
                           : -1.;
    m_metrics.clear();
    for (const auto *key : {"idle", "sceneSleeping", "visible", "steps", "sceneUpdates", "hudUpdates",
                            "renderFrames", "gpuResources", "bindingCreations", "gpuMusicTextureBytes",
                            "coverUploads", "gpuDrawCalls", "timerActive"}) {
        m_metrics.insert(QString::fromLatin1(key), resources.value(QString::fromLatin1(key)));
    }
    m_metrics.insert("elapsedMs", clock.elapsed() - m_recordStartedMs);
    m_metrics.insert("sampleDurationMs", elapsedNs / 1000000.);
    m_metrics.insert("qtFps", (count - m_previousPresents) * 1000000000. / elapsedNs);
    m_metrics.insert("sceneFps", (sceneFrames - m_previousSceneFrames) * 1000000000. / elapsedNs);
    m_metrics.insert("presentCount", qulonglong(count));
    m_metrics.insert("gapSamples", gaps.size());
    m_metrics.insert("gapP50Ms", percentile(.5));
    m_metrics.insert("gapP95Ms", percentile(.95));
    m_metrics.insert("gapP99Ms", percentile(.99));
    m_metrics.insert("gapMaxMs", gaps.isEmpty() ? 0. : gaps.last());
    m_metrics.insert("over33ms", over(33.333));
    m_metrics.insert("over50ms", over(50));
    m_metrics.insert("over100ms", over(100));
    m_metrics.insert("mainProcessResidentBytes", process.residentBytes);
    m_metrics.insert("mainProcessCpuOneCorePercent", cpu);
    m_metrics.insert("stepSamples", qulonglong(m_stepCount));
    m_metrics.insert("stepMeanCpuMs", m_stepCount ? m_stepTotalMs / m_stepCount : 0.);
    m_metrics.insert("stepMaxCpuMs", m_stepMaxMs);
    m_metrics.insert("latestSyncCpuMs", syncNs.load(std::memory_order_relaxed) / 1000000.);
    m_metrics.insert("latestSubmitCpuMs", submitNs.load(std::memory_order_relaxed) / 1000000.);
    m_metrics.insert("gpuTimeMs", QVariant());
    {
        QMutexLocker lock(&m_gapMutex);
        m_metrics.insert("gpuAdapter", m_adapterName);
    }
    m_previousSampleNs = now;
    m_previousCpuNs = process.cpuNanoseconds;
    m_previousPresents = count;
    m_previousSceneFrames = sceneFrames;
    m_stepCount = 0;
    m_stepTotalMs = m_stepMaxMs = 0;
    formatMetrics();
    if (m_recording) {
        m_samples.append(QJsonObject::fromVariantMap(m_metrics));
        m_status =
            QStringLiteral("正在收集 %1 秒 · 最长 10 分钟").arg((clock.elapsed() - m_recordStartedMs) / 1000);
        if (!m_saving && (m_samples.size() >= 1200 || clock.elapsed() - m_recordStartedMs >= 600000)) {
            saveRecording();
        }
    }
    emit changed();
}

void FrameProfiler::formatMetrics() {
    const auto number = [this](const char *key, int precision = 1) {
        return QString::number(m_metrics.value(key).toDouble(), 'f', precision);
    };
    const bool shown = m_metrics.value("visible").toBool();
    const bool idle = m_metrics.value("idle").toBool();
    const auto residentBytes = m_metrics.value("mainProcessResidentBytes").toLongLong();
    const auto cpu = m_metrics.value("mainProcessCpuOneCorePercent").toDouble();
    m_headline =
        QStringLiteral("%1 FPS · %2")
            .arg(number("qtFps"), shown ? (idle ? QStringLiteral("按需休眠") : QStringLiteral("活动"))
                                        : QStringLiteral("后台"));
    const auto memory = residentBytes < 0 ? QStringLiteral("不可用")
                                          : QString::number(residentBytes / 1048576., 'f', 1) + " MiB";
    m_details =
        QStringLiteral("场景提交      %1 FPS\nQt 帧间隔 P50 / P95  %2 / %3 ms\n"
                       "P99 / 峰值    %4 / %5 ms\n>33 / 50 / 100 ms   %6 / %7 / %8\n"
                       "逻辑 CPU 均值 / 峰值  %9 / %10 ms\n同步 / 提交 CPU  %11 / %12 ms\n"
                       "主进程 CPU    %13%（单核=100%）\n主进程常驻内存  %14\n"
                       "Draw calls / GPU 资源  %15 / %16\n封面 GPU 纹理  %17 MiB\n"
                       "GPU 耗时 / 占用  未采集\n"
                       "Qt FPS 含面板刷新；帧间隔含按需停绘。\n休眠时低帧率和长间隔不代表卡顿。\n内存与 CPU "
                       "不含 Node 桥接子进程。")
            .arg(number("sceneFps"), number("gapP50Ms"), number("gapP95Ms"), number("gapP99Ms"))
            .arg(number("gapMaxMs"), number("over33ms", 0), number("over50ms", 0), number("over100ms", 0))
            .arg(number("stepMeanCpuMs", 2), number("stepMaxCpuMs", 2), number("latestSyncCpuMs", 2),
                 number("latestSubmitCpuMs", 2))
            .arg(cpu < 0 ? QStringLiteral("不可用") : number("mainProcessCpuOneCorePercent"), memory,
                 number("gpuDrawCalls", 0), number("gpuResources", 0))
            .arg(QString::number(m_metrics.value("gpuMusicTextureBytes").toDouble() / 1048576., 'f', 1));
    const auto adapter = m_metrics.value("gpuAdapter").toString();
    if (!adapter.isEmpty()) {
        m_details.prepend(QStringLiteral("GPU 设备  %1\n").arg(adapter));
    }
}

QJsonObject FrameProfiler::environment() const {
    QString backend = QStringLiteral("unknown");
    if (m_window) {
        switch (m_window->rendererInterface()->graphicsApi()) {
        case QSGRendererInterface::Metal:
            backend = "Metal";
            break;
        case QSGRendererInterface::Direct3D11:
            backend = "Direct3D11";
            break;
        case QSGRendererInterface::Direct3D12:
            backend = "Direct3D12";
            break;
        case QSGRendererInterface::Vulkan:
            backend = "Vulkan";
            break;
        case QSGRendererInterface::OpenGL:
            backend = "OpenGL";
            break;
        case QSGRendererInterface::Software:
            backend = "Software";
            break;
        default:
            break;
        }
    }
    return {{"os", QSysInfo::prettyProductName()},
            {"architecture", QSysInfo::currentCpuArchitecture()},
            {"qt", qVersion()},
            {"logicalCpuCount", QThread::idealThreadCount()},
            {"graphicsApi", backend},
            {"gpuAdapter", m_metrics.value("gpuAdapter").toString()},
            {"windowWidth", m_window ? m_window->width() : 0},
            {"windowHeight", m_window ? m_window->height() : 0},
            {"devicePixelRatio", m_window ? m_window->devicePixelRatio() : 0},
            {"screenRefreshHz", m_window && m_window->screen() ? m_window->screen()->refreshRate() : 0}};
}

/** 手动开启有界采集；与界面可见性解耦，便于记录最小化后的内存释放。 */
bool FrameProfiler::startRecording() {
    if (m_recording || !m_samples.isEmpty()) {
        return false;
    }
    if (!QDir().mkpath(m_directory)) {
        m_status = QStringLiteral("无法创建诊断目录，请检查文稿目录权限。");
        emit changed();
        return false;
    }
    // 丢弃打开面板时已有的半个采样区间，让录制从用户按下按钮时开始。
    sample();
    m_recordStartedMs = clock.elapsed();
    m_recordStoppedMs = 0;
    presentNs.store(0, std::memory_order_relaxed);
    {
        QMutexLocker lock(&m_gapMutex);
        m_trace.clear();
        m_omittedTraceFrames = 0;
        m_capture.store(true, std::memory_order_relaxed);
    }
    m_recording = true;
    m_status = QStringLiteral("正在收集，请复现卡顿后停止并导出。");
    updateSampling();
    emit changed();
    return true;
}

/** 原子保存有界数值采样；失败时保留内存中的记录，允许用户再次导出。 */
bool FrameProfiler::saveRecording() {
    if (!m_recording && m_samples.isEmpty()) {
        return false;
    }
    if (m_saving) {
        return false;
    }
    m_saving = true;
    if (m_recording) {
        sample();
        m_recordStoppedMs = clock.elapsed();
    }
    m_recording = false;
    QJsonArray trace;
    quint64 omitted = 0;
    {
        QMutexLocker lock(&m_gapMutex);
        m_capture.store(false, std::memory_order_relaxed);
        omitted = m_omittedTraceFrames;
        for (const auto &frame : m_trace) {
            trace.append(QJsonArray{(frame.timestampNs / 1000000.) - m_recordStartedMs,
                                    frame.gapNs / 1000000., frame.syncCpuNs / 1000000.,
                                    frame.submitCpuNs / 1000000.});
        }
    }
    const auto name = "rhine-performance-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") +
                      "-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json";
    QSaveFile report(QDir(m_directory).filePath(name));
    const QJsonObject document{
        {"schema", 1},
        {"startedUtc", QDateTime::fromMSecsSinceEpoch(epochMs + m_recordStartedMs, QTimeZone::UTC)
                           .toString(Qt::ISODateWithMs)},
        {"durationMs", m_recordStoppedMs - m_recordStartedMs},
        {"environment", environment()},
        {"notes", "Qt FPS includes overlay repaints. Gaps include idle intervals. CPU is main-process "
                  "single-core percent; RSS excludes Node. Sync/submit are CPU wall time, not GPU time. No "
                  "account, request, track, screenshot, or environment-variable content is collected."},
        {"samples", m_samples},
        {"frameColumns", QJsonArray{"elapsedMs", "presentGapMs", "syncCpuMs", "submitCpuMs"}},
        {"frames", trace},
        {"omittedTraceFrames", qint64(omitted)}};
    const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Compact);
    if (!report.open(QIODevice::WriteOnly) || report.write(bytes) != bytes.size() || !report.commit()) {
        m_status = QStringLiteral("导出失败，记录已保留；点击“重试导出”。%1").arg(report.errorString());
        m_saving = false;
        updateSampling();
        emit changed();
        return false;
    }
    m_saving = false;
    m_samples = {};
    {
        QMutexLocker lock(&m_gapMutex);
        QList<FrameEvent>().swap(m_trace);
    }
    m_status = QStringLiteral("已保存：%1").arg(QDir::toNativeSeparators(report.fileName()));
    updateSampling();
    emit changed();
    return true;
}
