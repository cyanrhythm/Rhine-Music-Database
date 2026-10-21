#include "NativeHud.h"
#include "MusicData.h"
#include <algorithm>

namespace {
struct Provider {
    QString id;
    QString name;
    QString label;
    QString detail;
};
const QVector<Provider> providers{
    {"netease", "网易云音乐", "NETEASE", "搜索 · 个人音乐 · 推荐 · 播放 · 歌单编辑"},
    {"qq", "QQ 音乐", "QQ MUSIC", "全类搜索 · 每日推荐 · 个人音乐 · 歌单编辑"},
    {"kugou", "酷狗音乐", "KUGOU", "标准版 / 概念版 · 个人歌单 · 推荐与榜单 · 播放"},
    {"local", "本地音乐", "LOCAL", "文件夹管理 · 导入与索引 · 本地检索 · 播放"}};

QString modalTitle(const QString &modal, const MusicLibrary &music) {
    static const QHash<QString, QString> titles{{"provider", "MUSIC PLATFORM"},
                                                {"settings", "SYSTEM SETTINGS"},
                                                {"quality", "AUDIO QUALITY"},
                                                {"disc-rotation-warning", "DISC ROTATION"},
                                                {"saved", "MY MUSIC"},
                                                {"player", "NOW PLAYING"},
                                                {"discover", "DISCOVER MUSIC"},
                                                {"playlist-form", "PLAYLIST EDITOR"},
                                                {"playlist-picker", "PLAYLIST DIRECTORY"},
                                                {"confirm", "CONFIRM DELETE"}};
    if (modal == "local") {
        return "LOCAL MUSIC LIBRARY";
    }
    if (modal == "login") {
        if (music.provider == "qq") {
            return "QQ MUSIC LOGIN";
        }
        if (music.provider == "kugou") {
            return music.kugouEdition == "lite" ? "KUGOU CONCEPT LOGIN" : "KUGOU LOGIN";
        }
        return "NETEASE LOGIN";
    }
    return titles.value(modal, "MUSIC INDEX");
}
QString modalSubtitle(const QString &modal, const MusicLibrary &music) {
    static const QHash<QString, QString> subtitles{{"provider", "直接选择音乐平台 / 各平台账号独立保存"},
                                                   {"settings", "终端偏好设置"},
                                                   {"quality", "声音档案 / 播放音质"},
                                                   {"disc-rotation-warning", "开启前请确认资源与功耗风险"},
                                                   {"player", "播放队列 / QUEUE"},
                                                   {"discover", "推荐 / 排行榜 / 新专辑"},
                                                   {"playlist-form", "编辑我的歌单"},
                                                   {"playlist-picker", "添加到我的歌单"},
                                                   {"confirm", "删除确认"}};
    if (modal == "local") {
        return "文件夹管理 / 导入与索引";
    }
    if (modal == "search" && music.isLocal()) {
        return "本地音乐检索 / 仅搜索已导入的索引";
    }
    if (modal == "saved") {
        return "我的音乐 / " + music.nickname;
    }
    if (modal == "login") {
        return music.provider == "kugou" ? "使用对应版本酷狗 App 扫码；搜索与个人音乐需要登录"
                                         : "可选登录，公开搜索不需要账号";
    }
    return subtitles.value(modal, "音乐检索");
}
}

