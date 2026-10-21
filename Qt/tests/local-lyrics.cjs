const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const { spawn } = require('node:child_process');
const { createInterface } = require('node:readline');
const { bridgeRuntime } = require('./fixtures/bridge-runtime.cjs');

/** 检查本地歌词归一化 */
async function main() {
    const { bridge, runtime } = bridgeRuntime();
    const cases = [
        ['timed', '[00:01.00]第一句\n[00:03.50]第二句'],
        ['plain', '本地纯文本歌词'],
        ['invalid', 123],
    ];
    if (process.argv[3]) {
        const root = path.resolve(process.argv[3]);
        for (const file of fs.readdirSync(root, { recursive: true })) {
            if (path.extname(file).toLowerCase() === '.lrc') {
                cases.push([file, fs.readFileSync(path.join(root, file), 'utf8')]);
            }
        }
    }
    const child = spawn(runtime, [path.join(bridge, 'worker.cjs')], {
        cwd: bridge,
        stdio: ['pipe', 'pipe', 'pipe'],
    });
    let errors = '';
    child.stderr.on('data', (chunk) => {
        errors += chunk;
    });
    const timeout = setTimeout(() => child.kill(), 10000);
    const responses = new Map();
    const exited = new Promise((resolve, reject) => {
        child.on('error', reject);
        child.stdin.on('error', reject);
        // close 在 stderr 读完后触发，保留模块加载失败的原始原因。
        child.on('close', (code, signal) => {
            clearTimeout(timeout);
            if (code === 0) {
                resolve();
            } else {
                reject(new Error(`Local lyrics worker failed (${code ?? signal}): ${errors}`));
            }
        });
    });
    createInterface({ input: child.stdout }).on('line', (line) => {
        const response = JSON.parse(line);
        responses.set(response.id, response.body);
        if (responses.size === cases.length) {
            child.stdin.end();
        }
    });
    for (const [id, content] of cases) {
        child.stdin.write(
            JSON.stringify({ id, route: '/local/lyrics', params: { content }, cookies: {} }) + '\n',
        );
    }
    try {
        await exited;
    } finally {
        clearTimeout(timeout);
        if (child.exitCode === null) {
            child.kill();
        }
    }
    const timed = responses.get('timed');
    assert.equal(timed.code, 200);
    assert.equal(timed.document.source, 'local');
    assert.equal(timed.document.timing, 'line');
    assert.deepEqual(
        timed.document.lines.map((line) => line.startMs),
        [1000, 3500],
    );
    assert.equal(responses.get('plain').document.plainText, '本地纯文本歌词');
    assert.equal(responses.get('invalid').code, 400);
    for (const [id] of cases.slice(3)) {
        const result = responses.get(id);
        assert.equal(result.code, 200, id);
        assert.ok(result.document.lines.length || result.document.plainText, id);
    }
    console.log('Local lyrics private IPC and existing parser passed; real files:', cases.length - 3);
}
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
