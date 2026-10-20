#include "Engine.h"
#include "FrameProfiler.h"
#include "MusicData.h"
#include "MusicDiscover.h"
#include "RhiScene.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QWheelEvent>
#include <QtEndian>
#include <functional>
#include <algorithm>
#include <cmath>

bool runSceneEnhancementChecks(Engine &backend, QQuickWindow *window, const QString &output);

namespace {
/**
 * GPU 资源重建后只允许极少像素出现一个 RGB8 量化单位的舍入差异。
 * 尺寸、透明度及明显颜色差异仍严格失败；帧数据、HUD 命令和矩阵另行逐字节验证。
 */
bool equivalentGpuPixels(const QImage &expected, const QImage &actual, int &changedPixels,
                         qint64 pixelsPerDifference = 100000) {
    changedPixels = 0;
    if (expected.isNull() || expected.size() != actual.size()) {
        return false;
    }
    if (expected == actual) {
        return true;
    }
    const auto first = expected.convertToFormat(QImage::Format_RGBA8888);
    const auto second = actual.convertToFormat(QImage::Format_RGBA8888);
    const auto limit = qMax<qint64>(1, qint64(first.width()) * first.height() / pixelsPerDifference);
    for (int y = 0; y < first.height(); ++y) {
        const auto before = first.constScanLine(y);
        const auto after = second.constScanLine(y);
        for (int x = 0; x < first.width(); ++x) {
            bool changed = false;
            for (int channel = 0; channel < 4; ++channel) {
                const int difference = std::abs(int(before[x * 4 + channel]) - int(after[x * 4 + channel]));
                if (difference > 1 || (channel == 3 && difference != 0)) {
                    return false;
                }
                changed |= difference != 0;
            }
            if (changed && ++changedPixels > limit) {
                return false;
            }
        }
    }
    return true;
}
}

/** 离线 GPU 基准复用逐通道 RGB8 检查，并限制独立设备之间的舍入像素数量。 */
bool runGpuReferenceCheck(const QImage &expected, const QImage &actual) {
    int changedPixels = 0;
    // 两个独立 QRhi 设备允许最多万分之一像素出现一个 RGB8 单位的舍入，透明度仍须精确相同。
    // 窗口资源重建继续使用默认的十万分之一预算；场景数据和矩阵另外严格对照。
    const auto same = equivalentGpuPixels(expected, actual, changedPixels, 10000);
    if (changedPixels > 0) {
        qInfo() << "GPU reference RGB8 rounding pixels:" << changedPixels;
    }
    const auto output = qEnvironmentVariable("RHINE_GPU_DIFF");
    if (!same && !output.isEmpty()) {
        QDir().mkpath(output);
        expected.save(output + "/expected.png");
        actual.save(output + "/actual.png");
    }
    return same;
}

