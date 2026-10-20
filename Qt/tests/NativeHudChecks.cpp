#include "NativeHud.h"
#include "MusicData.h"
#include "MusicDiscover.h"
#include <QDebug>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPainter>
#include <QTemporaryDir>

namespace {
QJsonObject song(int index) {
    auto value =
        MusicData::item("song", QString::number(index), "本地曲目 " + QString::number(index), "测试歌手");
    value["album"] = "测试专辑";
    value["albumID"] = "42";
    value["duration"] = 180000;
    value["wiki"] = "第一段百科\n第二段百科\n第三段百科\n第四段百科";
    value["artists"] = QJsonArray{QJsonObject{{"id", "7"}, {"name", "测试歌手"}}};
    return value;
}
QImage paint(Glyphs &glyphs, const QJsonArray &commands) {
    QImage image(1920, 1080, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    glyphs.commands(painter, commands, [](QString) { return QImage(); });
    return image;
}
}

/** 对照迁移前冻结的命中与轮廓像素，验收不需要保留旧脚本或额外运行时。 */
bool runNativeHudChecks(const QString &root, Glyphs &glyphs, const QString &fixturePath) {
    QFile fixture(fixturePath);
    if (!fixture.open(QIODevice::ReadOnly)) {
        return false;
    }
    QHash<QString, QJsonObject> references;
    for (const auto &value : QJsonDocument::fromJson(qUncompress(fixture.readAll())).array()) {
        const auto entry = value.toObject();
        references.insert(entry["provider"].toString() + ":" + entry["panel"].toString() +
                              (entry["dark"].toBool() ? ":dark" : ":light"),
                          entry["expected"].toObject());
    }
    Host host(root, true);
    const auto content = QJsonDocument::fromJson(host.read("slots.json").toUtf8()).object();
    bool ok = true;
    int count = 0;
    for (const auto &provider : {QString("netease"), QString("qq"), QString("kugou")}) {
        for (const bool dark : {false, true}) {
            for (const auto &panel :
                 {QString("archive"), QString("detail"), QString("container"), QString("viewer"),
                  QString("search"), QString("saved"), QString("settings"), QString("provider"),
                  QString("login"), QString("player"), QString("discover"), QString("playlist-picker"),
                  QString("playlist-form"), QString("confirm")}) {
                MusicLibrary music(&host, content);
                music.provider = provider;
                music.api.provider = provider;
                music.uid = "42";
                music.nickname = "测试账号";
                music.status = "离线固定时间校验";
                music.keyword = "本地";
                music.loginStatus = "等待确认";
                music.coverImage.clear();
                music.likedIDs = QJsonArray{"1"};
                QJsonArray songs;
                for (int index = 1; index <= 20; ++index) {
                    songs.append(song(index));
                }
                music.current = songs.first().toObject();
                music.tracks = songs;
                for (int lane = 0; lane < 5; ++lane) {
                    music.lists[lane] = songs;
                    music.totals[lane] = songs.size();
                    music.hasMore[lane] = false;
                }
                music.player.queue = songs;
                music.player.current = music.current;
                music.player.index = 2;
                music.player.state = {{"playing", true},
                                      {"position", 45678},
                                      {"duration", 180000},
                                      {"volume", 0.7},
                                      {"seekable", true}};
                music.player.status = "原生播放器";
                music.player.quality = "exhigh";
                music.player.alternative = false;
                music.discover->items = songs;
                music.discover->status = "本地发现";
                music.discover->more = true;
                QJsonObject frame{{"mode", "archive"}, {"selected", 0}, {"detail", 1}, {"viewer", false}};
                QString modal = panel;
                if (QStringList{"archive", "detail", "container", "viewer"}.contains(panel)) {
                    modal.clear();
                }
                if (panel == "detail" || panel == "container") {
                    frame["mode"] = "detail";
                }
                if (panel == "viewer") {
                    frame["viewer"] = true;
                }
                if (panel == "container") {
                    auto item = MusicData::item("playlist", "8", "本地歌单", "测试账号");
                    item["creatorID"] = "42";
                    item["trackCount"] = 20;
                    music.current = item;
                }
                HudSceneActions scene;
                scene.theme = [](bool) {};
                NativeHud hud(&host, &glyphs, &music, scene);
                hud.modal = modal;
                hud.dark = dark;
                hud.themeMode = dark ? "dark" : "light";
                hud.query = "本地";
                hud.formKind = "create";
                hud.confirmTitle = "删除歌单：本地歌单";
                hud.motion.set(true, 0, true);
                const auto key = provider + ":" + panel + (dark ? ":dark" : ":light");
                auto expected = references.value(key);
#ifndef Q_OS_MACOS
                // 固定帧以 macOS 的修饰键录制，其他平台渲染同一份动作时换成 Ctrl。
                auto commands = expected["commands"].toArray();
                for (int index = 0; index < commands.size(); ++index) {
                    auto command = commands.at(index).toArray();
                    if (command.at(0) == "text") {
                        command[1] = command.at(1).toString().replace("⌘", "Ctrl");
                        commands[index] = command;
                    }
                }
                expected["commands"] = commands;
#endif
                // 列切换右箭头为避免压住标签右移 77；只调整这一项冻结几何。
                auto revisedCommands = expected["commands"].toArray();
                for (int index = 0; index < revisedCommands.size(); ++index) {
                    auto command = revisedCommands.at(index).toArray();
                    if (command.at(0) == "text" && command.at(1) == "→" &&
                        command.at(2).toDouble() == 1920 * 0.5234375 + 233 &&
                        command.at(3).toDouble() == 908) {
                        command[2] = command.at(2).toDouble() + 77;
                        revisedCommands[index] = command;
                    }
                }
                expected["commands"] = revisedCommands;
                auto revisedHits = expected["hits"].toArray();
                for (int index = 0; index < revisedHits.size(); ++index) {
                    auto hit = revisedHits.at(index).toObject();
                    if (hit["action"] == "column-next") {
                        hit["x"] = hit["x"].toDouble() + 77;
                        revisedHits[index] = hit;
                    }
                }
                expected["hits"] = revisedHits;
                hud.draw(frame, 1920, 50);
                const auto expectedCommands = expected["commands"].toArray();
                const auto expectedHits = expected["hits"].toArray();
                auto actualHits = hud.hits;
                auto actualPixels = paint(glyphs, hud.commands);
                auto expectedPixels = paint(glyphs, expectedCommands);
                // 新按钮单独由真实窗口回归验收；冻结对照只跳过其明确占用的小区域。
                for (int index = actualHits.size() - 1; index >= 0; --index) {
                    if (actualHits.at(index).toObject()["action"] == "archive:tab-mode") {
                        actualHits.removeAt(index);
                        for (auto *image : {&actualPixels, &expectedPixels}) {
                            QPainter painter(image);
                            painter.setCompositionMode(QPainter::CompositionMode_Source);
                            painter.fillRect(QRect(1572, 854, 194, 48), Qt::transparent);
                        }
                    }
                }
                if (panel == "settings") {
                    // 新增滚动轨道位于原留白处；其余设置行继续对照迁移前的尺寸、位置和像素。
                    actualHits = {};
                    for (const auto &value : hud.hits) {
                        if (!value.toObject()["action"].toString().startsWith("settings:scroll-")) {
                            actualHits.append(value);
                        }
                    }
                    const auto bounds = hud.modalBounds(1920);
                    const QRect rail(int(bounds.right() - 35),
                                     int(bounds.top() + NativeHud::SettingsViewportTop), 28,
                                     int(NativeHud::SettingsViewportBottom - NativeHud::SettingsViewportTop));
                    for (auto *image : {&actualPixels, &expectedPixels}) {
                        QPainter painter(image);
                        painter.setCompositionMode(QPainter::CompositionMode_Source);
                        painter.fillRect(rail, Qt::transparent);
                    }
                }
                const bool sameHits = actualHits == expectedHits;
                const bool samePixels = actualPixels == expectedPixels;
                if (!sameHits || !samePixels) {
                    ok = false;
                    const auto name = provider + ":" + panel + (dark ? ":dark" : ":light");
                    qCritical().noquote()
                        << "Native HUD parity:" << name << "hits" << sameHits << "pixels" << samePixels;
                    const auto folder = qEnvironmentVariable("RHINE_HUD_DIFF");
                    if (!folder.isEmpty()) {
                        QDir().mkpath(folder);
                        const auto prefix = folder + "/" + QString(name).replace(':', '-');
                        QFile actual(prefix + "-actual.json");
                        if (actual.open(QIODevice::WriteOnly)) {
                            actual.write(
                                QJsonDocument(QJsonObject{{"commands", hud.commands}, {"hits", hud.hits}})
                                    .toJson());
                        }
                        QFile original(prefix + "-expected.json");
                        if (original.open(QIODevice::WriteOnly)) {
                            original.write(QJsonDocument(expected).toJson());
                        }
                        paint(glyphs, hud.commands).save(prefix + "-actual.png");
                        paint(glyphs, expectedCommands).save(prefix + "-expected.png");
                    }
                }
                if (panel == "settings") {
                    const auto bounds = hud.modalBounds(1920);
                    hud.wheel(bounds.center().x(), bounds.top() + 500, -120, 1, 10000.);
                    hud.draw(frame, 1920, 50);
                    bool performanceVisible = false;
                    bool headerFixed = false;
                    bool clipped = true;
                    for (const auto &value : hud.hits) {
                        const auto hit = value.toObject();
                        const auto action = hit["action"].toString();
                        if (action == "close-modal") {
                            headerFixed = hit["y"].toDouble() == bounds.top() + 18;
                        } else if (!action.startsWith("settings:scroll-") && !action.startsWith("player:")) {
                            clipped &= hit["y"].toDouble() >= bounds.top() + NativeHud::SettingsViewportTop;
                            clipped &= hit["y"].toDouble() + hit["height"].toDouble() <=
                                       bounds.top() + NativeHud::SettingsViewportBottom;
                        }
                        performanceVisible |= action == "performance:toggle";
                    }
                    const bool atEnd = hud.settingsScroll == hud.settingsScrollMaximum();
                    hud.wheel(bounds.center().x(), bounds.top() + 500, 120, -1, -10000.);
                    const bool atStart = hud.settingsScroll == 0;
                    hud.action("settings:scroll-down");
                    hud.openModal("settings");
                    const bool reset = hud.settingsScroll == 0;
                    if (!performanceVisible || !headerFixed || !clipped || !atEnd || !atStart || !reset) {
                        qCritical() << "Settings scroll regression" << provider << performanceVisible
                                    << headerFixed << clipped << atEnd << atStart << reset;
                        ok = false;
                    }
                }
                ++count;
            }
        }
    }
    qInfo() << "Native HUD fixed-frame panels:" << count << (ok ? "passed" : "FAILED");
    return ok;
}

#ifdef RHINE_HUD_REGRESSION_TEST_MAIN
int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    const auto root = application.arguments().value(1);
    Glyphs glyphs(root);
    return runNativeHudChecks(root, glyphs, application.arguments().value(2)) ? 0 : 1;
}
#endif
