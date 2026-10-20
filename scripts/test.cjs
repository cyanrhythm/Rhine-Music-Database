const path = require('node:path');
const { spawnSync } = require('node:child_process');
process.chdir(path.resolve(__dirname, '..'));
const mac = process.platform === 'darwin';
const win = process.platform === 'win32';
const linux = process.platform === 'linux';
// 统一使用 python3：多数发行版不再提供 python 命令。
const python = process.env.PYTHON || (win ? 'python' : 'python3');
function run(command, args, env = {}) {
    const result = spawnSync(command, args, {
        stdio: 'inherit',
        env: {
            ...process.env,
            ...env,
        },
    });
    if (result.error) {
        throw result.error;
    }
    if (result.status !== 0) {
        process.exit(result.status || 1);
    }
}
run(process.execPath, ['scripts/check-repository.cjs']);
run(python, ['scripts/check-qt-project.py']);
for (const script of [
    'scripts/check-music-upstream.cjs',
    'Qt/tests/release-version.cjs',
    'Qt/tests/publish-release.cjs',
    'Qt/tests/api-contract.mjs',
    'Qt/tests/native-client.mjs',
    'Qt/tests/qq-state.mjs',
    'Qt/tests/qq-upgrade.mjs',
    'Qt/tests/kugou-state.mjs',
    'Qt/tests/lyrics-format.mjs',
    'Qt/tests/kugou-device.cjs',
    'Qt/tests/device-state.cjs',
    'Qt/tests/netease-xeapi.cjs',
]) {
    run(process.execPath, [script]);
}
run('ctest', ['--test-dir', 'output/qt-build', '--output-on-failure']);
// 与构建脚本保持同一份部署目录；资源位置随平台布局不同。
const deployments = {
    darwin: ['output/RHINE MUSIC DATABASE.app', 'Contents/Resources'],
    linux: ['output/RhineMusicDatabase.AppDir', 'usr/bin/resources'],
    win32: ['output/windows/RHINE MUSIC DATABASE', 'resources'],
};
const [app, resourceDirectory] = deployments[process.platform];
const resources = app + '/' + resourceDirectory;
run(process.execPath, ['scripts/check-packaged-bridge.cjs', resources + '/MusicBridge']);
run(process.execPath, ['Qt/tests/netease-xeapi.cjs', resources + '/MusicBridge']);
run(process.execPath, ['Qt/tests/kugou-device.cjs', resources + '/MusicBridge']);
if (mac) {
    run('codesign', ['--verify', '--deep', '--strict', app]);
}
run(python, ['scripts/audit-qt.py', app]);
run(process.execPath, ['scripts/check-packaged-app.cjs', app]);
console.log('Cross-platform offline regressions and application package audit passed');
