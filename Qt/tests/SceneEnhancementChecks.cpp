#include "Engine.h"
#include "MusicData.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickItem>
#include <QQuickRhiItem>
#include <QQuickWindow>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
void settleEnhancement() {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 200) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }
}

QPair<qint64, int> pixelDifference(const QImage &firstImage, const QImage &secondImage) {
    if (firstImage.isNull() || firstImage.size() != secondImage.size()) {
        return {-1, 256};
    }
    qint64 changed = 0;
    int maximum = 0;
    const auto first = firstImage.convertToFormat(QImage::Format_RGBA8888);
    const auto second = secondImage.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < first.height(); ++y) {
        const auto firstRow = first.constScanLine(y);
        const auto secondRow = second.constScanLine(y);
        for (int x = 0; x < first.width(); ++x) {
            bool different = false;
            for (int channel = 0; channel < 4; ++channel) {
                const auto delta = std::abs(int(firstRow[x * 4 + channel]) - int(secondRow[x * 4 + channel]));
                different |= delta != 0;
                maximum = std::max(maximum, delta);
            }
            changed += different;
        }
    }
    return {changed, maximum};
}
}

/**
 * 用真实窗口切换可选增强，严格比较关闭前后的像素及场景数据。
 * 检查独立 Loader 的销毁、主题、尺寸、后台重建和原生设置命中；全部使用离线夹具。
 */
