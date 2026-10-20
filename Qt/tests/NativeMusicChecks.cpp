#include "MusicLibrary.h"
#include "MusicData.h"
#include "MusicDiscover.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QMediaDevices>
#include <QThread>
#include <QTemporaryDir>
#include <QUrl>

namespace {
bool waitFor(const std::function<bool()> &ready, int timeout = 3000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return ready();
}

QJsonObject song(QString id, QString title = "本地歌曲") {
    auto value = MusicData::item("song", id, title, "测试歌手");
    value["albumID"] = "7";
    value["album"] = "测试专辑";
    value["duration"] = 180000;
    value["cover"] = "https://music.126.net/native-fixture.jpg";
    return value;
}

QJsonObject lyrics() {
    return {{"version", 1},
            {"timing", "line"},
            {"offsetMs", 0},
            {"plainText", "第一句\n第二句"},
            {"lines", QJsonArray{QJsonObject{{"startMs", 1000}, {"text", "第一句"}},
                                 QJsonObject{{"startMs", 3000}, {"text", "第二句"}}}}};
}
}

/** 原生业务回归只调用 Qt 类型和本地响应，不用脚本对象充当资料库、播放器或歌词模型。 */
bool runNativeMusicChecks(const QString &root) {
    QFile file(root + "/slots.json");
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    Host host(root, true);
    MusicLibrary library(&host, QJsonDocument::fromJson(file.readAll()).object());
    QTemporaryDir audioFixture;
    QFile audio(audioFixture.filePath("native-silent.wav"));
    if (!audio.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream wav(&audio);
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
    audio.close();
    const auto audioURL = QUrl::fromLocalFile(audio.fileName()).toString();
    host.mediaControl("volume", 0);
    bool ok = true;
    const auto check = [&](bool value, const char *name) {
        if (!value) {
            qCritical() << "Native music regression:" << name;
        }
        ok &= value;
    };
    const auto sourceQuality = [](QString provider, QJsonObject audio, bool trial = false) {
        return MusicPlayer::sourceQuality(provider, audio, trial);
    };
    const auto source = [](QString path, QJsonObject fields = {}) {
        fields["url"] = "https://audio.example/" + path;
        return fields;
    };
    check(sourceQuality("netease", source("song.mp3", {{"level", "standard"}})) == "standard",
          "actual NetEase downgrade remains standard");
    check(sourceQuality("netease", source("song.flac", {{"level", "hires"}})) == "hires",
          "actual NetEase Hi-Res level is retained");
    check(sourceQuality("netease", source("song.flac", {{"br", 1800000}})) == "sq",
          "lossless bitrate alone does not imply Hi-Res");
    check(sourceQuality("netease", source("alternate.mp3", {{"level", "hires"}, {"br", 128000}})) ==
              "standard",
          "actual lossy alternate source cannot retain a lossless label");
    check(sourceQuality("qq", source("M500fixture.mp3", {{"quality", "flac"}})) == "standard",
          "QQ actual file overrides a misleading requested quality");
    check(sourceQuality("qq", source("M800fixture.mp3")) == "hq" &&
              sourceQuality("qq", source("F000fixture.flac")) == "sq" &&
              sourceQuality("qq", source("A000fixture.ape")) == "sq",
          "QQ logged-in media prefixes identify HQ and lossless");
    check(sourceQuality("qq", source("unknown.mp3", {{"quality", "flac"}})).isEmpty(),
          "QQ unknown file does not inherit requested quality");
    check(sourceQuality("kugou", source("audio", {{"extName", "flac"}, {"br", 2000000}})) == "sq",
          "Kugou actual format survives an opaque URL");
    check(sourceQuality("kugou",
                        source("audio", {{"extName", "flac"}, {"sampleRate", 96000}, {"bitDepth", 24}})) ==
              "hires",
          "Kugou Hi-Res requires actual sample rate and bit depth");
    check(sourceQuality("netease", source("song.flac", {{"level", "hires"}}), true).isEmpty() &&
              sourceQuality("netease", {{"level", "hires"}}).isEmpty() &&
              sourceQuality("kugou", source("opaque", {{"br", 2000000}})).isEmpty(),
          "preview, absent source and unknown codec never claim quality");
    for (const auto &badge : {"netease-sq", "netease-hires", "netease-jymaster", "netease-jyeffect",
                              "netease-sky", "qq-hq", "qq-sq", "kugou-hq", "kugou-sq", "kugou-hires"}) {
        check(!host.image("quality:" + QString(badge)).isNull(), "official quality image is packaged");
    }
    QString returnedQuality = "lossless";
    int nativeSearches = 0;
    int details = 0;
    int mutations = 0;
    bool rejectQQWrite = false;
    bool rejectLibrary = false;
    host.fixture = [&](QString route, QJsonObject parameters) {
        const auto args = parameters["args"].toArray();
        QJsonValue value;
        if (route.endsWith("/search")) {
            ++nativeSearches;
            const auto query = args.at(0).toString();
            value = QJsonObject{
                {"items", QJsonArray{song("1", query)}}, {"total", 1}, {"rawCount", 1}, {"more", false}};
        } else if (route.endsWith("/lyrics")) {
            value = lyrics();
        } else if (route.endsWith("/songs")) {
            // 单个数组参数仍须保留嵌套层级，不能被 Qt 版本或编译器构造成参数列表本身。
            check(args.size() == 1 && args.first().isArray(), "song IDs remain a nested IPC argument");
            QJsonArray songs;
            for (const auto &id : args.at(0).toArray()) {
                if (id != "missing") {
                    songs.append(song(id.toString()));
                }
            }
            value = songs;
        } else if (route.endsWith("/detail")) {
            ++details;
            const auto entry = args.at(0).toObject();
            if (entry["kind"] == "song") {
                value = QJsonArray{song(entry["id"].toString()), QJsonArray{}, QJsonArray{}};
            } else {
                auto info = entry;
                info["trackCount"] = 3;
                value =
                    QJsonArray{info, QJsonArray{song("1"), song("2")}, QJsonArray{"1", "missing", "2", "3"}};
            }
        } else if (route.endsWith("/wiki")) {
            value = "本地百科";
        } else if (route.endsWith("/playback") || route.endsWith("/preview")) {
            value = QJsonObject{
                {"url", audioURL}, {"level", returnedQuality}, {"preview", route.endsWith("/preview")}};
        } else if (route.endsWith("/mutate")) {
            ++mutations;
            if (rejectQQWrite) {
                return QJsonObject{{"body", QJsonObject{{"code", 502},
                                                        {"message", "写入未确认"},
                                                        {"writeAttempted", true},
                                                        {"retryable", false}}}};
            }
            value = QJsonObject{{"liked", true}, {"subscribed", true}};
        } else if (route.endsWith("/account")) {
            value = QJsonArray{"42", "本地账号"};
        } else if (route.endsWith("/likedIDs")) {
            value = QJsonArray{"1", "missing", "2"};
        } else if (route.endsWith("/userPlaylists")) {
            if (rejectLibrary) {
                return QJsonObject{{"body", QJsonObject{{"code", 502}, {"message", "本地模拟读取失败"}}}};
            }
            auto owned = MusicData::item("playlist", "8", "本人歌单");
            owned["creatorID"] = "42";
            const auto collected = MusicData::item("playlist", "9", "收藏歌单");
            value = QJsonObject{{"items", QJsonArray{owned, collected}}, {"rawCount", 2}, {"more", false}};
        } else if (route == "/like" || route == "/playlist/name/update" || route == "/playlist/tracks") {
            ++mutations;
            return QJsonObject{{"body", QJsonObject{{"code", 200}}}};
        } else if (route == "/login/cellphone") {
            return QJsonObject{{"body", QJsonObject{{"code", 200}}},
                               {"cookies", QJsonArray{"MUSIC_U=fixture-native"}}};
        } else if (route == "/personalized") {
            return QJsonObject{
                {"body", QJsonObject{{"code", 200},
                                     {"result", QJsonArray{QJsonObject{{"id", 8}, {"name", "原生推荐"}}}}}}};
        } else {
            return QJsonObject{{"body", QJsonObject{{"code", 200}}}};
        }
        return QJsonObject{{"body", QJsonObject{{"code", 200}, {"value", value}}}};
    };
    QObject::connect(&library, &MusicLibrary::modeRequested, &library,
                     [&](QString mode) { library.frame["mode"] = mode; });
    QObject::connect(&library, &MusicLibrary::selectionRequested, &library,
                     [&](int selected, QJsonObject) { library.frame["selected"] = selected; });

    library.search("旧查询");
    library.search("新查询");
    check(waitFor([&] { return !library.searchLoading; }), "search finishes");
    check(library.current["title"] == "新查询" && library.totals[0] == 1, "latest search owns selection");
    library.search("还未完成");
    library.search(" ");
    QCoreApplication::processEvents();
    check(!library.searchLoading && library.status == "请输入 1–100 字关键词",
          "invalid query cancels pending search");
    library.choose(MusicData::item("song", "10"));
    library.choose(song("11"));
    check(waitFor([&] { return !library.detailLoading; }), "metadata selection settles");
    check(library.current["id"] == "11", "old details cannot replace a new song");

    QJsonArray many;
    for (int i = 0; i < 2000; ++i) {
        many.append(song(QString::number(i + 1)));
    }
    library.lists[0] = many;
    library.selectedLane = 0;
    library.selectedPhysical = library.physicalSlots[0].value(1);
    library.offsets[0] = 0;
    library.hasMore[0] = false;
    library.next(-25);
    check(library.offsets[0] == 1976, "large reverse deltas preserve logical and physical rows");
    library.setGroupView("分组", {5, 7}, 0);
    check(library.libraryTotal() == 2 && library.sourceItems(0).at(1).toObject()["id"] == "8",
          "native grouped view uses source indices");
    library.setGroupView({}, {}, 0);

    library.enterDirectory(MusicData::item("playlist", "8", "本地目录"));
    check(waitFor([&] { return !library.directoryLoading; }), "directory detail finishes");
    check(library.directory && library.directory->ids.size() == 4 && library.directory->cursor == 3,
          "directory retains sparse complete IDs");
    const auto parent = library.directory;
    library.enterDirectory(MusicData::item("album", "7", "嵌套目录"));
    check(waitFor([&] { return !library.directoryLoading; }), "nested directory finishes");
    library.leaveDirectory();
    check(library.directory == parent, "return restores parent directory identity");
    library.loadDirectoryPage(2);
    check(waitFor([&] { return !library.directoryLoading; }), "directory page finishes");
    check(library.directory->cursor == 4 && library.directory->songs.size() == 3 &&
              library.current["id"] == "3",
          "sparse IDs consumed once and later selection restored");
    int returnSelections = 0;
    const auto returnConnection = QObject::connect(&library, &MusicLibrary::selectionRequested, &library,
                                                   [&](int, QJsonObject) { ++returnSelections; });
    const auto returningSong = library.current;
    const auto returningPhysical = library.selectedPhysical;
    library.returnToDirectory();
    check(returnSelections == 0 && library.current == returningSong &&
              library.selectedPhysical == returningPhysical && library.frame["mode"] == "archive",
          "song return does not reselect a canonical slot or reload identity");
    QObject::disconnect(returnConnection);
    const auto queue = library.directoryQueue();
    check(queue.size() == 4 && queue.at(1).toObject()["deferred"].toBool(), "queue preserves unloaded IDs");
    library.leaveDirectory();
    library.enterDirectory(MusicData::item("playlist", "9", "迟到目录"));
    library.leaveDirectory();
    QCoreApplication::processEvents();
    check(!library.directory, "closed directory cannot reopen from a delayed response");

    library.uid = "42";
    library.likedIDs.reset();
    library.hasMore[1] = true;
    library.loadMoreSource(1);
    check(waitFor([&] { return !library.personalLoading; }), "liked songs finish");
    check(library.totals[1] == 3 && library.lists[1].size() == 2 && !library.hasMore[1],
          "unavailable liked IDs do not cause retry loops");
    library.hasMore[2] = true;
    library.loadMoreSource(2);
    check(waitFor([&] { return !library.personalLoading; }), "playlists finish");
    check(library.lists[2].size() == 1 && library.lists[3].size() == 1,
          "owned playlists require creator identity");
    library.current = song("1");
    library.like();
    check(waitFor([&] { return !library.writeLoading; }), "like mutation finishes");
    check(MusicData::indexOf(*library.likedIDs, "1") < 0, "like mutation updates IDs");
    bool renamed = false;
    library.playlistForm("rename", "重命名", "8", [&](bool value) { renamed = value; });
    check(waitFor([&] { return renamed; }), "owned rename succeeds");
    check(library.lists[2].first().toObject()["title"] == "重命名", "renamed title retained");
    const auto before = mutations;
    library.playlistForm("rename", "非法", "other");
    check(mutations == before, "cannot edit another user's playlist");

    library.player.play(song("1"), QJsonArray{song("1"), song("2")});
    check(waitFor([&] { return !library.player.loading; }), "native playback source finishes");
    check(!library.player.failed && library.player.queue.size() == 2, "native playback prepares the queue");
    check(library.player.actualQuality == "sq", "player retains actual resolved quality");
    check(waitFor([&] { return library.player.lyrics.status == "ready"; }), "native lyrics finish");
    library.player.lyrics.sync(1500);
    check(library.player.lyrics.index == 0, "lyrics locate the first line");
    library.player.lyrics.adjustOffset(2000);
    check(library.player.lyrics.index == 1, "lyrics offset updates the shared index");
    library.player.lyrics.sync(0);
    check(library.player.lyrics.index == 0, "backward seeking updates lyrics");
    host.mediaControl("pause");
    library.player.tick(100, true);
    const auto queueBeforeQuality = library.player.queue;
    const auto lyricsBeforeQuality = library.player.lyrics.document();
    returnedQuality = "standard";
    library.player.setQuality("standard");
    check(library.player.loading && library.player.actualQuality.isEmpty(),
          "quality switch clears old badge");
    check(waitFor([&] { return !library.player.loading; }), "quality switch resolves its source");
    check(!host.mediaState().value("playing").toBool() && library.player.queue == queueBeforeQuality &&
              library.player.lyrics.document() == lyricsBeforeQuality,
          "quality switch preserves pause, queue and lyrics");
    library.player.setQuality("hires");
    library.player.setQuality("lossless");
    check(waitFor([&] {
              library.player.tick(102, true);
              return !library.player.loading && library.player.actualQuality == "standard";
          }) &&
              library.player.quality == "lossless",
          "rapid switches keep latest preference and actual downgrade");
    library.player.setQuality("invalid-quality");
    check(library.player.quality == "lossless", "unknown quality is rejected");
    library.player.next();
    check(library.player.actualQuality.isEmpty(), "new request clears the previous quality immediately");
    check(waitFor([&] { return !library.player.loading; }) && library.player.current["id"] == "2",
          "next song uses the native queue");
    // 无输出设备时播放器会延后打开音源；真实解码错误只在设备可用时验证。
    if (!QMediaDevices::defaultAudioOutput().isNull()) {
        QFile invalidAudio(audioFixture.filePath("invalid-audio.bin"));
        check(invalidAudio.open(QIODevice::WriteOnly), "invalid audio fixture opens");
        invalidAudio.write("invalid audio fixture");
        invalidAudio.close();
        check(host.mediaLoad(QUrl::fromLocalFile(invalidAudio.fileName()).toString()),
              "decoder accepts the local source before inspecting its content");
        check(waitFor([&] {
                  library.player.tick(103, true);
                  return library.player.failed;
              }),
              "actual decoder failure reaches the player");
        check(library.player.actualQuality.isEmpty(), "decoder failure clears the previous actual quality");
    } else {
        qInfo() << "无音频输出设备，跳过真实解码失败检查";
    }
    library.player.stop();
    check(library.player.current.isEmpty() && library.player.lyrics.document().isEmpty() &&
              library.player.actualQuality.isEmpty(),
          "stop clears player and lyrics");

    library.discover->load("playlists");
    check(waitFor([&] { return !library.discover->loading; }), "native discovery finishes");
    check(library.discover->items.size() == 1 &&
              library.discover->items.first().toObject()["title"] == "原生推荐",
          "discovery normalizes metadata");
    library.passwordLogin("phone", "13800000000", "fixture-password");
    check(waitFor([&] { return !library.accountLoading; }) && library.uid == "42",
          "native password login confirms account");
    check(library.identityName() == "本地账号", "native account identity retained");
    library.logout();
    check(library.uid.isEmpty() && library.lists[1].isEmpty() && library.lists[2].isEmpty() &&
              library.lists[3].isEmpty(),
          "logout clears private collections");
    library.switchProvider("qq");
    check(library.provider == "qq" && library.player.queue.isEmpty() && library.directory == nullptr,
          "provider switch clears old native state");
    library.uid = "42";
    auto qqSong = song("songMID");
    qqSong["provider"] = "qq";
    qqSong["key"] = "qq:song:songMID";
    const auto qqBefore = mutations;
    library.like(qqSong);
    check(waitFor([&] { return !library.writeLoading; }), "QQ verified like completes");
    check(MusicData::indexOfId(library.lists[1], "songMID") >= 0 && mutations == qqBefore + 1,
          "QQ like accepts server result without fetching unsupported likedIDs");
    rejectQQWrite = true;
    library.like(qqSong);
    check(waitFor([&] { return !library.writeLoading; }), "QQ uncertain write completes once");
    check(MusicData::indexOfId(library.lists[1], "songMID") >= 0 && mutations == qqBefore + 2,
          "QQ uncertain write does not retry or change local heart");
    library.capabilities = {{"mutations", QJsonObject{{"likeSong", false}}}};
    library.like(qqSong);
    check(mutations == qqBefore + 2 && !library.writeLoading, "QQ closed capability blocks mutation");
    library.capabilities = {};
    auto ordinary = MusicData::item("playlist", "70001", "QQ 自建", "", "qq");
    ordinary["creatorID"] = "42";
    ordinary["owned"] = true;
    ordinary["dirId"] = "1";
    check(library.canEditPlaylist(ordinary), "QQ ordinary owned playlist editable");
    ordinary["dirId"] = "201";
    check(!library.canEditPlaylist(ordinary), "QQ system liked directory is not ordinary playlist");
    library.player.play(qqSong, std::nullopt, std::nullopt, true);
    check(waitFor([&] { return !library.player.loading; }) && library.player.trial,
          "QQ explicit preview is marked as trial");
    library.player.stop();
    library.switchProvider("kugou", "lite");
    check(library.kugouEdition == "lite" && library.accountKey() == "kugou-lite",
          "Kugou editions retain separate account keys");
    // 恢复与串行加载流程
    for (const auto &provider : {QString("netease"), QString("qq"), QString("kugou")}) {
        library.switchProvider(provider);
        host.cookies["MUSIC_U"] = "fixture-loading";
        host.cookies["qqmusic_session"] = "fixture-loading";
        host.cookies["__kugou"] =
            QJsonObject{{library.kugouEdition, QJsonObject{{"token", "fixture-loading"}, {"userid", "42"}}}};
        library.restoreAccount();
        library.showSource(2);
        library.showSource(3);
        check(library.restoringAccount && library.sourceMessage(3).contains("正在恢复"),
              "shared restore hint appears before account response");
        check(waitFor([&] { return !library.sourceLoading(); }) && !library.lists[3].isEmpty(),
              "last requested collection loads after account restoration");
        library.lists[1] = {};
        library.hasMore[1] = true;
        library.loadMoreSource(1);
        library.lists[2] = {};
        library.lists[3] = {};
        library.hasMore[2] = true;
        library.hasMore[3] = true;
        library.loadMoreSource(3);
        check(library.sourceMessage(3).contains("正在读取"), "shared collection waiting hint");
        check(waitFor([&] { return !library.sourceLoading(); }) && !library.lists[3].isEmpty(),
              "collection switch during a serial request is not dropped");
        library.lists[3] = {};
        library.hasMore[3] = true;
        host.invalidateMetadata();
        rejectLibrary = true;
        library.loadMoreSource(3);
        check(waitFor([&] { return !library.sourceLoading(); }) && library.sourceMessage(3).contains("重试"),
              "failure replaces loading with a retry hint");
        rejectLibrary = false;
        host.invalidateMetadata();
        library.loadMoreSource(3);
        check(waitFor([&] { return !library.sourceLoading(); }) && !library.lists[3].isEmpty(),
              "explicit retry recovers the collection");
        library.restoreAccount();
        library.logout();
        check(waitFor([&] { return !library.sourceLoading(); }) && library.uid.isEmpty() &&
                  library.sourceMessage(3).contains("登录后"),
              "logout clears restore state and isolates its old callback");
    }
    check(nativeSearches > 0 && details > 0 && mutations >= 2,
          "native business exercises the real callback boundary");
    host.shutdown();
    qInfo() << "Native library, directories, account, playback, lyrics, mutations and discovery:"
            << (ok ? "passed" : "FAILED");
    return ok;
}