/** 弹窗共用一次绘制栈与命中表，关闭曲线结束后才释放当前面板。 */
void NativeHud::modalDraw(double width) {
    auto &music = *m_music;
    save();
    alpha(motion.opacity);
    if (modal == "lyrics-source") {
        const auto bounds = modalBounds(width);
        translate(0, motion.offset);
        fill(bounds.x(), bounds.y(), bounds.width(), bounds.height(),
             dark ? QJsonArray{0.15, 0.15, 0.15, 1, "calibratedWhite"} : QJsonArray{0.929, 0.922, 0.894, 1});
        hits = {};
        m_modalControls = true;
        const QStringList sources{"auto", "local", "netease", "qq", "kugou"};
        const QStringList labels{"自动（本地优先）", "本地歌词", "网易云音乐", "QQ 音乐", "酷狗音乐"};
        for (int index = 0; index < sources.size(); ++index) {
            const auto label =
                (music.player.lyrics.localSource == sources[index] ? QStringLiteral("✓ ") : QString()) +
                labels[index];
            button(label, bounds.x() + 10, bounds.y() + 11 + index * 42, bounds.width() - 20, 42,
                   "player:lyrics-source:" + sources[index], 15);
        }
        m_modalControls = false;
        restore();
        return;
    }
    const bool search = modal == "search";
    fill(0, 0, width, 1080,
         dark ? QJsonArray{17.0 / 255, 24.0 / 255, 27.0 / 255, search ? 0.15 : 0.6}
              : QJsonArray{227.0 / 255, 224.0 / 255, 215.0 / 255, search ? 0.12 : 0.45});
    translate(0, motion.offset);
    const auto bounds = modalBounds(width);
    const auto x = bounds.x();
    const auto y = bounds.y();
    const auto usable = bounds.width();
    fill(x, y, usable, 836,
         dark ? QJsonArray{0.15, 0.15, 0.15, 1, "calibratedWhite"} : QJsonArray{0.929, 0.922, 0.894, 1});
    hits = {};
    m_modalControls = true;
    QString category = "MUSIC DIRECTORY";
    if (modal == "settings") {
        category = "SYSTEM PREFERENCES";
    } else if (modal == "player") {
        category = "AUDIO TERMINAL";
    } else if (modal == "quality") {
        category = "AUDIO SPECIFICATION";
    }
    text("RHINE LAB / MUSIC DATABASE / " + category, x + 53, y + 37, 11);
    button("← 返回", x + usable - 220, y + 18, 167, 54, "close-modal", 20, "demibold");
    text(modalTitle(modal, music), x + 53, y + 91, 38, "demibold");
    text(modalSubtitle(modal, music), x + 53, y + 147, 15);
    if (modal == "disc-rotation-warning") {
        text("唱片旋转需要持续刷新画面。", x + 53, y + 250, 22, "demibold");
        text("开启后会增加 CPU / GPU 占用、耗电和发热，可能缩短电池续航。", x + 53, y + 310, 16);
        text("仅当前歌曲的详情页和歌词页旋转；资源库、暂停和后台均不旋转。", x + 53, y + 352, 16);
        text("该功能默认关闭，减少动态效果开启时也不会旋转。", x + 53, y + 394, 16);
        button("了解风险，仍然开启", x + 53, y + 480, 260, 48, "disc-rotation:enable", 16);
        button("保持关闭", x + 340, y + 480, 180, 48, "disc-rotation:cancel", 16);
    } else if (modal == "quality") {
        qualityPanel(x, y, usable);
    } else if (modal == "provider") {
        platformPanel(x, y, usable);
    } else if (modal == "confirm") {
        limited(confirmTitle, x + 53, y + 260, 24, usable - 106, "demibold");
        text(music.isLocal() ? "只移除本地索引，磁盘音乐文件保持原样。"
                             : "此操作将同步至" + music.providerName() + "。",
             x + 53, y + 330, 16);
        button("确认删除", x + 53, y + 450, 180, 45, "confirm:yes", 16);
        button("取消", x + 270, y + 450, 150, 45, "close-modal", 16);
    } else if (modal == "playlist-picker") {
        text("选择我的歌单", x + 53, y + 240, 18);
        if (music.provider == "qq" &&
            std::none_of(music.lists[2].begin(), music.lists[2].end(),
                         [&music](QJsonValue value) { return music.canEditPlaylist(value.toObject()); })) {
            text("暂无可编辑的自建普通歌单，请先创建歌单。", x + 53, y + 320, 16);
        }
        for (int index = 0; index < 7 && listOffset + index < music.lists[2].size(); ++index) {
            const auto value = music.lists[2].at(listOffset + index).toObject();
            if (music.provider == "qq" && !music.canEditPlaylist(value)) {
                continue;
            }
            button(fit(value["title"].toString(), 18, usable - 106), x + 53, y + 290 + index * 60,
                   usable - 106, 52, "music:playlist-pick:" + value["id"].toString(), 18);
        }
        button("↑ 上一页", x + usable - 330, y + 758, 120, 32, "music:picker-prev", 12);
        button("下一页 ↓", x + usable - 190, y + 758, 120, 32, "music:picker-next", 12);
        limited(music.status, x + 53, y + 775, 12, usable - 420);
    } else if (modal == "playlist-form") {
        text(formKind == "create" ? "歌单名称" : "新歌单名称", x + 53, y + 240, 15);
        text(music.provider == "qq" ? "1–20 字 / 按 ENTER 提交" : "按 ENTER 提交", x + 53, y + 420, 12);
        limited(music.status, x + 53, y + 775, 12, usable - 106);
    } else if (modal == "local") {
        localPanel(x, y, usable);
    } else if (modal == "discover") {
        discoverPanel(x, y, usable);
    } else if (modal == "player") {
        playerPanel(x, y, usable);
    } else if (modal == "settings") {
        settings(x, y, usable);
    } else if (modal == "login") {
        loginPanel(x, y, usable);
    } else {
        libraryPanel(x, y, usable);
    }
    m_modalControls = false;
    restore();
}

