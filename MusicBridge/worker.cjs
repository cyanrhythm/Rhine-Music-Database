// 网易云协议使用锁定的 Enhanced 原包；QQ 协议使用锁定的 QQ API 原包。
// 本文件仅通过父子进程的私有管道传递请求。
// 替代音源组件的日志也必须静默，避免污染私有 IPC。
process.env.LOG_LEVEL = 'silent';
process.env.JSON_LOG = 'true';
delete process.env.LOG_FILE;
const fs = require('node:fs');
const path = require('node:path');
const readline = require('node:readline');
const tokenPath = path.join(require('node:os').tmpdir(), 'anonymous_token');
if (!fs.existsSync(tokenPath)) {
    fs.writeFileSync(tokenPath, '', {
        mode: 0o600,
    });
}
// 上游诊断可能包含 URL 或 Cookie，禁止混入 IPC 消息和日志。
console.log = console.info = console.warn = console.error = console.debug = () => {};
const apiRoot = path.join(__dirname, 'node_modules', '@neteasecloudmusicapienhanced', 'api');
const routes = Object.freeze({
    '/login/qr/key': 'login_qr_key',
    '/login/qr/check': 'login_qr_check',
    '/user/account': 'user_account',
    '/search': 'search',
    '/search/suggest': 'search_suggest',
    '/song/detail': 'song_detail',
    '/album': 'album',
    '/playlist/detail': 'playlist_detail',
    '/user/playlist': 'user_playlist',
    '/likelist': 'likelist',
    '/album/sublist': 'album_sublist',
    '/artists': 'artists',
    '/artist/album': 'artist_album',
    '/playlist/delete': 'playlist_delete',
    '/like': 'like',
    '/album/sub': 'album_sub',
    '/playlist/subscribe': 'playlist_subscribe',
    '/artist/sub': 'artist_sub',
    '/artist/sublist': 'artist_sublist',
    '/playlist/create': 'playlist_create',
    '/playlist/name/update': 'playlist_name_update',
    '/playlist/tracks': 'playlist_tracks',
    '/login/cellphone': 'login_cellphone',
    '/login': 'login',
    '/song/url': 'song_url',
    '/song/url/v1': 'song_url_v1',
    '/personalized': 'personalized',
    '/recommend/songs': 'recommend_songs',
    '/album/newest': 'album_newest',
    '/top/playlist': 'top_playlist',
    '/playlist/catlist': 'playlist_catlist',
    '/toplist': 'toplist',
    '/song/wiki/summary': 'song_wiki_summary',
    '/lyric/new': 'lyric_new',
    '/lyric': 'lyric',
});
// 沿用上游 main.js 的函数适配方式，只加载允许的接口。
// 接口和加密文件保持上游原始字节内容。
const { cookieToJson, generateDeviceId } = require(path.join(apiRoot, 'util'));
const { getXeapiPublicKey } = require(path.join(apiRoot, 'util', 'xeapiKey'));
const xeapiKeyPath = path.join(require('node:os').tmpdir(), 'xeapi_public_key');
global.deviceId = generateDeviceId();
let xeapiPublicKey;
let xeapiInitializing;
let requestModule;
/**
 * 在真实 XEAPI 请求前准备上游密钥，并让并发请求共享一次初始化或刷新。
 * 密钥仅写入当前桥接的私有临时目录；不修改上游协议和加密实现。
 */
async function prepareXeapiKey() {
    if (xeapiInitializing) {
        return xeapiInitializing;
    }
    if (!xeapiPublicKey && fs.existsSync(xeapiKeyPath)) {
        try {
            const cachedKey = JSON.parse(fs.readFileSync(xeapiKeyPath, 'utf8'));
            if (cachedKey.publicKey && cachedKey.sk) {
                xeapiPublicKey = cachedKey;
                global.deviceId = cachedKey.deviceId || global.deviceId;
            }
        } catch {
            // 损坏的临时缓存不能作为可用密钥，接下来重新向上游注册。
        }
    }
    const nextUpdateTime = Number(xeapiPublicKey?.nextUpdateTime) || 0;
    const updateAtMs = nextUpdateTime < 1e12 ? nextUpdateTime * 1000 : nextUpdateTime;
    if (xeapiPublicKey && (!updateAtMs || Date.now() < updateAtMs)) {
        return;
    }
    xeapiInitializing = (async () => {
        try {
            const publicKey = await getXeapiPublicKey(xeapiPublicKey || {}, global.deviceId);
            if (!publicKey.publicKey || !publicKey.sk) {
                throw new Error('Incomplete XEAPI key');
            }
            fs.writeFileSync(xeapiKeyPath, JSON.stringify(publicKey), { mode: 0o600 });
            xeapiPublicKey = publicKey;
            // 上游 request 在模块内缓存密钥和会话，刷新后重新载入以使用新密钥。
            const requestPath = require.resolve(path.join(apiRoot, 'util', 'request'));
            delete require.cache[requestPath];
            requestModule = undefined;
        } catch {
            // 初始化错误不得悄悄改成旧版码率接口，否则母带等档位会丢失。
            const error = new Error('网易云音源初始化失败，请重试');
            error.body = { code: 503, message: error.message };
            throw error;
        } finally {
            xeapiInitializing = undefined;
        }
    })();
    return xeapiInitializing;
}
const api = Object.fromEntries(
    Object.values(routes).map((name) => {
        const endpoint = require(path.join(apiRoot, 'module', name + '.js'));
        return [
            name,
            (data = {}) =>
                endpoint(
                    {
                        ...data,
                        cookie:
                            typeof data.cookie === 'string' ? cookieToJson(data.cookie) : data.cookie || {},
                    },
                    async (...args) => {
                        if (args[2]?.crypto === 'xeapi') {
                            await prepareXeapiKey();
                        }
                        requestModule ||= require(path.join(apiRoot, 'util', 'request'));
                        return requestModule(...args);
                    },
                ),
        ];
    }),
);
// 原样执行上游前端的认证和用户函数；加载时仅适配 ES 模块的
// 导入导出语法，使其能使用私有传输层。
const vm = require('node:vm');
/**
 * 载入锁定的上游认证或用户模块。
 * 仅适配导入导出和请求入口，禁止改写协议实现。
 */