bool runSceneEnhancementChecks(Engine &backend, QQuickWindow *window, const QString &output) {
    auto &hud = backend.app->hud;
    auto &music = backend.app->music;
    bool ok = true;
    QJsonArray results;
    const auto check = [&](bool value, const char *name) {
        results.append(QJsonObject{{"check", name}, {"passed", value}});
        if (!value) {
            qCritical() << "Scene enhancement regression:" << name;
        }
        ok &= value;
    };
    const auto refresh = [&] {
        music.selectedPhysical = backend.scene.selection();
        backend.step(90);
        settleEnhancement();
    };
    const auto capture = [&](const QString &name) {
        const auto image = window->grabWindow();
        if (!output.isEmpty()) {
            QDir().mkpath(output);
            image.save(output + "/" + name + ".png");
        }
        return image;
    };
    const auto toggle = [&] {
        backend.action("scene-enhancement");
        refresh();
    };
    const auto originalSize = window->size();
    // 冻结画面时也冻结自动视觉时钟，避免主题波在手动时间与真实计时之间往返。
    backend.setVisible(false);
    {
        NativeHud defaultHud(&backend.host, backend.glyphs(), &music, {});
        check(defaultHud.sceneEnhancement, "RhineLab Enhance defaults to enabled without a saved override");
    }
    const bool transitionBenchmark = qEnvironmentVariableIsSet("RHINE_TRANSITION_BENCHMARK");
    if (hud.sceneEnhancement && !transitionBenchmark) {
        toggle();
    }
    auto *loader = window->findChild<QQuickItem *>("sceneEnhancementLoader");
    check(transitionBenchmark || (loader && !hud.sceneEnhancement && !loader->property("active").toBool() &&
                                  !loader->property("item").value<QObject *>()),
          "explicitly disabled enhancement has no active instance");
    hud.modal.clear();
    hud.closing = false;
    hud.discRotation = false;
    hud.trackListMode = false;
    hud.reduced = true;
    hud.tab = "overview";
    music.player.stop();
    music.directory.reset();
    music.provider = "netease";
    music.api.provider = "netease";
    music.current = MusicData::item("playlist", "1001", "RHINE LAB / 音乐档案", "本地视觉验收");
    music.status = "离线画面增强验证";
    music.coverImage = "enhancement-cover";
    QImage cover(1024, 1024, QImage::Format_RGBA8888);
    cover.fill(QColor("#56645c"));
    {
        QPainter painter(&cover);
        painter.setPen(QPen(QColor("#e3d9b4"), 5));
        painter.drawEllipse(QPointF(512, 512), 390, 390);
        painter.drawEllipse(QPointF(512, 512), 56, 56);
    }
    backend.host.fixtureImage(music.coverImage, cover);

    if (transitionBenchmark) {
        hud.sceneEnhancement = true;
        hud.reduced = false;
        backend.scene.review("archive");
        backend.scene.freeze(std::nullopt);
        backend.scene.reduced(false);
        backend.scene.energyIdle(true);
        std::atomic<quint64> presentations{0};
        const auto connection = QObject::connect(
            window, &QQuickWindow::frameSwapped, window,
            [&] { presentations.fetch_add(1, std::memory_order_relaxed); }, Qt::DirectConnection);
        double timeSeconds = 100;
        const auto present = [&] {
            const auto previous = presentations.load(std::memory_order_relaxed);
            QElapsedTimer elapsed;
            elapsed.start();
            timeSeconds += 1.0 / 60.0;
            backend.step(timeSeconds);
            window->update();
            while (presentations.load(std::memory_order_relaxed) == previous && elapsed.elapsed() < 2000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            return elapsed.nsecsElapsed() / 1000000.0;
        };
        for (int index = 0; index < 30; ++index) {
            present();
        }
        QJsonArray measurements;
        for (int cycle = 0; cycle < 2; ++cycle) {
            for (const QString mode : {QString("detail"), QString("archive")}) {
                const auto before = backend.performance();
                backend.scene.setMode(mode);
                QList<double> intervals;
                int hybridFrames = 0;
                for (int index = 0; index < 300; ++index) {
                    intervals.append(present());
                    hybridFrames += backend.frame()["originalActorBlend"].toDouble() > 0;
                }
                std::sort(intervals.begin(), intervals.end());
                const auto after = backend.performance();
                const auto newBindings =
                    after["bindingCreations"].toULongLong() - before["bindingCreations"].toULongLong();
                if (mode == "archive") {
                    check(newBindings == 0, "return animation reuses existing record bindings");
                }
                measurements.append(QJsonObject{
                    {"cycle", cycle},
                    {"mode", mode},
                    {"presentedFrames", intervals.size()},
                    {"hybridFrames", hybridFrames},
                    {"rendererFrames",
                     qint64(after["renderFrames"].toULongLong() - before["renderFrames"].toULongLong())},
                    {"newBindings", qint64(after["bindingCreations"].toULongLong() -
                                           before["bindingCreations"].toULongLong())},
                    {"presentRoundTripP50Ms", intervals[intervals.size() / 2]},
                    {"presentRoundTripP95Ms", intervals[int(intervals.size() * 0.95)]},
                    {"presentRoundTripMaxMs", intervals.last()}});
            }
        }
        QObject::disconnect(connection);
        QDir().mkpath(output);
        QFile file(output + "/transition-benchmark.json");
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(measurements).toJson());
        }
        return ok;
    }

    if (qEnvironmentVariableIsSet("RHINE_ENHANCEMENT_BENCHMARK")) {
        QJsonArray measurements;
        backend.scene.review("archive");
        backend.scene.setMode("archive");
        backend.scene.reduced(false);
        backend.scene.energyIdle(false);
        std::atomic<quint64> presentations{0};
        const auto connection = QObject::connect(
            window, &QQuickWindow::frameSwapped, window,
            [&] { presentations.fetch_add(1, std::memory_order_relaxed); }, Qt::DirectConnection);
        double timeSeconds = 100;
        for (const bool enhanced : {false, true}) {
            if (hud.sceneEnhancement != enhanced) {
                toggle();
            }
            QList<double> intervals;
            for (int index = 0; index < 150; ++index) {
                if (index % 15 == 0) {
                    backend.scene.navigate("row", (index / 15) % 2 == 0 ? 1 : -1);
                }
                const auto previous = presentations.load(std::memory_order_relaxed);
                QElapsedTimer elapsed;
                elapsed.start();
                timeSeconds += 1.0 / 60.0;
                backend.step(timeSeconds);
                window->update();
                while (presentations.load(std::memory_order_relaxed) == previous &&
                       elapsed.elapsed() < 2000) {
                    QCoreApplication::processEvents();
                    QThread::msleep(1);
                }
                if (index >= 30) {
                    intervals.append(elapsed.nsecsElapsed() / 1000000.0);
                }
            }
            std::sort(intervals.begin(), intervals.end());
            auto performance = QJsonObject::fromVariantMap(backend.performance());
            performance["enhanced"] = enhanced;
            performance["presentRoundTripP50Ms"] = intervals[intervals.size() / 2];
            performance["presentRoundTripP95Ms"] = intervals[int(intervals.size() * 0.95)];
            measurements.append(performance);
        }
        QObject::disconnect(connection);
        QDir().mkpath(output);
        QFile file(output + "/navigation-benchmark.json");
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(measurements).toJson());
        }
        backend.scene.captureOnly(false);
        backend.setVisible(true);
        return true;
    }

    const auto compareMode = [&](const QString &name, const QString &mode, bool dark) {
        // 歌曲详情必须使用歌曲夹具，避免歌单面板遮住唱片而产生无效的像素对照。
        music.current = MusicData::item(mode == "detail" ? "song" : "playlist", "1001",
                                        "RHINE LAB / 音乐档案", "本地视觉验收");
        backend.scene.captureOnly(false);
        backend.scene.review(mode);
        backend.scene.reduced(true);
        backend.scene.setMode(mode);
        hud.dark = dark;
        hud.themeMode = dark ? "dark" : "light";
        backend.scene.theme(dark);
        backend.scene.energyIdle(true);
        // 先推进真实主题波，让暗色落到每一个实例，而不是只改变 HUD 和全局背景。
        for (int index = 1; index <= 120; ++index) {
            backend.scene.sample(2.101 + index / 60.0);
        }
        backend.scene.captureOnly(true);
        backend.scene.theme(dark);
        refresh();
        const auto before = capture(name + "-original");
        const auto frame = backend.frame();
        const auto buffers = backend.drawBuffers();
        const auto commands = backend.commands();
        const auto resources = backend.performance().value("gpuResources");
        toggle();
        const auto enhanced = capture(name + "-enhanced");
        auto *composite = window->findChild<QQuickItem *>("enhancementComposite");
        check(hud.sceneEnhancement && loader && loader->property("active").toBool() && composite,
              "enabled setting creates the independent compositor");
        check(!before.isNull() && before.size() == enhanced.size() && before != enhanced,
              "enabled enhancement changes actual window pixels");
        check(composite && composite->inherits("OpticalScene"),
              "enhancement uses a separate native optical renderer");
        if (mode == "detail") {
            const QRect actorRegion(enhanced.width() * 0.18, enhanced.height() * 0.40,
                                    enhanced.width() * 0.22, enhanced.height() * 0.24);
            const auto difference = pixelDifference(before.copy(actorRegion), enhanced.copy(actorRegion));
            qInfo() << "[enhancement]" << name << "original actor changed pixels" << difference.first
                    << "maximum" << difference.second;
            check(difference.first == 0, "extracted record preserves the original renderer pixels");
            if (auto *renderItem = qobject_cast<QQuickRhiItem *>(composite)) {
                renderItem->update();
                settleEnhancement();
                check(backend.performance()["gpuDrawCalls"].toULongLong() == 1 &&
                          capture(name + "-cached") == enhanced,
                      "unchanged extracted background uses only composite and preserves exact pixels");
                renderItem->setProperty("lightingEnabled", false);
                settleEnhancement();
                check(capture(name + "-unlit") != enhanced,
                      "lighting changes invalidate the cached extracted background");
                renderItem->setProperty("lightingEnabled", true);
                settleEnhancement();
                check(capture(name + "-relit") == enhanced,
                      "restoring lighting rebuilds the same extracted background pixels");
            }
        }
        if (name == "archive-light" && composite) {
            composite->setProperty("lightingEnabled", false);
            settleEnhancement();
            const auto unlit = capture("archive-light-emitter-disabled");
            const QRect neighborRegion(enhanced.width() * 0.06, enhanced.height() * 0.52,
                                       enhanced.width() * 0.18, enhanced.height() * 0.18);
            check(enhanced.copy(neighborRegion) != unlit.copy(neighborRegion),
                  "selected emitter changes illumination on neighboring shells away from the emitter");
            composite->setProperty("lightingEnabled", true);
            settleEnhancement();
        }
        auto beforeFrame = frame;
        auto afterFrame = backend.frame();
        for (const auto &key : {"time", "boot"}) {
            beforeFrame.remove(key);
            afterFrame.remove(key);
        }
        check(beforeFrame == afterFrame && commands == backend.commands(),
              "enhancement leaves scene metadata and HUD commands unchanged");
        const auto currentBuffers = backend.drawBuffers();
        bool sameBuffers = buffers.size() == currentBuffers.size();
        for (auto entry = buffers.begin(); entry != buffers.end(); ++entry) {
            const auto current = currentBuffers.value(entry.key());
            sameBuffers &= entry->matrices == current.matrices && entry->themes == current.themes;
        }
        check(sameBuffers, "enhancement leaves geometry and material state bytes unchanged");
        toggle();
        const auto restored = capture(name + "-restored");
        const auto restoredDifference = pixelDifference(before, restored);
        qInfo() << "[enhancement]" << name << "restored changed pixels" << restoredDifference.first
                << "maximum" << restoredDifference.second;
        check(before == restored, "disabling enhancement restores every original pixel exactly");
        check(loader && !loader->property("item").value<QObject *>() &&
                  !window->findChild<QQuickItem *>("enhancementComposite"),
              "disabling enhancement destroys its scene graph owners");
        check(resources == backend.performance().value("gpuResources"),
              "original renderer resource count is unchanged");
    };
    compareMode("archive-light", "archive", false);
    compareMode("detail-light", "detail", false);
    compareMode("archive-dark", "archive", true);
    compareMode("detail-dark", "detail", true);
    window->resize(1152, 720);
    refresh();
    compareMode("archive-16x10", "archive", false);
    window->resize(originalSize);
    refresh();

    toggle();
    backend.setVisible(true);
    QElapsedTimer idle;
    idle.start();
    while (!backend.performance().value("idle").toBool() && idle.elapsed() < 10000) {
        settleEnhancement();
    }
    check(backend.performance().value("idle").toBool(), "enabled enhancement allows the scheduler to idle");
    std::atomic<quint64> presents{0};
    const auto presentation = QObject::connect(
        window, &QQuickWindow::frameSwapped, window,
        [&] { presents.fetch_add(1, std::memory_order_relaxed); }, Qt::DirectConnection);
    settleEnhancement();
    settleEnhancement();
    settleEnhancement();
    QObject::disconnect(presentation);
    check(presents.load(std::memory_order_relaxed) == 0,
          "enabled enhancement does not schedule continuous scene graph rendering at idle");
    const auto beforeNavigation = capture("enhanced-navigation-before");
    const auto previousSelection = backend.scene.selection();
    backend.scene.captureOnly(false);
    backend.scene.navigate("row", 1);
    backend.host.invalidate();
    for (int index = 0; index < 4; ++index) {
        settleEnhancement();
    }
    const auto afterNavigation = capture("enhanced-navigation-after");
    const QRect modelRegion(0, beforeNavigation.height() * 0.22, beforeNavigation.width() * 0.40,
                            beforeNavigation.height() * 0.45);
    check(backend.scene.selection() != previousSelection &&
              beforeNavigation.copy(modelRegion) != afterNavigation.copy(modelRegion),
          "enabled enhancement follows live scene navigation instead of freezing its source texture");
    backend.scene.captureOnly(true);
    backend.setVisible(false);
    toggle();
    compareMode("archive-after-navigation", "archive", false);

    // 按钮由真正的 QML 输入层触发，避免只调用内部状态而漏掉设置页连接。
    hud.openModal("settings");
    hud.settingsScroll = 0;
    refresh();
    capture("settings-enhancement");
    bool clicked = false;
    for (const auto &value : backend.hits()) {
        const auto hit = value.toMap();
        if (hit.value("action") != "scene-enhancement") {
            continue;
        }
        const auto scale = window->height() / 1080.;
        const QPointF point(
            (hit.value("x").toDouble() + hit.value("width").toDouble() / 2) * scale,
            (hit.value("y").toDouble() + hit.value("height").toDouble() / 2 + hud.motion.offset) * scale);
        for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, point, point, window->mapToGlobal(point), Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(window, &event);
        }
        clicked = true;
        break;
    }
    refresh();
    check(clicked && hud.sceneEnhancement && backend.state().value("sceneEnhancement").toBool(),
          "settings button enables enhancement through native QML input");
    hud.modal.clear();
    refresh();
    capture("enhanced-before-settle");
    settleEnhancement();
    const auto beforeHide = capture("enhanced-before-hide");
    const auto beforeHideFrame = backend.frame();
    const auto beforeHideCommands = backend.commands();
    window->hide();
    QElapsedTimer hidden;
    hidden.start();
    while (hidden.elapsed() < 2500) {
        settleEnhancement();
    }
    check(!backend.performance().value("timerActive").toBool(),
          "enhancement does not keep the hidden visual timer awake");
    window->show();
    backend.setVisible(false);
    settleEnhancement();
    refresh();
    const auto afterHide = capture("enhanced-after-hide");
    auto firstHiddenFrame = beforeHideFrame;
    auto secondHiddenFrame = backend.frame();
    for (const auto &key : {"time", "boot"}) {
        firstHiddenFrame.remove(key);
        secondHiddenFrame.remove(key);
    }
    check(firstHiddenFrame == secondHiddenFrame && beforeHideCommands == backend.commands(),
          "enhanced background recreation preserves the exact frozen scene and HUD state");
    const auto rebuiltDifference = pixelDifference(beforeHide, afterHide);
    qInfo() << "[enhancement] rebuilt changed pixels" << rebuiltDifference.first << "maximum"
            << rebuiltDifference.second;
    // 光学分支的折射与 HDR 会放大少量导数舍入；仅设备重建允许千分之一像素内的小幅差异。
    // 上面的关闭恢复继续要求逐像素零差异，不能借用新渲染器的重建容差。
    check(rebuiltDifference.first >= 0 &&
              rebuiltDifference.first <= qint64(beforeHide.width()) * beforeHide.height() / 1000 &&
              rebuiltDifference.second <= 16,
          "enhanced graphics recreation stays within the bounded RGB8 rounding budget");
    toggle();
    capture("final-original");
    backend.scene.captureOnly(false);
    backend.setVisible(true);
    if (!output.isEmpty()) {
        QFile report(output + "/checks.json");
        if (report.open(QIODevice::WriteOnly)) {
            report.write(QJsonDocument(QJsonObject{{"passed", ok}, {"checks", results}}).toJson());
        }
    }
    qInfo() << "Optional scene enhancement checks:" << (ok ? "passed" : "FAILED");
    return ok;
}