void NativeHud::platformChoices(double x, double y, double width, bool expanded) {
    constexpr double gap = 12;
    const auto cardWidth = (width - gap * (providers.size() - 1)) / providers.size();
    for (int index = 0; index < providers.size(); ++index) {
        const auto &provider = providers.at(index);
        const auto left = x + index * (cardWidth + gap);
        const bool selected = m_music->provider == provider.id;
        const auto height = expanded ? 190 : 66;
        const auto tint = dark ? QJsonArray{0.55, 0.59, 0.46, selected ? 0.3 : 0.12}
                               : QJsonArray{0.44, 0.49, 0.36, selected ? 0.23 : 0.09};
        fill(left, y, cardWidth, height, tint);
        hit(left, y, cardWidth, height, "provider:" + provider.id, provider.name);
        if (selected) {
            fill(left, y, 3, height, dark ? QJsonArray{0.7, 0.75, 0.53, 1} : QJsonArray{0.44, 0.49, 0.36, 1});
        }
        text(provider.label + (selected ? " / ACTIVE" : ""), left + 16, y + 10, 10);
        text(provider.name, left + 16, y + 29, expanded ? 22 : 17, "demibold");
        if (expanded) {
            const auto lines = m_glyphs->wrap(provider.detail, cardWidth - 32, 14);
            for (int row = 0; row < std::min(3, int(lines.size())); ++row) {
                text(lines.at(row), left + 16, y + 75 + row * 21, 14);
            }
            const bool authenticated = m_host->hasCredential(provider.id);
            const auto note = provider.id == "local"
                                  ? QStringLiteral("独立本地库 / 无需登录")
                                  : (authenticated ? QStringLiteral("本机已保存登录")
                                                   : QStringLiteral("未登录 / 可选择后扫码"));
            limited(note, left + 16, y + 150, 12, cardWidth - 32);
        }
    }
}
void NativeHud::platformPanel(double x, double y, double width) {
    text("PROVIDER ACCESS / 音乐平台", x + 53, y + 222, 13, "demibold");
    platformChoices(x + 53, y + 258, width - 106, true);
    rule(x + 53, y + 490, width - 106, 0.2);
    text("切换平台会停止当前播放，各平台账号与个人音乐独立保存。", x + 53, y + 520, 15);
    text("网易云与 QQ 支持歌单编辑；酷狗支持读取与播放。", x + 53, y + 558, 15);
    if (m_music->isLocal()) {
        button("文件夹管理 ＋", x + 53, y + 660, 330, 48, "discover", 16);
    } else {
        button("账号登录 / " + m_music->providerName(), x + 53, y + 660, 330, 48, "music:login", 16);
    }
}
namespace {
struct QualityDescription {
    QString title;
    QString caption;
    QString detail;
    QString badge;
};
const QHash<QString, QualityDescription> qualityDescriptions{
    {"standard", {"标准", "STANDARD", "日常聆听，兼顾音质与数据流量", ""}},
    {"higher", {"较高", "HIGH QUALITY", "提升声音细节，保持轻量传输", "hq"}},
    {"exhigh", {"极高", "HIGH QUALITY", "更充分保留有损编码中的声音细节", "hq"}},
    {"lossless", {"无损", "LOSSLESS / SQ", "无损编码，保留更完整的录音细节", "sq"}},
    {"ape", {"无损 / APE", "LOSSLESS / APE", "以 APE 格式提供无损音源", "sq"}},
    {"hires", {"高解析无损", "HI-RES AUDIO", "还原高解析录音中的丰富层次", "hires"}},
    {"jymaster", {"超清母带", "MASTER AUDIO", "聆听母带版本中的细腻质感与动态", "jymaster"}},
    {"best", {"自动最高可用", "BEST AVAILABLE", "由平台选择当前可用的最高音质", ""}}};
}

