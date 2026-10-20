#include "NativeApplication.h"
#include <cmath>

HudSceneActions NativeApplication::sceneActions(NativeScene *scene) {
    return {[scene](QString mode) { scene->setMode(mode); },
            [scene](bool dark) { scene->theme(dark); },
            [scene](bool reduced) { scene->reduced(reduced); },
            [scene](QString type, double x, double y, double delta) { scene->pointer(type, x, y, delta); },
            [scene](QString action) { scene->viewerAction(action); },
            [scene](QString key) { scene->viewerKey(key); },
            [scene]() { scene->openViewer(); },
            [scene](bool front) { scene->coverFront(front); }};
}

/** 建立原生音乐、HUD、场景的值接口；所有异步失效统一唤醒 Qt 帧调度。 */
NativeApplication::NativeApplication(Host *host, Glyphs *glyphs, NativeScene *scene, QJsonObject content,
                                     QObject *parent)
    : QObject(parent), music(host, content), hud(host, glyphs, &music, sceneActions(scene)), m_host(host),
      m_scene(scene) {
    connect(&music, &MusicLibrary::invalidated, host, &Host::invalidate);
    connect(&music, &MusicLibrary::selectionRequested, this,
            [scene](int index, QJsonObject navigation) { scene->select(index, navigation); });
    connect(&music, &MusicLibrary::modeRequested, this, [scene](QString mode) { scene->setMode(mode); });
    connect(&music, &MusicLibrary::navigationRequested, this,
            [scene](QString axis, int direction) { scene->navigate(axis, direction); });
    connect(&music, &MusicLibrary::musicIdentityRequested, this,
            [scene](QString key) { scene->musicIdentity(key); });
    connect(&music, &MusicLibrary::musicClearRequested, this, [scene]() { scene->musicClear(); });
}
void NativeApplication::initialize() {
    m_scene->theme(hud.dark);
    m_scene->reduced(hud.reduced);
    const auto saved = m_host->preference("volume");
    bool valid = false;
    const auto volume = saved.toDouble(&valid);
    music.player.volume(valid && std::isfinite(volume) ? volume : 0.7);
    music.restoreAccount();
    music.restoreIdentityName();
}
QJsonObject NativeApplication::step(double time, double width, bool reuseScene) {
    const auto viewChanged = m_scene->setTrackListView(hud.trackListMode && music.current["kind"] == "song");
    const auto frame = reuseScene && !viewChanged ? music.frame : m_scene->sample(time);
    if (frame.isEmpty()) {
        return {};
    }
    music.frame = frame;
    music.observe(frame["selected"].toInt());
    if (hud.tab != "overview" && music.wantsSongDetails && frame["mode"] == "detail") {
        music.sameAlbum();
    }
    hud.draw(frame, width, time);
    return {{"commands", hud.commands},
            {"hits", hud.hits},
            {"state", hud.state()},
            {"current", music.current},
            {"cover", music.coverImage}};
}
void NativeApplication::poll(double time, bool force) {
    music.player.tick(time, force);
}
void NativeApplication::mediaAction(QString action) {
    if (action == "resume" || action == "pause") {
        m_host->mediaControl(action);
    } else {
        hud.action("player:" + action);
    }
}

void NativeApplication::pointer(QString type, double x, double y) {
    if (!hud.modal.isEmpty()) {
        // 弹窗外只接受完整点击，拖动、取消和面板内部留白都不触发返回。
        if (type == "pointerdown") {
            m_dragging = true;
            m_down = {x, y};
            m_moved = false;
        } else if (type == "pointermove" && m_dragging && std::hypot(x - m_down.x(), y - m_down.y()) > 6) {
            m_moved = true;
        } else if (type == "pointerup") {
            const auto bounds = hud.modalBounds(hud.width).translated(0, hud.motion.offset);
            const bool isClick = !m_moved && std::hypot(x - m_down.x(), y - m_down.y()) <= 6;
            if (m_dragging && isClick && !bounds.contains(m_down) && !bounds.contains(x, y) && !hud.closing) {
                hud.closeModal();
            }
            m_dragging = false;
        } else if (type == "pointercancel" || type == "pointerleave") {
            m_dragging = false;
        }
        return;
    }
    if (type == "pointerdown") {
        m_dragging = true;
        m_down = {x, y};
        m_moved = false;
    }
    if (type == "pointermove" && m_dragging && std::hypot(x - m_down.x(), y - m_down.y()) > 6) {
        m_moved = true;
    }
    if (type == "pointerup" && m_dragging) {
        const auto picked = m_scene->pick(x, y);
        m_scene->pointer(type, x, y);
        m_dragging = false;
        const bool isClick = !m_moved && std::hypot(x - m_down.x(), y - m_down.y()) <= 6;
        if (isClick && !hud.frame["viewer"].toBool() && hud.frame["mode"] == "detail") {
            const auto bounds = hud.detailBounds(hud.width);
            const bool isInspectingSong =
                music.current["kind"] == "song" && picked && m_scene->isSelectedCell(*picked);
            // 详情卡片外的完整点击复用返回路径；封面检查和拖动继续交给场景。
            if (!bounds.contains(m_down) && !bounds.contains(x, y) && !isInspectingSong) {
                hud.action("back");
            }
        } else if (isClick && hud.frame["mode"] == "archive" && picked) {
            music.observe(m_scene->selection());
            if (!music.current.isEmpty() && music.current["kind"] != "song") {
                music.enterDirectory(music.current);
            }
        }
        return;
    }
    m_scene->pointer(type, x, y);
}
