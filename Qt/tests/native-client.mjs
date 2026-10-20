import assert from 'node:assert/strict';
import { dispatch } from '../../MusicBridge/client/dispatch.mjs';

const message = (provider, operation, args, cookies = {}) => ({
    route: '/native/' + provider + '/' + operation,
    params: { args },
    cookies,
});

// 上游在失败请求期间续期的会话仍须交还私有 Host，不能因界面报错丢失新凭据。
const renewed = [{ token: 'opaque-fixture', credential: { musickey: 'renewed-private' } }];
const rejectedSearch = await dispatch(message('qq', 'search', ['本地', 'song', 0]), async () => ({
    body: { code: 502, errorKey: 'search.rejected', message: '搜索被拒绝' },
    qqSessions: renewed,
}));
assert.deepEqual(rejectedSearch.qqSessions, renewed);
assert.equal(rejectedSearch.body.errorKey, 'search.rejected');
assert.equal(JSON.stringify(rejectedSearch.body).includes('renewed-private'), false);

const netease = await dispatch(message('netease', 'search', ['本地', 'song', 0]), async (request) => {
    assert.equal(request.route, '/search');
    assert.equal(request.params.type, 1);
    return {
        body: {
            code: 200,
            result: { songCount: 1, songs: [{ id: 42, name: '本地歌曲', ar: [{ name: '测试' }] }] },
        },
        cookies: ['MUSIC_U=fixture-native; Path=/'],
    };
});
assert.equal(netease.body.value.items[0].id, '42');
assert.equal(netease.body.value.items[0].artist, '测试');
assert.equal(netease.body.value.total, 1);
assert.deepEqual(netease.cookies, ['MUSIC_U=fixture-native; Path=/']);

// 官方音源保留实际返回等级；替代音源的请求等级回填不得冒充母带。
for (const alternative of [false, true]) {
    const playback = await dispatch(
        message('netease', 'playback', [{ id: '42' }, 'jymaster', alternative]),
        async (request) => {
            assert.equal(request.params.level, 'jymaster');
            assert.equal(request.params.unblock, alternative);
            return {
                body: {
                    code: 200,
                    msg: alternative
                        ? 'Warning: Customizing unblock sources is not supported on this endpoint.'
                        : '',
                    data: [{ id: 42, url: 'https://example.com/audio', level: 'jymaster', type: 'flac' }],
                },
            };
        },
    );
    assert.equal(playback.body.code, 200);
    assert.equal(playback.body.value.level, alternative ? undefined : 'jymaster');
    assert.equal(playback.body.value.type, alternative ? undefined : 'flac');
}

const sessions = [{ token: 'private-fixture', credential: { musickey: 'private-key' } }];
const qq = await dispatch(message('qq', 'request', ['/user/detail', {}]), async (request) => {
    assert.equal(request.route, '/qq/user/detail');
    return {
        body: { code: 200, profile: { musicid: '42', nick: '测试账号' } },
        qqSessions: sessions,
        cookies: [],
    };
});
assert.equal(qq.body.value.profile.musicid, '42');
assert.deepEqual(qq.qqSessions, sessions);
assert.equal(JSON.stringify(qq.body).includes('private-fixture'), false);

let kugouRequests = 0;
const kugou = await dispatch(message('kugou', 'search', ['本地', 'song', 0]), async () => {
    kugouRequests++;
    return {};
});
assert.equal(kugou.body.code, 503);
assert.match(kugou.body.message, /需要登录/);
assert.equal(kugouRequests, 0);

// 原生 Host 交付当前版本的扁平凭据；登录后搜索必须实际到达 SDK。
const authenticatedKugou = await dispatch(
    message('kugou', 'search', ['本地', 'song', 0], { __kugou: { token: 'fixture-token' } }),
    async (request) => {
        assert.equal(request.route, '/kugou/search');
        assert.equal(request.cookies.__kugou.token, 'fixture-token');
        return { body: { code: 200, data: { lists: [], total: 0 } } };
    },
);
assert.equal(authenticatedKugou.body.code, 200);
assert.equal(authenticatedKugou.body.value.total, 0);

const failure = await dispatch(message('qq', 'request', ['/login/qr/check', {}]), async () => {
    throw { body: { code: 429, failureReason: 'local-backoff', retryAfterMs: 120000, rawToken: 'secret' } };
});
assert.equal(failure.body.code, 429);
assert.equal(failure.body.retryAfterMs, 120000);
assert.equal(JSON.stringify(failure).includes('secret'), false);

const parallel = await Promise.all(
    ['A', 'B'].map((token) =>
        dispatch(message('netease', 'search', [token, 'song', 0], { MUSIC_U: token }), async (request) => {
            await new Promise((resolve) => setTimeout(resolve, token === 'A' ? 10 : 0));
            assert.equal(request.cookies.MUSIC_U, token);
            return {
                body: {
                    code: 200,
                    result: { songCount: 1, songs: [{ id: token === 'A' ? 1 : 2, name: token }] },
                },
            };
        }),
    ),
);
assert.deepEqual(
    parallel.map((result) => result.body.value.items[0].title),
    ['A', 'B'],
);
const unsupported = await dispatch(message('netease', 'constructor', []), async () => {
    throw new Error('Unsupported operations must not reach the SDK');
});
assert.equal(unsupported.body.code, 400);
console.log('Native SDK client normalization, private credentials, errors and concurrent contexts passed');