/** 音质选项采用统一档案标记；重点档位展开为卡片，普通档位保持紧凑行高。 */
void NativeHud::qualityChoice(const QString &id, int number, double x, double y, double width,
                              bool featured) {
    const auto description = qualityDescriptions.value(id);
    const bool selected = m_music->player.quality == id;
    const auto height = featured ? 174. : 57.;
    const auto accent = dark ? QJsonArray{0.69, 0.74, 0.53, 1} : QJsonArray{0.43, 0.48, 0.34, 1};
    auto tint = accent;
    tint[3] = selected ? 0.18 : 0.055;
    if (featured || selected) {
        fill(x, y, width, height, tint);
    }
    rule(x, y + height, width, selected ? 0.6 : 0.13);
    if (selected) {
        fill(x, y, 3, height, accent);
    }
    hit(x, y, width, height, "quality:set:" + id, description.title);
    const auto index = QString::number(number).rightJustified(2, '0');
    const auto key = "quality:" + m_music->provider + "-" + description.badge;
    const auto pixels = description.badge.isEmpty() ? QImage() : m_host->image(key);
    const auto badgeX = featured ? x + width - 104 : x + 30;
    const auto badgeY = featured ? y + 19 : y + 16;
    if (!pixels.isNull()) {
        const auto badgeWidth = std::min(68., 21. * pixels.width() / pixels.height());
        const auto badgeHeight = badgeWidth * pixels.height() / pixels.width();
        if (m_music->provider == "kugou") {
            fill(badgeX - 4, badgeY - 4, badgeWidth + 8, badgeHeight + 8, {0.30, 0.33, 0.26, 1});
        }
        image(key, badgeX, badgeY, badgeWidth, badgeHeight);
    } else if (!featured) {
        QString label = "STD";
        if (id == "best") {
            label = "AUTO";
        } else if (description.badge == "hq") {
            label = "HQ";
        }
        text(label, x + 30, y + 18, 11, "demibold");
    }
    if (featured) {
        text(index + " / " + description.caption, x + 22, y + 22, 11, "demibold", 0.8);
        text(description.title, x + 22, y + 62, 26, "demibold");
        limited(description.detail, x + 22, y + 106, 13, width - 44);
        text(selected ? "✓ 已选择" : "选择此档 →", x + 22, y + 143, 12, "demibold");
    } else {
        text(index, x + 10, y + 20, 9);
        text(description.title, x + 112, y + 6, 17, "demibold");
        limited(description.detail, x + 112, y + 32, 12, width - 256);
        text(selected ? "✓ 已选择" : description.caption, x + width - 134, y + 20, 10, "demibold");
    }
}

