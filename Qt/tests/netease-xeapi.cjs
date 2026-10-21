// 冷启动、并发、刷新与失败重试均走真实 worker 和锁定上游协议，仅 HTTP 使用本地响应。
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { bridgeRuntime } = require('./fixtures/bridge-runtime.cjs');
const { bridge, runtime } = bridgeRuntime();
const hook = path.resolve(__dirname, 'fixtures/bridge-http.cjs');

/** 在独立临时目录执行一组真实 IPC 请求，并始终清理本测试的桥接进程和文件。 */
async function scenario(environment, verify) {
    const temporaryDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'rhine-xeapi-check-'));
    const child = spawn(runtime, ['--require', hook, path.join(bridge, 'worker.cjs')], {
        cwd: bridge,
        env: {
            ...process.env,
            RHINE_TEST_XEAPI_EXPIRE_FIRST: '',
            RHINE_TEST_XEAPI_FAIL_FIRST: '',
            ...environment,
            TMPDIR: temporaryDirectory,
            TEMP: temporaryDirectory,
            TMP: temporaryDirectory,
            RHINE_TEST_BRIDGE: bridge,
        },
        stdio: ['pipe', 'pipe', 'pipe'],
    });
    let buffer = '';
    let errors = '';
    let sequence = 0;
    const pending = new Map();
    const rejectPending = (error) => {
        for (const request of pending.values()) {
            clearTimeout(request.timer);
            request.reject(error);
        }
        pending.clear();
    };
    // 启动失败直接报告原始原因
    child.on('error', (error) => rejectPending(error));
    child.stdin.on('error', (error) => rejectPending(error));
    const exit = new Promise((resolve) => {
        child.once('close', (code, signal) => {
            rejectPending(new Error(`XEAPI worker exited (${code ?? signal}): ${errors}`));
            resolve(code);
        });
    });
    child.stderr.on('data', (chunk) => {
        errors += chunk;
    });
    child.stdout.on('data', (chunk) => {
        buffer += chunk;
        let end;
        while ((end = buffer.indexOf('\n')) >= 0) {
            const response = JSON.parse(buffer.slice(0, end));
            buffer = buffer.slice(end + 1);
            const request = pending.get(response.id);
            if (request) {
                clearTimeout(request.timer);
                pending.delete(response.id);
                request.resolve(response.body);
            }
        }
    });
    function request(level, id = '1') {
        return new Promise((resolve, reject) => {
            const requestId = String(++sequence);
            const timer = setTimeout(() => reject(new Error('XEAPI IPC timeout: ' + errors)), 15000);
            pending.set(requestId, { resolve, reject, timer });
            child.stdin.write(
                JSON.stringify({
                    id: requestId,
                    route: '/song/url/v1',
                    params: { id, level, unblock: false },
                    cookies: { MUSIC_U: 'fixture-account' },
                }) + '\n',
            );
        });
    }
    try {
        await verify(request, temporaryDirectory);
        child.stdin.end();
        assert.equal(await exit, 0);
        assert.equal(errors, '');
    } finally {
        for (const request of pending.values()) {
            clearTimeout(request.timer);
        }
        if (child.exitCode === null) {
            child.kill();
            await exit;
        }
        fs.rmSync(temporaryDirectory, { recursive: true, force: true });
    }
}

async function main() {
    await scenario({}, async (request, directory) => {
        const levels = ['lossless', 'hires', 'jymaster'];
        const responses = await Promise.all(levels.map((level) => request(level)));
        for (const [index, response] of responses.entries()) {
            assert.equal(response.code, 200);
            assert.equal(response.data[0].level, levels[index]);
            assert.equal(response.data[0].authenticated, true);
            assert.equal(response.data[0].keyRegistrations, 1);
        }
        const keyPath = path.join(directory, 'xeapi_public_key');
        assert(fs.existsSync(keyPath));
        if (process.platform !== 'win32') {
            assert.equal(fs.statSync(keyPath).mode & 0o777, 0o600);
        }
        assert.equal((await request('jymaster', '2')).code, 403);
        assert.equal((await request('jymaster')).data[0].keyRegistrations, 1);
    });
    await scenario({ RHINE_TEST_XEAPI_EXPIRE_FIRST: '1' }, async (request) => {
        assert.equal((await request('jymaster')).data[0].keyVersion, '1');
        const responses = await Promise.all([request('hires'), request('jymaster')]);
        for (const response of responses) {
            assert.equal(response.code, 200);
            assert.equal(response.data[0].keyRegistrations, 2);
            assert.equal(response.data[0].keyVersion, '2');
        }
    });
    await scenario({ RHINE_TEST_XEAPI_FAIL_FIRST: '1' }, async (request, directory) => {
        const failed = await Promise.all([request('hires'), request('jymaster')]);
        for (const response of failed) {
            assert.equal(response.code, 503);
            assert.match(response.message, /音源初始化失败/);
            assert.equal(response.data, undefined);
        }
        assert.equal(fs.existsSync(path.join(directory, 'xeapi_public_key')), false);
        const retried = await request('jymaster');
        assert.equal(retried.code, 200);
        assert.equal(retried.data[0].level, 'jymaster');
        assert.equal(retried.data[0].keyRegistrations, 2);
    });
    console.log(
        'NetEase XEAPI cold start, concurrent initialization, refresh, quality and explicit failure checks passed',
    );
}
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
