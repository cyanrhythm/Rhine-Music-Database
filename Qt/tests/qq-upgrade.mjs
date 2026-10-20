import assert from 'node:assert/strict';
import { QQAPI, qqItem } from '../../MusicBridge/client/qq.mjs';
import { safeLoginDetails } from '../../MusicBridge/client/failure.mjs';

const song = { id: 42, mid: 'songMID', type: 0, name: '本地曲目', file: { media_mid: 'mediaMID' } };
let state = 'not-subscribed';
let writeCalls = 0;
let verification = 'present';
let incomplete = false;
let source = { url: 'https://sjy6.stream.qqmusic.qq.com/fixture.mp3', quality: 'flac' };
const calls = [];
const api = new QQAPI({
    host: { authenticated: () => true },
    async request(route, params) {
        calls.push({ route, params });
        if (route === '/qq/getSongInfo') {
            return { response: { code: 0, songinfo: { code: 0, data: { track_info: song } } } };
        }
        if (route === '/qq/user/subscription-status') {
            return { items: [{ id: params.ids, status: state }] };
        }
        if (route === '/qq/user/song/like') {
            ++writeCalls;
            assert.deepEqual(params.song, { id: 42, mid: 'songMID', type: 0 });
            return { code: 200, liked: params.liked };
        }
        if (route === '/qq/user/album/subscribe' || route === '/qq/user/playlist/subscribe') {
            ++writeCalls;
            return { code: 200, subscribed: params.subscribed };
        }
        if (route === '/qq/user/playlist/tracks') {
            ++writeCalls;
            assert.deepEqual(params.songs, [{ id: 42, type: 0 }]);
            return { changed: false, verification: { status: verification } };
        }
        if (route === '/qq/user/playlist/create') {
            ++writeCalls;
            return { code: 200, tid: '70001', dirId: 1, name: '服务器确认的名称', verified: true };
        }
        if (route === '/qq/user/playlist/rename') {
            ++writeCalls;
            assert.deepEqual(params, { tid: '70001', name: '新名称' });
            return { verified: true, name: '新名称' };
        }
        if (route === '/qq/user/playlist/delete') {
            ++writeCalls;
            assert.deepEqual(params, { tid: '70001' });
            return { verified: true, deleted: true };
        }
        if (route === '/qq/user/playlist') {
            assert.equal(params.uid, undefined);
            assert.equal(params.playCount, 1);
            return {
                more: params.favoriteOffset === undefined || incomplete,
                nextFavoriteOffset: params.favoriteOffset === undefined ? 1000 : undefined,
                playlist: [
                    {
                        tid: String(params.favoriteOffset || 1),
                        name: '收藏歌单',
                        play_cnt: params.favoriteOffset === undefined ? 0 : undefined,
                    },
                ],
                total: 1001,
            };
        }
        if (route === '/qq/getMusicPlay') {
            return { data: { playUrl: { songMID: source } } };
        }
        if (route === '/qq/recommend/categories') {
            return { groups: [{ categories: [{ id: 8, name: '国风' }] }] };
        }
        if (route === '/qq/recommend/playlists' || route === '/qq/recommend/category-playlists') {
            return { playlists: [{ tid: '70001', title: '推荐', creator: { nick: '作者' } }], more: true };
        }
        if (['/qq/user/daily-songs', '/qq/user/guess-you-like', '/qq/recommend/new-songs'].includes(route)) {
            return { songs: [song] };
        }
        throw new Error('未设置本地响应：' + route);
    },
});
assert.equal((await api.mutate('/like', { id: 'songMID' })).liked, true);
state = 'subscribed';
assert.equal((await api.mutate('/like', { id: 'songMID' })).liked, false);
assert.equal((await api.mutate('/album/sub', { id: 'albumMID' })).subscribed, false);
assert.equal((await api.mutate('/playlist/subscribe', { id: '70001' })).subscribed, false);
state = 'unknown';
const beforeUnknown = writeCalls;
await assert.rejects(api.mutate('/album/sub', { id: 'albumMID' }), /状态尚未确认/);
assert.equal(writeCalls, beforeUnknown);
const created = await api.mutate('/playlist/create', { name: '新歌单' });
assert.equal(created.playlist.title, '服务器确认的名称');
assert.equal(created.playlist.provider, 'qq');
await assert.rejects(api.mutate('/playlist/create', { name: '长'.repeat(21) }), /1–20/);
await api.mutate('/playlist/name/update', { id: '70001', name: '新名称' });
await api.mutate('/playlist/delete', { id: '70001' });
await api.mutate('/playlist/tracks', { pid: '70001', tracks: 'songMID', op: 'add' });
verification = 'unknown';
const beforeUnconfirmed = writeCalls;
await assert.rejects(api.mutate('/playlist/tracks', { pid: '70001', tracks: 'songMID', op: 'add' }), /核实/);
assert.equal(writeCalls, beforeUnconfirmed + 1);
song.type = 1;
const blocked = new QQAPI(api.base);
await assert.rejects(
    blocked.mutate('/playlist/tracks', { pid: '70001', tracks: 'songMID', op: 'add' }),
    /普通曲库/,
);
song.type = 0;
const lists = await api.userPlaylists('1152921504716954983');
assert.equal(lists.items.length, 2);
assert.equal(lists.items[0].playCount, 0);
assert.equal(lists.items[1].playCount, undefined);
assert.equal(lists.incomplete, false);
incomplete = true;
assert.equal((await api.userPlaylists('42')).incomplete, true);
assert.equal((await api.discover('playlists')).more, false);
assert.equal((await api.discover('categories', 30, '8')).more, true);
assert.deepEqual(calls.at(-1).params, { id: '8', page: 2, limit: 30 });
for (const tab of ['daily', 'guess', 'new-songs']) {
    assert.equal((await api.discover(tab)).items[0].id, 'songMID');
}
assert.equal((await api.categories())[0].id, '8');
assert.equal(qqItem({ singerMID: 'artistMID', singerName: '歌手' }, 'artist').id, 'artistMID');
assert.equal(qqItem({ albummid: 'albumMID', name: '专辑', singer: '歌手' }, 'album').artist, '歌手');
let beforePlay = calls.filter((call) => call.route === '/qq/getMusicPlay').length;
assert.equal((await api.playback(qqItem(song), 'best', false)).quality, 'flac');
assert.equal(calls.at(-1).params.quality, 'best');
assert.equal(calls.filter((call) => call.route === '/qq/getMusicPlay').length, beforePlay + 1);
source = { ...source, preview: true };
assert.equal((await api.preview(qqItem(song), 'best', false)).preview, true);
assert.equal(calls.at(-1).params.preview, 1);
source = { url: '', restriction: { hint: { key: 'playback.preview-missing' } } };
await assert.rejects(api.preview(qqItem(song), 'best', false), /没有试听文件/);
source = { url: '', restriction: { hint: { key: 'playback.maybe-region-restricted' } } };
await assert.rejects(api.playback(qqItem(song), 'best', false), /可能受地区限制/);
const failure = safeLoginDetails({
    errorKey: 'auth.login-rejected',
    primaryFailure: {
        upstreamCode: 20279,
        token: 'private',
    },
    writeAttempted: true,
    verification: 'unconfirmed',
});
assert.equal(failure.primaryFailure.upstreamCode, 20279);
assert.equal(JSON.stringify(failure).includes('private'), false);
assert.equal(failure.writeAttempted, true);
console.log('QQ 3.3.1 状态核对、一次写入、普通歌曲限制、收藏续读、推荐、音质与试听契约通过');