/** 将播放偏好与实际音源分开呈现；卡片与设置共用播放器的音质切换入口。 */
void NativeHud::qualityPanel(double x, double y, double width) {
    const auto &music = *m_music;
    const auto &player = music.player;
    const auto left = x + 53;
    const auto usable = width - 106;
    const QHash<QString, QString> names{{"standard", "标准"},     {"hq", "高品质 / HQ"},
                                        {"sq", "无损 / SQ"},      {"hires", "高解析无损 / Hi-Res"},
                                        {"jymaster", "超清母带"}, {"jyeffect", "高清臻音"},
                                        {"sky", "沉浸环绕声"}};
    QString actual = names.value(player.actualQuality, "尚未确认");
    if (player.failed) {
        actual = "音源不可用";
    } else if (player.loading) {
        actual = "正在获取音源…";
    } else if (player.trial) {
        actual = "试听片段";
    }
    const auto accent = dark ? QJsonArray{0.69, 0.74, 0.53, 1} : QJsonArray{0.43, 0.48, 0.34, 1};
    fill(left, y + 195, 3, 48, accent);
    text("实际音源 / SOURCE", left + 17, y + 199, 10, "demibold", 0.6);
    text(actual, left + 17, y + 219, 18, "demibold");
    const auto preference = qualityDescriptions.value(player.quality).title;
    limited("已选偏好：" + preference, left + usable * 0.52, y + 219, 14, usable * 0.48);
    rule(left, y + 257, usable, 0.25);
    QStringList featured{"jymaster", "hires"};
    if (music.provider == "qq") {
        featured = {"best", "lossless"};
    } else if (music.provider == "kugou") {
        featured = {"hires", "lossless"};
    }
    const auto cardWidth = (usable - 18) / 2;
    for (int index = 0; index < featured.size(); ++index) {
        qualityChoice(featured.at(index), index + 1, left + index * (cardWidth + 18), y + 278, cardWidth,
                      true);
    }
    const auto options = MusicPlayer::qualityOptions(music.provider);
    int row = 0;
    for (auto option = options.crbegin(); option != options.crend(); ++option) {
        if (!featured.contains(option->first)) {
            qualityChoice(option->first, row + 3, left, y + 474 + row * 58, usable, false);
            ++row;
        }
    }
    // 未开放手动选择的扩展音源仍展示官方图标名称，避免把图标当作额外可选档位。
    if (music.provider == "netease") {
        text("其他音源标识", left, y + 738, 11, "demibold");
        const QStringList extraBadges{"jyeffect", "sky"};
        for (int index = 0; index < extraBadges.size(); ++index) {
            const auto id = extraBadges.at(index);
            const auto key = "quality:" + music.provider + "-" + id;
            const auto pixels = m_host->image(key);
            const auto badgeX = left + 130 + index * 235;
            if (!pixels.isNull()) {
                const auto badgeWidth = 18. * pixels.width() / pixels.height();
                image(key, badgeX, y + 735, badgeWidth, 18);
                text(names.value(id), badgeX + badgeWidth + 12, y + 737, 12);
            }
        }
    }
    rule(left, y + 775, usable, 0.22);
    text("选择与设置同步 · 切换保留播放位置", left, y + 791, 12);
    limited("可用音质由歌曲与平台权益决定，以实际音源为准。", left + usable * 0.47, y + 791, 12,
            usable * 0.53);
}