/** 用真实 QML 输入和 QRhi 窗口验证原生业务，全部数据来自本地夹具。 */
int runUiChecks(Engine &backend, QQuickWindow *window, const QString &output, FrameProfiler &profiler) {
    if (!window || !backend.ready() || !backend.scene.review("archive")) {
        return 1;
    }
    if (qEnvironmentVariableIntValue("RHINE_ENHANCEMENT_ONLY") == 1) {
        return runSceneEnhancementChecks(backend, window, output) ? 0 : 1;
    }
    auto &music = backend.app->music;
    auto &hud = backend.app->hud;
    // 既有界面与 GPU 基线继续显式验收原版；默认开启与增强模式另有真实窗口检查。
    hud.sceneEnhancement = false;
    backend.step(50);
    auto &player = music.player;
    bool ok = true;
    int failures = 0;
    const auto check = [&](bool value, const char *name) {
        if (!value) {
            qCritical() << "UI regression:" << name;
            ++failures;
        }
        ok &= value;
    };
    const auto settle = [] {
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 150) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
    };
    QElapsedTimer wait;
    QJsonArray songs;
    for (int index = 1; index <= 40; ++index) {
        auto song = MusicData::item("song", QString::number(index), "本地测试曲目 " + QString::number(index),
                                    "测试歌手");
        song["album"] = "测试专辑";
        song["albumID"] = "7";
        song["duration"] = 180000;
        song["wiki"] = "本地验证";
        songs.append(song);
        music.details.insert(song["key"].toString(), song);
    }
    QJsonArray lyricLines;
    const QStringList lyricTexts{"风穿过安静的街道", "我们听见远处的回声", "每一束光都落在这里",
                                 "让这首歌继续流动"};
    for (int index = 0; index < lyricTexts.size(); ++index) {
        lyricLines.append(QJsonObject{{"startMs", index * 1000},
                                      {"endMs", QJsonValue::Null},
                                      {"text", lyricTexts.at(index)},
                                      {"translation", ""},
                                      {"words", QJsonArray{}}});
    }
    const QJsonObject lyrics{{"version", 1},  {"source", "netease"}, {"timing", "line"},
                             {"offsetMs", 0}, {"plainText", ""},     {"lines", lyricLines}};
    QString fixtureAudioURL;
    backend.host.fixture = [&](QString route, QJsonObject parameters) {
        if (route.endsWith("/playback") && !fixtureAudioURL.isEmpty()) {
            return QJsonObject{{"body", QJsonObject{{"code", 200},
                                                    {"value", QJsonObject{{"url", fixtureAudioURL},
                                                                          {"level", "standard"}}}}}};
        }
        if (route.endsWith("/lyrics")) {
            return QJsonObject{{"body", QJsonObject{{"code", 200}, {"value", lyrics}}}};
        }
        if (route.endsWith("/detail")) {
            const auto item = parameters["args"].toArray().at(0).toObject();
            return QJsonObject{
                {"body",
                 QJsonObject{{"code", 200}, {"value", QJsonArray{item, QJsonArray{}, QJsonArray{}}}}}};
        }
        return QJsonObject{{"body", QJsonObject{{"code", 200}, {"value", QJsonObject{}}}}};
    };
    music.lists[0] = songs;
    music.lists[1] = songs;
    music.hasMore = {false, false, false, false, false};
    music.keyword = "本地测试";
    music.status = "离线界面验证";
    hud.reduced = true;
    hud.openModal("search");
    const auto refresh = [&] {
        // 固定展示夹具不随物理选中自动重新读取；点击动作仍走真实原生控制器。
        music.selectedPhysical = backend.scene.selection();
        backend.step(50);
        settle();
    };
    const auto capture = [&](QString name) {
        if (!output.isEmpty()) {
            QDir().mkpath(output);
            window->grabWindow().save(output + "/" + name + ".png");
        }
    };
    window->requestActivate();
    refresh();
    const double scale = window->height() / 1080.0;
    const double width = window->width() / scale;
    const auto wheel = [&](double x, double y, bool pixels) {
        const QPointF position(x * scale, y * scale);
        QWheelEvent event(position, window->mapToGlobal(position), pixels ? QPoint(0, -15) : QPoint(),
                          pixels ? QPoint() : QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(window, &event);
        refresh();
    };
    const auto click = [&](QString action) {
        for (const auto &value : backend.hits()) {
            const auto hit = value.toMap();
            if (hit.value("action").toString() != action) {
                continue;
            }
            const auto offset = hud.modal.isEmpty() || hit.value("fixed").toBool() ? 0 : hud.motion.offset;
            const QPointF position((hit.value("x").toDouble() + hit.value("width").toDouble() / 2) * scale,
                                   (hit.value("y").toDouble() + offset + hit.value("height").toDouble() / 2) *
                                       scale);
            for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                QMouseEvent event(type, position, position, window->mapToGlobal(position), Qt::LeftButton,
                                  type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                                  Qt::NoModifier);
                QCoreApplication::sendEvent(window, &event);
            }
            refresh();
            return true;
        }
        return false;
    };
    const auto hasAction = [&](QString action) {
        return std::any_of(hud.hits.begin(), hud.hits.end(),
                           [&](const QJsonValue &value) { return value.toObject()["action"] == action; });
    };
    const auto textY = [&](QString label) -> std::optional<double> {
        for (const auto &value : hud.commands) {
            const auto command = value.toArray();
            if (command.at(0) == "text" && command.at(1) == label) {
                return command.at(3).toDouble();
            }
        }
        return std::nullopt;
    };
    const auto clickBlank = [&](QPointF from, QPointF to) {
        const auto send = [&](QEvent::Type type, QPointF stagePosition) {
            const auto position = stagePosition * scale;
            QMouseEvent event(type, position, position, window->mapToGlobal(position),
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(window, &event);
        };
        send(QEvent::MouseButtonPress, from);
        if (from != to) {
            send(QEvent::MouseMove, to);
        }
        send(QEvent::MouseButtonRelease, to);
        refresh();
    };
    // 使用真实窗口输入检查面板内留白、背景拖动和背景点击，避免关闭后穿透到场景。
    for (const auto &kind : QStringList{"search", "saved", "settings", "player", "discover", "provider"}) {
        hud.openModal(kind);
        refresh();
        const auto panel = hud.modalBounds(width);
        const QPointF inside(panel.x() + 10, panel.y() + 400);
        clickBlank(inside, inside);
        check(hud.modal == kind, "modal inner whitespace stays open");
        clickBlank({10, 400}, {10, 450});
        check(hud.modal == kind, "modal background drag stays open");
        const auto selected = backend.scene.selection();
        clickBlank({10, 400}, {10, 400});
        check(hud.modal.isEmpty() && backend.scene.selection() == selected,
              "modal background click closes without scene selection");
    }
    clickBlank({60, 1030}, {60, 1030});
    check(hud.modal == "player", "player cover opens queue");
    clickBlank({60, 1030}, {60, 1030});
    check(hud.modal.isEmpty(), "player cover toggles queue closed");
    hud.openModal("search");
    refresh();
    wheel(width * 0.7, 500, false);
    check(hud.listOffset == 1, "search row wheel routing");
    wheel(width * 0.7, 500, true);
    check(hud.listOffset == 2, "trackpad pixel wheel routing");
    auto input = window->findChild<QQuickItem *>("musicSearchInput");
    const auto bounds = hud.modalBounds(width);
    check(input && std::abs(input->x() / scale - bounds.x() - 53) < 0.01 &&
              std::abs(input->width() / scale - bounds.width() + 106) < 0.01 && input->hasActiveFocus(),
          "search input alignment and focus");
    check(click("music:page-next") && hud.listOffset == 7, "native next page click");
    check(click("music:page-prev") && hud.listOffset == 2, "native previous page click");
    capture("search");
    hud.openModal("saved");
    hud.musicCollection = 1;
    refresh();
    wheel(width / 2, 500, false);
    check(hud.listOffset == 1, "saved music row scrolling");
    QImage cover(1024, 1024, QImage::Format_RGBA8888_Premultiplied);
    cover.fill(QColor(78, 153, 197));
    QPainter painter(&cover);
    painter.scale(2, 2);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(213, 233, 231), 7));
    painter.drawEllipse(QRectF(52, 52, 408, 408));
    backend.glyphs()->text(painter, "RHINE LAB", 85, 112, 45, "bold", 1, Qt::white);
    backend.glyphs()->text(painter, "MUSIC ARCHIVE", 75, 355, 31, "demibold", 1, Qt::white);
    painter.end();
    backend.host.fixtureImage("fixture-disc", cover);
    hud.modal.clear();
    music.current = MusicData::item("playlist", "1",
                                    "《鸣潮》巡回演唱会「致予新世界」上海歌单 / 长标题布局验收", "测试歌手");
    music.tracks = songs;
    music.coverImage = "fixture-disc";
    backend.scene.review("detail");
    refresh();
    wheel(width * 0.5, 500, false);
    check(hud.trackOffset == 1, "playlist track row scrolling");
    capture("playlist");
    const auto originalWindowSize = window->size();
    window->resize(1152, 720);
    refresh();
    refresh();
    capture("playlist-16x10");
    window->resize(originalWindowSize);
    refresh();
    music.current = songs.first().toObject();
    music.current["title"] = "本地光盘模型验收";
    music.current["wiki"] = "本地生成封面 / 无网络访问";
    music.coverImage = "fixture-disc";
    music.detailLoading = false;
    hud.tab = "overview";
    refresh();
    capture("disc-case");
    // 同一张封面在揭示开始和结束时应同样清晰，验证真实 GPU 材质而非仅检查状态值。
    const auto detailFrame = [&](double clarity) {
        auto frame = backend.frame();
        frame["reference"] = false;
        QJsonArray draws;
        for (const auto &value : frame["draws"].toArray()) {
            auto draw = value.toObject();
            if (draw["name"] == "Frosted_Polymer" && draw["musicSelected"].toBool()) {
                draw["clarity"] = clarity;
            }
            draws.append(draw);
        }
        frame["draws"] = draws;
        return frame;
    };
    const auto initialCover = renderReference(backend.root(), detailFrame(0), backend.current(),
                                              backend.cover(), window->size(), backend.drawBuffers());
    const auto revealedCover = renderReference(backend.root(), detailFrame(1), backend.current(),
                                               backend.cover(), window->size(), backend.drawBuffers());
    int coverDifference = 0;
    check(equivalentGpuPixels(initialCover, revealedCover, coverDifference, 10000),
          "detail cover stays clear throughout material reveal");
    backend.scene.openViewer();
    backend.scene.coverFront();
    refresh();
    check(backend.frame()["viewerReady"].toBool() && !backend.frame()["draws"].toArray().isEmpty(),
          "viewer loads actual model geometry");
    // 比较封面中心区域，避开 HUD；真实窗口不能继续采样资料库的旧景深纹理。
    const auto actualViewer = window->grabWindow().convertToFormat(QImage::Format_RGB32);
    const auto expectedViewer = renderReference(backend.root(), backend.frame(), backend.current(),
                                                backend.cover(), actualViewer.size(), backend.drawBuffers())
                                    .convertToFormat(QImage::Format_RGB32);
    // 中心区域必须有可见模型，防止相机或绘制故障让两张空画面再次通过。
    const auto clearColor = actualViewer.pixelColor(actualViewer.width() / 2, actualViewer.height() * .22);
    int modelPixels = 0;
    int modelSamples = 0;
    for (int y = actualViewer.height() * .3; y < actualViewer.height() * .7; y += 8) {
        for (int x = actualViewer.width() * .35; x < actualViewer.width() * .65; x += 8) {
            const auto pixel = actualViewer.pixelColor(x, y);
            const int contrast = qAbs(pixel.red() - clearColor.red()) +
                                 qAbs(pixel.green() - clearColor.green()) +
                                 qAbs(pixel.blue() - clearColor.blue());
            modelPixels += contrast > 16;
            ++modelSamples;
        }
    }
    check(modelSamples > 0 && modelPixels > modelSamples * .05, "viewer draws visible model pixels");
    int matchingPixels = 0;
    int sampledPixels = 0;
    if (actualViewer.size() == expectedViewer.size()) {
        for (int y = actualViewer.height() / 4; y < actualViewer.height() * .72; y += 8) {
            for (int x = actualViewer.width() / 5; x < actualViewer.width() * .8; x += 8) {
                const auto actualColor = actualViewer.pixelColor(x, y);
                const auto expectedColor = expectedViewer.pixelColor(x, y);
                const int difference = qAbs(actualColor.red() - expectedColor.red()) +
                                       qAbs(actualColor.green() - expectedColor.green()) +
                                       qAbs(actualColor.blue() - expectedColor.blue());
                matchingPixels += difference <= 24;
                ++sampledPixels;
            }
        }
    }
    if (!sampledPixels || matchingPixels <= sampledPixels * .95) {
        qWarning() << "Viewer GPU comparison:" << actualViewer.size() << expectedViewer.size()
                   << matchingPixels << sampledPixels;
        if (!output.isEmpty()) {
            actualViewer.save(output + "/viewer-actual.png");
            expectedViewer.save(output + "/viewer-expected.png");
        }
    }
    check(sampledPixels > 0 && matchingPixels > sampledPixels * .95,
          "viewer samples current scene texture after archive transition");
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/viewer-front.png");
    }
    backend.scene.coverFront(false);
    refresh();
    check(backend.frame()["eye"].toArray().at(2).toDouble() < 0, "disc rear camera");
    capture("disc-back");
    backend.scene.viewerAction("close");
    backend.step(51);
    hud.modal.clear();
    backend.scene.reduced(false);
    QJsonArray followSongs;
    for (int id : {501, 502}) {
        auto song = songs.first().toObject();
        song["id"] = QString::number(id);
        song["key"] = "netease:song:" + QString::number(id);
        song["title"] = "本地切歌 " + QString::number(id);
        followSongs.append(song);
        music.details[song["key"].toString()] = song;
    }
    player.queue = followSongs;
    player.current = followSongs.first().toObject();
    music.followPlayback(player.current);
    for (int index = 1; index <= 240; ++index) {
        backend.step(51 + index / 60.0);
    }
    player.current = followSongs.at(1).toObject();
    music.followPlayback(player.current);
    backend.step(55.01);
    check(music.current["id"] == "502" && music.directory && music.directory->index == 1 &&
              hud.frame["mode"] == "detail",
          "song change synchronizes upper detail and archive selection");
    check(backend.frame()["returningCards"].toInt() > 0, "song change returns outgoing disc");
    for (int index = 1; index <= 240; ++index) {
        backend.step(55.01 + index / 60.0);
    }
    capture("song-switch");
    backend.scene.setMode("archive");
    refresh();
    std::optional<double> parentY;
    std::optional<double> infoY;
    for (const auto &value : hud.hits) {
        const auto hit = value.toObject();
        if (hit["action"] == "music:parent") {
            parentY = hit["y"].toDouble();
        } else if (hit["action"] == "music:directory-info") {
            infoY = hit["y"].toDouble();
        }
    }
    check(parentY && infoY && *infoY == *parentY + 54,
          "directory info sits immediately below parent navigation");
    capture("directory-navigation");
    // 列表使用真实 QML 键盘与点击路径；临时目录避免改变后续既有导航夹具。
    const auto originalDirectory = music.directory;
    music.directory = std::make_shared<MusicDirectory>(*originalDirectory);
    music.directory->songs = songs;
    music.directory->ids = {};
    music.selectDirectory(12);
    refresh();
    const auto selectedTitle = music.current["title"].toString();
    const auto titleCommand = [&]() {
        for (const auto &value : hud.commands) {
            const auto command = value.toArray();
            if (command.at(0) == "text" && command.at(1) == selectedTitle) {
                return command;
            }
        }
        return QJsonArray{};
    };
    const auto originalTitle = titleCommand();
    const auto toggleTrackList = [&] {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
        refresh();
    };
    check(hasAction("archive:tab-mode") && click("archive:tab-mode"), "visible Tab Mode button toggles view");
    check(hud.trackListMode && titleCommand() == originalTitle,
          "Tab preserves original current title font, position and size");
    check(backend.frame()["trackListBlend"].toDouble() > 0,
          "Tab mode shifts archive projection toward lower left");
    const auto rulerArrow = [&] {
        QJsonArray arrows;
        for (const auto &value : hud.commands) {
            const auto command = value.toArray();
            if (command.at(0) == "fill-path" &&
                command.at(2).toArray() == QJsonArray{0.46, 0.48, 0.37, 0.75}) {
                arrows.append(command);
            }
        }
        return arrows;
    };
    const auto fixedArrow = rulerArrow();
    QVector<double> rowCenters;
    QVector<double> tickCenters;
    bool sameTypography = true;
    bool horizontal = true;
    for (const auto &value : hud.commands) {
        const auto command = value.toArray();
        if (command.at(0) == "text" && command.at(1).toString().startsWith("本地测试曲目 ")) {
            sameTypography &= command.at(4) == 28 && command.at(5) == "bold";
            const auto bounds = backend.glyphs()->bounds(command.at(1).toString(), 28, "bold");
            rowCenters.append(command.at(3).toDouble() + bounds["y"].toDouble() +
                              bounds["height"].toDouble() / 2);
        }
        if (command.at(0) == "fill" && command.at(5).toDouble() == 1.2 &&
            (command.at(4).toDouble() == 14 || command.at(4).toDouble() == 50)) {
            tickCenters.append(command.at(3).toDouble() + 0.6);
        }
        horizontal &= command.at(0) != "rotate";
    }
    std::sort(rowCenters.begin(), rowCenters.end());
    std::sort(tickCenters.begin(), tickCenters.end());
    bool alignedTicks = rowCenters.size() == 9 && tickCenters.size() == 9;
    for (int index = 0; alignedTicks && index < 9; ++index) {
        alignedTicks &= std::abs(rowCenters.at(index) - tickCenters.at(index)) < 1;
    }
    check(sameTypography && horizontal && alignedTicks,
          "nine horizontal titles share typography and each has an aligned ruler tick");
    check(rowCenters.size() == 9 &&
              rowCenters.at(4) - rowCenters.at(3) > rowCenters.at(3) - rowCenters.at(2) &&
              rowCenters.at(5) - rowCenters.at(4) > rowCenters.at(6) - rowCenters.at(5),
          "selected song has larger gaps on both sides");
    capture("archive-track-list");
    // 固定时间采样整行运动与刻度，覆盖快速反向打断和动画结束后的休眠条件。
    hud.reduced = false;
    music.selectDirectory(13);
    backend.step(50);
    backend.step(50.1);
    const auto incomingEarly = textY("本地测试曲目 14");
    check(incomingEarly && *incomingEarly > 509 && *incomingEarly < 597 && hud.animating(50.1),
          "incoming whole song slides upward from its previous row");
    bool movingTickAligned = false;
    if (incomingEarly) {
        const auto bounds = backend.glyphs()->bounds("本地测试曲目 14", 28, "bold");
        const auto center = *incomingEarly + bounds["y"].toDouble() + bounds["height"].toDouble() / 2;
        for (const auto &value : hud.commands) {
            const auto command = value.toArray();
            if (command.at(0) == "fill" && command.at(5).toDouble() == 1.2 &&
                std::abs(command.at(3).toDouble() + 0.6 - center) < 0.01) {
                movingTickAligned = true;
            }
        }
    }
    check(movingTickAligned, "ruler tick follows song throughout vertical transition");
    check(fixedArrow.size() == 1 && rulerArrow() == fixedArrow,
          "single ruler arrow stays fixed while songs and ticks slide");
    backend.step(50.22);
    const auto incomingLater = textY("本地测试曲目 14");
    check(incomingEarly && incomingLater && *incomingLater < *incomingEarly,
          "song and ruler continue sliding across multiple frames");
    if (!output.isEmpty()) {
        settle();
        window->grabWindow().save(output + "/archive-track-list-moving.png");
    }
    music.selectDirectory(12);
    backend.step(50.22);
    const auto interrupted = textY("本地测试曲目 14");
    check(incomingLater && interrupted && std::abs(*incomingLater - *interrupted) < 0.01,
          "rapid reverse selection preserves current list position");
    backend.step(51);
    check(textY("本地测试曲目 13") == std::optional<double>(509) && !hud.animating(52),
          "list settles exactly at original title anchor and stops scheduling");
    hud.reduced = true;
    refresh();
    const auto savedTitle = music.current["title"];
    music.current["title"] = QString("超长歌曲名称 Long title ").repeated(20);
    refresh();
    check(rulerArrow() == fixedArrow, "long song names do not move ruler or arrow");
    music.current["title"] = savedTitle;
    refresh();
    check(click("music:index:14") && music.directory->index == 14 && hud.frame["mode"] == "archive",
          "list click selects the matching archive song without opening details");
    music.selectDirectory(0);
    refresh();
    check(!hasAction("music:index:-1") && hasAction("music:index:4") && !hasAction("music:index:5"),
          "track list start shows only available successors without wrapping");
    music.selectDirectory(39);
    refresh();
    check(hasAction("music:index:35") && !hasAction("music:index:34") && !hasAction("music:index:40"),
          "track list end shows only available predecessors without wrapping");
    hud.openModal("search");
    refresh();
    toggleTrackList();
    check(hud.trackListMode, "Tab in search does not toggle archive view");
    hud.closeModal();
    refresh();
    toggleTrackList();
    check(!hud.trackListMode, "Tab restores original archive view");
    for (int index = 1; index <= 150; ++index) {
        backend.step(52 + index / 60.0);
    }
    const auto restoredProjection = backend.frame()["projection"].toArray();
    check(backend.frame()["trackListBlend"].toDouble() == 0 && restoredProjection.at(8).toDouble() == 0 &&
              restoredProjection.at(9).toDouble() == 0,
          "leaving Tab mode restores original archive projection exactly");
    music.directory = originalDirectory;
    music.selectDirectory(originalDirectory->index);
    refresh();
    const auto folder = music.directory;
    const auto directoryIndex = folder->index;
    check(click("music:directory-info") && music.directoryInfo, "directory info opens");
    clickBlank({50, 600}, {50, 600});
    check(music.directoryInfo && hud.frame["mode"] == "detail", "directory panel whitespace stays open");
    clickBlank({width - 10, 200}, {width - 10, 250});
    check(music.directoryInfo && hud.frame["mode"] == "detail", "directory background drag stays open");
    clickBlank({width - 10, 200}, {width - 10, 200});
    check(!music.directoryInfo && hud.frame["mode"] == "archive" && music.directory == folder &&
              music.directory->index == directoryIndex,
          "directory background click returns to same track and folder");
    check(click("open"), "song detail opens from directory");
    clickBlank({width - 10, 200}, {width - 10, 200});
    check(hud.frame["mode"] == "archive", "song detail background click returns");
    music.directory.reset();
    player.current = {};
    backend.scene.reduced(true);
    for (const auto &kind : {QString("album"), QString("playlist"), QString("artist")}) {
        hud.openModal("search");
        music.searchKind = kind;
        music.current = MusicData::item(kind, "8", "本地分类检索", "测试歌手");
        refresh();
        check(!hasAction("music:play-loaded"), "search excludes background directory");
        capture("search-" + kind);
    }
    hud.openModal("search");
    hud.action("music:group:1");
    music.lists[0] = {};
    for (int index = 0; index < songs.size(); ++index) {
        auto song = songs.at(index).toObject();
        song["artists"] = QJsonArray{QJsonObject{{"id", QString::number(index / 10 + 10)},
                                                 {"name", "测试歌手 " + QString::number(index / 10)}}};
        music.lists[0].append(song);
    }
    music.changed();
    refresh();
    check(click("music:expand:artist%3A10"), "artist group native click");
    check(hud.activeGroup == "artist:10" && hud.rows(0).size() == 10, "group preserves song indices");
    check(click("music:group-back"), "group return native click");
    capture("artist-groups");
    for (const bool keyboard : {false, true}) {
        hud.modal.clear();
        auto directory = std::make_shared<MusicDirectory>();
        directory->item = MusicData::item("artist", "789658", "HOYO-MiX", "", "kugou");
        directory->parentPhysical = music.physicalSlots[0].first();
        directory->parentItem = songs.first().toObject();
        music.directory = directory;
        music.showDirectoryInfo();
        backend.scene.review("detail");
        refresh();
        if (keyboard) {
            QKeyEvent event(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &event);
            refresh();
        } else {
            check(click("music:directory"), "empty artist return button");
        }
        check(!music.directory && !music.directoryInfo, "empty artist restores parent");
    }
    QTemporaryDir audioFixture;
    QFile audioFile(audioFixture.filePath("silent.wav"));
    check(audioFile.open(QIODevice::WriteOnly), "local audio fixture file");
    QDataStream wav(&audioFile);
    wav.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 bytes = 8000 * 2 * 4;
    wav.writeRawData("RIFF", 4);
    wav << quint32(36 + bytes);
    wav.writeRawData("WAVEfmt ", 8);
    wav << quint32(16) << quint16(1) << quint16(1) << quint32(8000) << quint32(16000) << quint16(2)
        << quint16(16);
    wav.writeRawData("data", 4);
    wav << bytes;
    wav.writeRawData(QByteArray(bytes, '\0').constData(), bytes);
    audioFile.close();
    fixtureAudioURL = QUrl::fromLocalFile(audioFile.fileName()).toString();
    backend.host.mediaControl("volume", 0);
    check(backend.host.mediaLoad(QUrl::fromLocalFile(audioFile.fileName()).toString()),
          "local audio source accepted");
    wait.restart();
    while (backend.host.mediaState().value("duration").toLongLong() == 0 && wait.elapsed() < 5000) {
        settle();
    }
    const auto decoded = backend.host.mediaState();
    check(decoded.value("duration").toLongLong() >= 3900 && decoded.value("error").toString().isEmpty(),
          "native PCM decode duration");
    backend.host.mediaControl("pause");
    settle();
    check(!backend.host.mediaState().value("playing").toBool(), "native pause");
    backend.host.mediaControl("seek", 1500);
    // 系统音频后端异步完成跳转；等待目标位置，不能用固定的一帧延迟判断。
    wait.restart();
    while (qAbs(backend.host.mediaState().value("position").toLongLong() - 1500) >= 100 &&
           wait.elapsed() < 3000) {
        settle();
    }
    const auto seekState = backend.host.mediaState();
    if (qAbs(seekState.value("position").toLongLong() - 1500) >= 100) {
        qWarning() << "Native seek state:" << seekState;
    }
    check(qAbs(seekState.value("position").toLongLong() - 1500) < 100, "native seek");
    backend.host.mediaControl("volume", .4);
    check(qAbs(backend.host.mediaState().value("volume").toDouble() - .4) < .01, "native volume");
    player.current = songs.first().toObject();
    player.queue = MusicData::slice(songs, 0, 10);
    player.index = 0;
    hud.modal.clear();
    hud.lyricsMode = false;
    hud.reduced = false;
    music.current = player.current;
    player.actualQuality = "sq";
    player.loading = false;
    player.failed = false;
    backend.scene.musicIdentity(music.current["key"].toString());
    backend.scene.review("detail");
    backend.host.mediaControl("seek", 500);
    backend.host.mediaControl("resume");
    refresh();
    check(!hud.discRotation, "disc rotation is disabled by default");
    const auto disabledAngle = backend.frame()["discAngle"].toDouble();
    settle();
    refresh();
    check(backend.frame()["discAngle"].toDouble() == disabledAngle, "default playback never rotates");
    hud.reduced = true;
    backend.action("disc-rotation");
    refresh();
    check(hud.modal == "disc-rotation-warning" && !hud.discRotation, "enabling requires a risk prompt");
    capture("disc-rotation-warning");
    check(click("disc-rotation:cancel") && !hud.discRotation, "cancel leaves rotation disabled");
    backend.action("disc-rotation");
    refresh();
    check(click("disc-rotation:enable") && hud.discRotation, "risk acceptance enables rotation");
    hud.modal.clear();
    hud.motion.set(false, 50, true);
    hud.reduced = false;
    refresh();
    const auto discAngle = backend.frame()["discAngle"].toDouble();
    const auto discUploads = backend.performance()["coverUploads"].toULongLong();
    settle();
    refresh();
    check(backend.frame()["discAngle"].toDouble() > discAngle, "playing selected disc rotates");
    check(backend.performance()["coverUploads"].toULongLong() == discUploads,
          "rotation reuses the uploaded cover texture");
    const auto qualityImage = std::find_if(hud.commands.begin(), hud.commands.end(), [](QJsonValue value) {
        const auto command = value.toArray();
        return command[0] == "image" && command[1] == "quality:netease-sq";
    });
    check(qualityImage != hud.commands.end(), "playing song displays actual source badge");
    capture("playing-disc-quality");
    hud.lyricsMode = true;
    refresh();
    const auto lyricsDiscAngle = backend.frame()["discAngle"].toDouble();
    settle();
    refresh();
    check(backend.frame()["discAngle"].toDouble() > lyricsDiscAngle,
          "lyrics page keeps selected disc rotating");
    hud.lyricsMode = false;
    backend.host.mediaControl("pause");
    refresh();
    const auto pausedAngle = backend.frame()["discAngle"].toDouble();
    settle();
    refresh();
    check(backend.frame()["discAngle"].toDouble() == pausedAngle, "paused disc preserves its angle");
    hud.reduced = true;
    check(click("quality:open") && hud.modal == "quality", "quality badge opens its explanation card");
    capture("quality-card");
    const auto qualityProvider = music.provider;
    const bool qualityDark = hud.dark;
    const auto qualityThemeMode = hud.themeMode;
    hud.themeMode = "light";
    for (const auto &provider : QStringList{"netease", "qq", "kugou"}) {
        music.provider = provider;
        refresh();
        capture("quality-card-" + provider);
        const auto bounds = hud.modalBounds(width);
        int qualityChoices = 0;
        for (const auto &value : hud.hits) {
            const auto target = value.toObject();
            if (!target["action"].toString().startsWith("quality:set:")) {
                continue;
            }
            ++qualityChoices;
            const QRectF choice(target["x"].toDouble(), target["y"].toDouble(), target["width"].toDouble(),
                                target["height"].toDouble());
            check(bounds.contains(choice), "every provider quality option remains inside its panel");
        }
        check(qualityChoices == MusicPlayer::qualityOptions(provider).size(),
              "quality redesign retains every supported provider option");
    }
    music.provider = qualityProvider;
    hud.dark = true;
    refresh();
    capture("quality-card-dark");
    hud.dark = qualityDark;
    hud.themeMode = qualityThemeMode;
    refresh();
    const auto qualityPosition = backend.host.mediaState()["position"].toDouble();
    const auto qualityQueue = player.queue;
    check(click("quality:set:jymaster") && player.quality == "jymaster",
          "featured master card selects the shared preference");
    wait.restart();
    while (player.loading && wait.elapsed() < 3000) {
        settle();
    }
    refresh();
    check(player.actualQuality == "standard", "selected master does not overwrite actual source quality");
    capture("quality-card-master-selected");
    player.failed = true;
    refresh();
    check(textY("音源不可用").has_value(), "failed playback never presents a stale available quality");
    player.failed = false;
    refresh();
    check(click("quality:set:standard") && player.quality == "standard",
          "quality card updates shared preference");
    wait.restart();
    while (player.loading && wait.elapsed() < 3000) {
        settle();
    }
    wait.restart();
    while (std::abs(backend.host.mediaState()["position"].toDouble() - qualityPosition) >= 100 &&
           wait.elapsed() < 3000) {
        settle();
    }
    check(std::abs(backend.host.mediaState()["position"].toDouble() - qualityPosition) < 100 &&
              !backend.host.mediaState()["playing"].toBool() && player.queue == qualityQueue,
          "quality card preserves actual decoder position, pause and queue");
    backend.action("settings");
    refresh();
    check(textY("标准").has_value(), "settings reflects quality card selection");
    check(click("quality"), "settings quality cycle remains available");
    const auto selectedQuality = player.quality;
    backend.action("quality:open");
    refresh();
    check(player.quality == selectedQuality, "quality card reflects settings selection");
    hud.modal.clear();
    hud.motion.set(false, 50, true);
    player.actualQuality = "sq";
    music.current = songs.at(1).toObject();
    refresh();
    check(backend.frame()["discAngle"].toDouble() == 0, "another song has no playing disc rotation");
    const auto staleBadge = std::find_if(hud.commands.begin(), hud.commands.end(), [](QJsonValue value) {
        const auto command = value.toArray();
        return command[0] == "image" && command[1].toString().startsWith("quality:");
    });
    check(staleBadge == hud.commands.end(), "another song does not inherit playback quality");
    // 使用更长的本地 PCM，确保休眠观察期间持续播放并触发真实进度通知。
    QFile shortAudio(audioFile.fileName());
    check(shortAudio.open(QIODevice::ReadOnly), "read long playback fixture source");
    auto longWave = shortAudio.readAll();
    const auto samples = longWave.mid(44);
    for (int repeat = 0; repeat < 4; ++repeat) {
        longWave.append(samples);
    }
    qToLittleEndian<quint32>(quint32(longWave.size() - 8), longWave.data() + 4);
    qToLittleEndian<quint32>(quint32(longWave.size() - 44), longWave.data() + 40);
    QFile longAudio(audioFixture.filePath("rotation-idle.wav"));
    check(longAudio.open(QIODevice::WriteOnly), "write long playback fixture");
    longAudio.write(longWave);
    longAudio.close();
    backend.host.mediaLoad(QUrl::fromLocalFile(longAudio.fileName()).toString());
    backend.scene.review("archive");
    hud.reduced = true;
    backend.wake();
    wait.restart();
    while (!backend.performance()["idle"].toBool() && wait.elapsed() < 5000) {
        settle();
    }
    check(backend.performance()["idle"].toBool(), "playing archive enters demand sleep");
    const auto archiveSceneUpdates = backend.performance()["sceneUpdates"].toULongLong();
    const auto archivePosition = backend.host.mediaState()["position"].toLongLong();
    bool stayedAsleep = true;
    for (int sample = 0; sample < 12; ++sample) {
        settle();
        stayedAsleep &=
            backend.performance()["idle"].toBool() && !backend.performance()["timerActive"].toBool();
    }
    check(stayedAsleep && backend.performance()["sceneUpdates"].toULongLong() == archiveSceneUpdates &&
              backend.host.mediaState()["position"].toLongLong() > archivePosition,
          "playing archive remains asleep through progress updates without 3D redraws");
    {
        // 独立时钟避免固定展示夹具的 50 秒时间污染正常动效与媒体通知回归。
        Engine playingArchive(backend.root(), true);
        playingArchive.scene.review("archive");
        auto &archiveMusic = playingArchive.app->music;
        archiveMusic.selectedPhysical = playingArchive.scene.selection();
        archiveMusic.current = player.current;
        archiveMusic.player.current = player.current;
        playingArchive.app->hud.discRotation = true;
        playingArchive.app->hud.reduced = false;
        playingArchive.host.mediaControl("volume", 0);
        playingArchive.host.mediaLoad(QUrl::fromLocalFile(longAudio.fileName()).toString());
        playingArchive.start();
        wait.restart();
        while (!playingArchive.performance()["idle"].toBool() && wait.elapsed() < 5000) {
            settle();
        }
        check(playingArchive.performance()["idle"].toBool(),
              "full-motion playing archive enters demand sleep");
        const auto sceneUpdates = playingArchive.performance()["sceneUpdates"].toULongLong();
        bool idleDuringPlayback = true;
        for (int sample = 0; sample < 12; ++sample) {
            archiveMusic.player.lyrics.changed();
            settle();
            idleDuringPlayback &= playingArchive.performance()["idle"].toBool() &&
                                  !playingArchive.performance()["timerActive"].toBool();
        }
        check(idleDuringPlayback &&
                  playingArchive.performance()["sceneUpdates"].toULongLong() == sceneUpdates,
              "full-motion archive ignores progress and lyrics wakeups while rotation is enabled");
    }
    backend.host.mediaLoad(fixtureAudioURL);
    backend.host.mediaControl("pause");
    hud.discRotation = false;
    music.current = player.current;
    backend.scene.review("detail");
    hud.reduced = true;
    hud.openModal("player");
    refresh();
    check(click("player:mode") && player.mode == "one", "native playback mode click");
    check(click("player:remove:1") && player.queue.size() == 9, "native queue removal");
    const auto dragControl = [&](const std::optional<QRectF> &bounds, double fraction) {
        if (!bounds) {
            check(false, "player drag region exists");
            return;
        }
        const QPointF start((bounds->x() + bounds->width() * 0.2) * scale, bounds->center().y() * scale);
        const QPointF end((bounds->x() + bounds->width() * fraction) * scale, start.y());
        for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease}) {
            const auto position = type == QEvent::MouseButtonPress ? start : end;
            QMouseEvent event(type, position, position, window->mapToGlobal(position),
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(window, &event);
            settle();
        }
    };
    dragControl(hud.playerSeekRect, 0.5);
    wait.restart();
    while (std::abs(backend.host.mediaState()["position"].toLongLong() - 2000) >= 100 &&
           wait.elapsed() < 3000) {
        settle();
    }
    check(std::abs(backend.host.mediaState()["position"].toLongLong() - 2000) < 100,
          "QML seek reaches native decoder");
    dragControl(hud.playerVolumeRect, 0.6);
    check(std::abs(backend.host.mediaState()["volume"].toDouble() - 0.6) < 0.01, "QML volume reaches output");
    capture("player");
    player.lyrics.load(songs.first().toObject(), true);
    refresh();
    check(click("player:lyrics"), "native lyrics button");
    refresh();
    check(hud.lyricsMode && music.current["id"] == player.current["id"],
          "lyrics opens playing song immediately");
    check(textY("每一束光都落在这里").has_value(), "lyrics replaces detail card");
    capture("lyrics");
    check(click("player:lyric-line:1"), "native lyric seek click");
    wait.restart();
    while (std::abs(backend.host.mediaState()["position"].toLongLong() - 1000) >= 100 &&
           wait.elapsed() < 3000) {
        settle();
    }
    check(std::abs(backend.host.mediaState()["position"].toLongLong() - 1000) < 100, "lyric seeks decoder");
    backend.action("player:lyrics-follow");
    check(player.lyrics.index == 1, "lyrics follows native seek");
    backend.action("player:lyrics-earlier");
    refresh();
    check(player.lyrics.offsetMs == 200 && textY("已提前 0.2s").has_value(),
          "lyric offset and direction label");
    player.lyrics.load(songs.first().toObject());
    refresh();
    check(player.lyrics.offsetMs == 0, "cached lyrics reset offset");
    const auto beforeScroll = textY("我们听见远处的回声");
    backend.wheel(width * 0.7, 500, -2, true);
    refresh();
    const auto afterScroll = textY("我们听见远处的回声");
    check(beforeScroll && afterScroll && *beforeScroll > *afterScroll && *beforeScroll - *afterScroll < 10,
          "sub-row pixels scroll lyrics continuously");
    clickBlank({width - 10, 200}, {width - 10, 200});
    check(!hud.lyricsMode && hud.frame["mode"] == "archive", "lyrics background click returns");
    backend.host.mediaControl("stop");
    player.stop();
    hud.openModal("discover");
    music.discover->items = MusicData::slice(songs, 0, 10);
    music.discover->status = "本地发现页面验收";
    refresh();
    capture("discover");
    hud.openModal("login");
    music.loginMethod = "email";
    refresh();
    auto accountField = window->findChild<QQuickItem *>("accountField");
    auto passwordField = window->findChild<QQuickItem *>("passwordField");
    check(accountField && passwordField && accountField->isVisible() && passwordField->isVisible() &&
              passwordField->property("echoMode").toInt() == 2,
          "native login and masked password");
    capture("login");
    hud.openModal("settings");
    refresh();
    check(hasAction("provider") && !hasAction("provider:qq"), "settings has one provider entry");
    capture("settings");
    music.switchProvider("qq");
    hud.openModal("discover");
    music.discover->items = MusicData::slice(songs, 0, 6);
    refresh();
    check(hasAction("discover:tab:daily") && hasAction("discover:tab:guess") &&
              hasAction("discover:tab:new-songs"),
          "QQ daily, guess and new songs are separate entries");
    capture("qq-discover");
    hud.openModal("search");
    refresh();
    check(hasAction("music:kind:0") && hasAction("music:kind:1") && hasAction("music:kind:2") &&
              hasAction("music:kind:3"),
          "QQ supports all four search filters");
    capture("qq-search");
    hud.closeModal();
    music.current = songs.first().toObject();
    backend.scene.review("detail");
    refresh();
    check(hasAction("music:like") && hasAction("music:add-to-playlist") && hasAction("music:preview"),
          "QQ heart, playlist and explicit preview controls are visible");
    capture("qq-detail");
    hud.openModal("login");
    music.loginMethod = "wechat";
    refresh();
    check(!accountField->isVisible() && !passwordField->isVisible(), "QQ QR hides password form");
    capture("qq-login");
    hud.openModal("settings");
    refresh();
    check(click("provider") && hasAction("provider:netease") && hasAction("provider:qq") &&
              hasAction("provider:kugou"),
          "three directly selectable providers");
    capture("providers");
    music.switchProvider("kugou");
    hud.openModal("login");
    music.loginMethod = "qr";
    refresh();
    check(hasAction("music:kugou-edition:standard") && hasAction("music:kugou-edition:lite"),
          "Kugou app editions");
    capture("kugou-login");
    music.switchProvider("kugou", "lite");
    hud.openModal("login");
    refresh();
    check(music.kugouEdition == "lite" && music.providerName() == "酷狗概念版", "Kugou concept edition");
    capture("kugou-concept-login");
    hud.openModal("settings");
    refresh();
    const auto previousVisibility = window->visibility();
    backend.action("window:fullscreen");
    wait.restart();
    while (window->visibility() != QWindow::FullScreen && wait.elapsed() < 10000) {
        settle();
    }
    check(window->visibility() == QWindow::FullScreen, "native fullscreen");
    backend.action("window:fullscreen");
    wait.restart();
    while (window->visibility() != previousVisibility && wait.elapsed() < 10000) {
        settle();
    }
    check(window->visibility() == previousVisibility, "restore window mode");
    window->resize(originalWindowSize);
    player.stop();
    hud.modal.clear();
    hud.motion.set(true, 0, true);
    backend.scene.review("archive");
    backend.scene.setMode("archive");
    backend.wake();
    wait.restart();
    while (!backend.performance()["idle"].toBool() && wait.elapsed() < 15000) {
        settle();
    }
    check(backend.performance()["idle"].toBool(), "native scene converges into idle");
    const auto idleSteps = backend.performance()["steps"].toULongLong();
    const auto idleFrames = backend.performance()["renderFrames"].toULongLong();
    for (int index = 0; index < 8; ++index) {
        settle();
    }
    check(backend.performance()["steps"].toULongLong() == idleSteps &&
              backend.performance()["renderFrames"].toULongLong() == idleFrames,
          "idle has zero CPU steps and GPU renders");
    backend.scene.theme(true);
    backend.host.invalidate();
    settle();
    check(backend.performance()["steps"].toULongLong() > idleSteps,
          "background scene change wakes scheduler");
    backend.scene.theme(false);
    window->hide();
    settle();
    const auto hiddenSteps = backend.performance()["steps"].toULongLong();
    backend.host.invalidate();
    for (int index = 0; index < 4; ++index) {
        settle();
    }
    check(!backend.performance()["timerActive"].toBool() &&
              backend.performance()["steps"].toULongLong() == hiddenSteps,
          "hidden window stays asleep");
    window->show();
    // Windows 的窗口曝光异步完成；等待实际推进，避免固定 150 ms 把正常恢复误判为失败。
    wait.restart();
    while (backend.performance()["steps"].toULongLong() <= hiddenSteps && wait.elapsed() < 8000) {
        settle();
    }
    check(backend.performance()["steps"].toULongLong() > hiddenSteps, "window restore refreshes state");
    backend.setVisible(false);
    hud.modal.clear();
    backend.scene.review("detail");
    backend.setVisible(true);
    const auto cachedSelection = [&](int index) {
        music.current = MusicData::item("song", QString::number(index), "缓存回选验收", "本地");
        music.current["key"] = "cache-" + QString::number(index);
        music.coverImage = "fixture-disc";
        music.selectedPhysical = backend.scene.selection();
        backend.step(70);
        settle();
        // 资源计数由渲染线程提交；同步抓取本帧后再断言，不能拿排队中的旧帧计数比较。
        check(!window->grabWindow().isNull(), "cache selection reaches the GPU");
    };
    cachedSelection(0);
    cachedSelection(1);
    cachedSelection(0);
    const auto warmedResources = backend.performance()["gpuResources"].toULongLong();
    for (int index = 0; index < 20; ++index) {
        cachedSelection(index % 2);
    }
    check(warmedResources > 0 && backend.performance()["gpuResources"].toULongLong() == warmedResources,
          "cover cache hits keep GPU resources stable");
    backend.scene.diagnostics(false);
    const auto beforeBindings = backend.performance()["bindingCreations"].toULongLong();
    const auto beforeUploads = backend.performance()["coverUploads"].toULongLong();
    cachedSelection(99);
    const auto newBindings = backend.performance()["bindingCreations"].toULongLong() - beforeBindings;
    check(newBindings > 0 && newBindings <= 8, "new song updates only texture bindings");
    check(backend.performance()["coverUploads"].toULongLong() == beforeUploads,
          "album cover shares uploaded texture");
    // 多首新曲填满纹理缓存后，隐藏窗口须销毁 GPU 对象；恢复须还原同一幅冻结画面。
    for (int i = 100; i < 135; ++i) {
        cachedSelection(i);
    }
    check(backend.performance().value("gpuMusicTextureBytes").toULongLong() <= 24 * 1024 * 1024,
          "settled song texture cache respects the byte budget");
    const auto beforeRelease = window->grabWindow();
    const auto frameBeforeRelease = backend.frame();
    const auto buffersBeforeRelease = backend.drawBuffers();
    const auto commandsBeforeRelease = backend.commands();
    const auto selectedBeforeRelease = backend.current();
    window->hide();
    wait.restart();
    while (backend.performance().value("gpuResources").toULongLong() != 0 && wait.elapsed() < 8000) {
        settle();
    }
    check(backend.performance().value("gpuResources").toULongLong() == 0,
          "sustained background releases the graphics scene");
    check(!backend.performance().value("timerActive").toBool(), "resource cleanup keeps visual timer asleep");
    window->show();
    wait.restart();
    while (backend.performance().value("gpuResources").toULongLong() == 0 && wait.elapsed() < 8000) {
        settle();
    }
    settle();
    check(backend.current() == selectedBeforeRelease, "graphics rebuild preserves current song");
    qInfo() << "Graphics restore exposure:" << window->isVisible() << window->isExposed()
            << window->visibility() << backend.performance().value("visible")
            << backend.performance().value("gpuResources");
    check(backend.performance().value("gpuResources").toULongLong() > 0,
          "restored window rebuilds GPU resources");
    const auto afterRelease = window->grabWindow();
    if (!output.isEmpty()) {
        beforeRelease.save(output + "/background-before.png");
        afterRelease.save(output + "/background-after.png");
        for (const auto &entry : {qMakePair(QString("background-before.json"), frameBeforeRelease),
                                  qMakePair(QString("background-after.json"), backend.frame())}) {
            QFile file(output + "/" + entry.first);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(QJsonDocument(entry.second).toJson());
            }
        }
    }
    auto frameBefore = frameBeforeRelease;
    auto frameAfter = backend.frame();
    for (const auto &key : {"time", "boot"}) {
        frameBefore.remove(key);
        frameAfter.remove(key);
    }
    check(frameBefore == frameAfter, "graphics rebuild preserves exact scene and material data");
    check(commandsBeforeRelease == backend.commands(), "graphics rebuild preserves exact HUD commands");
    const auto buffersAfterRelease = backend.drawBuffers();
    bool buffersMatch = buffersBeforeRelease.size() == buffersAfterRelease.size();
    for (auto it = buffersBeforeRelease.begin(); it != buffersBeforeRelease.end(); ++it) {
        const auto after = buffersAfterRelease.value(it.key());
        buffersMatch &= it->matrices == after.matrices && it->themes == after.themes;
    }
    check(buffersMatch, "graphics rebuild preserves exact matrix and theme bytes");
    int changedPixels = 0;
    check(equivalentGpuPixels(beforeRelease, afterRelease, changedPixels),
          "background release and rebuild preserve the frozen window pixels");
    qInfo() << "[ui] Rebuild RGB8 rounding pixels:" << changedPixels;
    if (!output.isEmpty()) {
        QFile metrics(output + "/performance.json");
        if (metrics.open(QIODevice::WriteOnly)) {
            metrics.write(QJsonDocument::fromVariant(backend.performance()).toJson());
        }
    }
    // 用户侧性能诊断覆盖实际窗口、停止采样、文件导出和业务内容隔离。
    const auto failuresBeforePerformance = failures;
    QTemporaryDir diagnostics;
    profiler.setReportDirectory(diagnostics.path());
    check(!profiler.visible() && !profiler.recording() && !backend.scene.isProfiling(),
          "performance sampling is opt-in");
    backend.action("settings");
    refresh();
    capture("performance-settings-top");
    check(!hasAction("performance:toggle"), "offscreen performance entry has no input hit");
    const auto settingsBounds = hud.modalBounds(width);
    for (int index = 0; index < 24 && hud.settingsScroll < hud.settingsScrollMaximum(); ++index) {
        wheel(settingsBounds.center().x(), settingsBounds.top() + 500, index % 2 == 0);
    }
    check(hud.settingsScroll > 0 && hasAction("performance:toggle"),
          "settings native wheel exposes performance entry");
    capture("performance-settings-scrolled");
    hud.settingsScroll = hud.settingsScrollMaximum();
    refresh();
    check(hasAction("update:check") && hasAction("update:auto") && hasAction("update:release"),
          "settings scroll exposes software update controls");
    check(!hasAction("update:download"), "download requires a validated newer release");
    check(click("update:check") && backend.host.updater.status().contains("离线"),
          "update button reports offline mode without a request");
    capture("software-update-settings");
    check(click("performance:toggle") && profiler.visible(),
          "settings performance button opens native monitor");
    backend.action("close-modal");
    refresh();
    check(profiler.startRecording(), "performance recording starts");
    wait.restart();
    while (wait.elapsed() < 650) {
        settle();
    }
    const auto sampledMetrics = profiler.metrics();
    check(sampledMetrics.value("mainProcessResidentBytes").toLongLong() > 0,
          "performance reports actual main-process memory");
    check(sampledMetrics.value("qtFps").toDouble() >= 0 && sampledMetrics.contains("gapP95Ms"),
          "performance reports actual present counters");
    check(sampledMetrics.value("gpuTimeMs").isNull(), "CPU timing is never presented as GPU timing");
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/performance-panel.png");
    }
    check(profiler.saveRecording(), "performance recording exports atomically");
    const auto reports = QDir(diagnostics.path()).entryList({"*.json"}, QDir::Files);
    check(reports.size() == 1, "one manual performance diagnostic exported");
    if (!reports.isEmpty()) {
        QFile report(diagnostics.filePath(reports.first()));
        check(report.open(QIODevice::ReadOnly), "performance diagnostic opens");
        const auto bytes = report.readAll();
        const auto document = QJsonDocument::fromJson(bytes).object();
        const auto samples = document["samples"].toArray();
        check(document["schema"].toInt() == 1 && !samples.isEmpty(), "diagnostic has samples and schema");
        check(!document["environment"].toObject()["graphicsApi"].toString().isEmpty(),
              "diagnostic identifies graphics backend");
        for (const auto &entry : samples) {
            check(!entry.toObject().contains("query") && !entry.toObject().contains("current") &&
                      !entry.toObject().contains("cookies"),
                  "diagnostic excludes business state");
        }
        if (!output.isEmpty()) {
            QFile savedReport(output + "/performance-report.json");
            if (savedReport.open(QIODevice::WriteOnly)) {
                savedReport.write(bytes);
            }
        }
    }
    window->hide();
    settle();
    check(!backend.scene.isProfiling(), "hidden performance panel stops live sampling");
    window->show();
    settle();
    check(backend.scene.isProfiling(), "visible performance panel resumes live sampling");
    profiler.toggleVisible();
    settle();
    check(!backend.scene.isProfiling(), "closing performance panel disables native timing");
    // 少于半秒的录制也必须写入最终一次采样。
    check(profiler.startRecording() && profiler.saveRecording(), "short performance recording retained");
    // 模拟录制期间目录被移走，保留待导出的记录。
    const auto missingDirectory = diagnostics.filePath("removed");
    QDir().mkpath(missingDirectory);
    profiler.setReportDirectory(missingDirectory);
    check(profiler.startRecording(), "performance error-path recording starts");
    QDir(missingDirectory).removeRecursively();
    check(!profiler.saveRecording() && !profiler.recording(), "failed export retains pending diagnostic");
    profiler.setReportDirectory(diagnostics.path());
    check(profiler.saveRecording(), "failed performance export can be retried");
    profiler.setReportDirectory(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                                "/RHINE Diagnostics");
    if (failures == failuresBeforePerformance) {
        qInfo() << "Native performance settings, sampling, export and retry checks passed";
    }
    // 独立动画夹具放在既有固定时间检查之后，避免较晚的时钟影响旧 HUD 曲线。
    hud.modal.clear();
    hud.reduced = false;
    backend.setVisible(false);
    backend.scene.review("archive");
    backend.scene.reduced(false);
    music.directory.reset();
    music.current = {};
    player.queue = followSongs;
    player.current = followSongs.last().toObject();
    music.followPlayback(player.current);
    for (const bool enhanced : {false, true}) {
        hud.sceneEnhancement = enhanced;
        backend.scene.setMode("detail");
        bool sawFade = false;
        bool blockedEarlyComposite = true;
        for (int index = 1; index <= 240; ++index) {
            backend.step(60 + int(enhanced) * 10 + index / 60.0);
            if (enhanced) {
                const double blend = backend.frame()["originalActorBlend"].toDouble();
                sawFade |= blend > 0 && blend < 1;
                if (index < 30) {
                    blockedEarlyComposite &= blend == 0;
                }
                if (index == 30 || index == 110 || index == 240) {
                    settle();
                    capture("extraction-" + QString::number(index));
                }
            }
        }
        if (enhanced) {
            check(blockedEarlyComposite && sawFade && backend.scene.originalActorBlend() == 1,
                  "original actor fades in only after extraction clears neighboring shells");
        }
        const QJsonValue selectedCell = backend.frame()["stats"].toObject()["selectedCell"];
        const QJsonValue selectedKey = music.current["key"];
        const auto selectedIndex = music.directory->index;
        backend.action("back");
        bool retained = true;
        for (int index = 1; index <= 180; ++index) {
            backend.step(64 + int(enhanced) * 10 + index / 60.0);
            retained &= music.current["key"] == selectedKey && music.directory->index == selectedIndex &&
                        backend.frame()["stats"].toObject()["selectedCell"] == selectedCell;
        }
        check(retained && backend.frame()["mode"] == "archive",
              enhanced ? "enhanced return keeps current song and physical cell throughout animation"
                       : "original return keeps current song and physical cell throughout animation");
    }
    hud.sceneEnhancement = false;
    backend.step(78);
    settle();
    if (ok) {
        qInfo() << "Native QML scrolling, paging and search input checks passed";
    }
    ok &= runSceneEnhancementChecks(backend, window, output.isEmpty() ? QString() : output + "/enhancement");
    return ok ? 0 : 1;
}