function loadClient(name, request) {
    const source = fs.readFileSync(path.join(__dirname, 'upstream', name + '.js'), 'utf8');
    const names = [...source.matchAll(/export function (\w+)/g)].map((m) => m[1]);
    const executable = source
        .replace(/^import request from '@\/utils\/request';\s*/m, '')
        .replace(/export function /g, 'function ');
    return vm.runInNewContext(executable + '\n({' + names.join(',') + '})', {
        request,
        Date,
    });
}
/**
 * 根据私有 IPC 消息分派允许的音乐请求。
 * 沿用锁定上游的协议与错误语义，响应 Cookie 交给父进程隔离处理。
 */
async function dispatch(message) {
    if (message.route === '/local/lyrics') {
        const content = message.params?.content;
        if (typeof content !== 'string' || content.length > 1000000) {
            return { body: { code: 400, message: '本地歌词内容无效' }, cookies: [] };
        }
        const result = require('./lyrics-format.cjs').normalizeLyrics('netease', { lrc: { lyric: content } });
        if (result.document) {
            result.document.source = 'local';
        }
        return { body: result, cookies: [] };
    }
    if (message.route?.startsWith('/native/')) {
        const client = await import('./client/dispatch.mjs');
        return client.dispatch(message, dispatch);
    }
    if (message.route?.startsWith('/qq/')) {
        return require('./qq.cjs').dispatch(message);
    }
    if (message.route?.startsWith('/kugou/')) {
        return require('./kugou.cjs').dispatch(message);
    }
    let responseCookies = [];
    const request = async (config) => {
        const moduleName = routes[config.url];
        if (!moduleName) {
            throw new Error('Unsupported metadata route');
        }
        const query = Object.fromEntries(Object.entries(config.params || {}).map(([k, v]) => [k, String(v)]));
        query.cookie = Object.fromEntries(
            Object.entries(message.cookies || {}).filter(
                ([key]) => !key.startsWith('__') && key !== 'qqmusic_session',
            ),
        );
        const result = await api[moduleName](query);
        responseCookies.push(...(result.cookie || []));
        return result.body;
    };
    const auth = loadClient('auth', request);
    const user = loadClient('user', request);
    const p = message.params || {};
    const calls = {
        '/login/qr/key': () => auth.loginQrCodeKey(),
        '/login/qr/check': () => auth.loginQrCodeCheck(p.key),
        '/user/account': () => user.userAccount(),
        '/user/playlist': () => user.userPlaylist(p),
        '/likelist': () => user.userLikedSongsIDs(p.uid),
        '/album/sublist': () => user.likedAlbums(p),
    };
    const body = await (calls[message.route]
        ? calls[message.route]()
        : request({
              url: message.route,
              params: p,
          }));
    return {
        body: ['/lyric', '/lyric/new'].includes(message.route)
            ? require('./lyrics-format.cjs').normalizeLyrics('netease', body, message.params)
            : body,
        cookies: responseCookies,
    };
}

function send(value) {
    process.stdout.write(JSON.stringify(value) + '\n');
}
readline
    .createInterface({
        input: process.stdin,
    })
    .on('line', async (line) => {
        let message;
        try {
            message = JSON.parse(line);
            const result = await dispatch(message);
            send({
                id: message.id,
                ...result,
            });
        } catch (error) {
            send({
                id: message?.id,
                body: error.body || {
                    code: 503,
                    message: '音乐请求失败，请重试',
                },
                cookies: error.cookie || [],
            });
        }
    })
    .on('close', () => process.exit(0));