void NativeHud::settings(double x, double y, double width) {
    const auto viewport =
        QRectF(x + 40, y + SettingsViewportTop, width - 80, SettingsViewportBottom - SettingsViewportTop);
    settingsScroll = std::clamp(settingsScroll, 0., settingsScrollMaximum());
    const auto firstHit = hits.size();
    const auto originalY = y;
    save();
    clip(viewport.x(), viewport.y(), viewport.width(), viewport.height());
    y -= settingsScroll;
    const auto &music = *m_music;
    const auto left = x + 53;
    const auto usable = width - 106;
    text("PROVIDER ACCESS / 音乐平台", left, y + 184, 12, "demibold");
    const auto tint = dark ? QJsonArray{1, 1, 1, 0.025} : QJsonArray{0, 0, 0, 0.025};
    fill(left, y + 210, usable, 66, tint);
    text("当前平台 / " + music.providerName(), left + 12, y + 233, 17, "demibold");
    button("切换平台 ▾", left + usable - 235, y + 225, 225, 36, "provider", 13);
    if (music.isLocal()) {
        button("文件夹管理 / 导入与索引", left, y + 290, 340, 38, "discover", 13);
    } else {
        button(music.uid.isEmpty() ? "登录 / " + music.providerName() : "退出登录 / " + music.nickname, left,
               y + 290, 340, 38, music.uid.isEmpty() ? "music:login" : "music:logout", 13);
    }
    const QHash<QString, QString> memberships{
        {"free", "免费账号"}, {"green", "绿钻会员"}, {"super", "超级会员"}, {"unknown", "会员状态未知"}};
    const auto accountNote = music.provider == "qq" && !music.uid.isEmpty()
                                 ? memberships.value(music.vipLevel, "会员状态未知")
                                 : QString("各平台账号独立保存");
    limited(accountNote, left + 365, y + 301, 12, usable - 365);
    text("TERMINAL PREFERENCES / 终端偏好", left, y + 348, 12, "demibold");
    static const QHash<QString, QString> qualities{
        {"standard", "标准"}, {"higher", "较高"},   {"exhigh", "极高"},       {"lossless", "无损"},
        {"hires", "Hi-Res"},  {"jymaster", "母带"}, {"best", "自动最高可用"}, {"ape", "APE 无损"}};
    QString theme = dark ? "暗色" : "亮色";
    if (themeMode == "system") {
        theme = "跟随系统";
    }
    QVector<QStringList> rows{{"COLOR THEME / 界面配色", theme, "theme"},
                              {"RhineLab Enhance", sceneEnhancement ? "开启" : "关闭", "scene-enhancement"},
                              {"REDUCED MOTION / 动态效果", reduced ? "减少动态效果" : "完整动效", "motion"},
                              {"AUDIO QUALITY / 音质", qualities.value(music.player.quality), "quality"}};
    if (music.provider == "netease") {
        rows.append({"ALTERNATE SOURCE / 替代音源",
                     music.player.alternative ? "开启（音质由来源决定）" : "关闭", "alternative"});
    }
    if (music.isLocal()) {
        rows.removeLast();
    } else {
        rows.append({"OFFICIAL PLAYER / 官方窗口", m_host->activates() ? "播放时显示" : "尽量保持后台",
                     "music:activate"});
    }
    for (int index = 0; index < rows.size(); ++index) {
        const auto &row = rows.at(index);
        const auto yy = y + 377 + index * 50;
        fill(left, yy, usable, 44, tint);
        text(row.at(0), left + 12, yy + 12, 15, "demibold");
        button(row.at(1), left + usable - 235, yy + 4, 225, 36, row.at(2), 13);
    }
    text("↑ ↓ 选曲 / ENTER 查看 / " + m_host->modifierKey() + " ENTER 播放 / ESC 返回", left, y + 704, 12);
    rule(left, y + 737, usable, 0.2);
    text("PROJECT / 项目与开源参考", left, y + 752, 10, "demibold");
    const QVector<QPair<QString, QString>> links{{"本项目 / Rhine Music Database ↗", "project-home"},
                                                 {"网易云音乐 API / api-enhanced ↗", "project-author"},
                                                 {"QQ 音乐 API / qq-music-api ↗", "project-player"},
                                                 {"酷狗音乐 API / KuGouMusicApi ↗", "project-kugou"},
                                                 {"界面原项目 / RhineLabUI ↗", "project-rhine"},
                                                 {"歌词参考 / LyricsX ↗", "project-lyricsx"},
                                                 {"格式移植 / LyricsKit ↗", "project-lyricskit"},
                                                 {"同步与匹配参考 / Lyrimuse ↗", "project-lyrimuse"}};
    const auto linkWidth = (usable - 280 - 12) / 2;
    for (int index = 0; index < links.size(); ++index) {
        button(links.at(index).first, left + (index % 2) * (linkWidth + 8), y + 775 + (index / 2) * 28,
               linkWidth, 25, "music:" + links.at(index).second, 11);
    }
    button("切换全屏 / F11", left + usable - 260, y + 775, 260, 36, "window:fullscreen", 13);
    button("REINITIALIZE SYSTEM ↻", left + usable - 260, y + 819, 260, 36, "replay", 13);
    rule(left, y + 911, usable, 0.2);
    text("PERFORMANCE / 性能与诊断", left, y + 928, 12, "demibold");
    fill(left, y + 960, usable, 44, tint);
    text("帧数显示 / 性能面板", left + 12, y + 972, 15, "demibold");
    button("显示 / 隐藏 · F10", left + usable - 235, y + 964, 225, 36, "performance:toggle", 13);
    text("在性能面板内手动收集、导出诊断文件。", left, y + 1021, 12);
    fill(left, y + 1064, usable, 44, tint);
    text("DISC ROTATION / 唱片旋转", left + 12, y + 1076, 15, "demibold");
    button(discRotation ? "开启 / 点击关闭" : "关闭 / 点击了解风险", left + usable - 235, y + 1068, 225, 36,
           "disc-rotation", 13);
    text("默认关闭；开启会增加持续渲染、耗电和发热。", left, y + 1125, 12);
    rule(left, y + 1170, usable, 0.2);
    text("SOFTWARE UPDATE / 软件更新", left, y + 1188, 12, "demibold");
    const auto &updater = m_host->updater;
    const auto buildLabel = RHINE_RELEASE_BUILD ? QString() : QString(" · 开发版");
    text("当前版本 / " + AppUpdater::currentVersion() + buildLabel, left, y + 1220, 15, "demibold");
    button(m_host->preference("auto-update") == "off" ? "启动检查 / 关闭" : "启动检查 / 开启",
           left + usable - 235, y + 1208, 225, 36, "update:auto", 13);
    limited(updater.status(), left, y + 1260, 13, usable);
    button(updater.busy() ? "取消" : "检查更新", left, y + 1293, 160, 36,
           updater.busy() ? "update:cancel" : "update:check", 13);
    if (!updater.busy() && updater.available()) {
        const bool downloaded = !updater.downloadedPath().isEmpty();
        button(downloaded ? "打开下载目录" : "下载新版本", left + 175, y + 1293, 200, 36,
               downloaded ? "update:open" : "update:download", 13);
    }
    button("发布说明 ↗", left + usable - 200, y + 1293, 200, 36, "update:release", 13);
    text("下载校验后退出并替换完整应用；Windows 请保留原 data 文件夹。", left, y + 1355, 12);
    restore();
    // QML 的透明输入层不会继承绘制裁切，必须同步裁剪命中区域，避免隐藏按钮拦截点击。
    QJsonArray clippedHits;
    for (qsizetype index = 0; index < hits.size(); ++index) {
        auto entry = hits.at(index).toObject();
        if (index >= firstHit) {
            const QRectF area(entry["x"].toDouble(), entry["y"].toDouble(), entry["width"].toDouble(),
                              entry["height"].toDouble());
            const auto visible = area.intersected(viewport);
            if (visible.isEmpty()) {
                continue;
            }
            entry["x"] = visible.x();
            entry["y"] = visible.y();
            entry["width"] = visible.width();
            entry["height"] = visible.height();
        }
        clippedHits.append(entry);
    }
    hits = clippedHits;
    const auto railX = x + width - 24;
    const auto railTop = originalY + SettingsViewportTop + 30;
    const auto railHeight = viewport.height() - 60;
    const auto thumbHeight = railHeight * viewport.height() / (SettingsContentBottom - SettingsViewportTop);
    fill(railX, railTop, 3, railHeight, dark ? QJsonArray{1, 1, 1, 0.1} : QJsonArray{0, 0, 0, 0.1});
    fill(railX, railTop + settingsScroll / settingsScrollMaximum() * (railHeight - thumbHeight), 3,
         thumbHeight, dark ? QJsonArray{1, 1, 1, 0.45} : QJsonArray{0, 0, 0, 0.4});
    button("↑", railX - 10, originalY + SettingsViewportTop, 23, 25, "settings:scroll-up", 14);
    button("↓", railX - 10, originalY + SettingsViewportBottom - 25, 23, 25, "settings:scroll-down", 14);
}
