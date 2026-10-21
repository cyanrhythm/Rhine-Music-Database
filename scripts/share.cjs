const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { buildVersion } = require('./release-version.cjs');
const { app } = require('./app-layout.cjs');
const { spawnSync } = require('node:child_process');
process.chdir(path.resolve(__dirname, '..'));
const mac = process.platform === 'darwin';
const win = process.platform === 'win32';
const linux = process.platform === 'linux';
// 统一使用 python3：多数发行版不再提供 python 命令。
const python = process.env.PYTHON || (win ? 'python' : 'python3');
if (!fs.existsSync(app)) {
    throw Error('Run npm run build first');
}

/** 发布步骤失败时抛出错误，使临时目录始终能够清理。 */
function run(cmd, args) {
    const result = spawnSync(cmd, args, {
        stdio: 'inherit',
    });
    if (result.error) {
        throw result.error;
    }
    if (result.status !== 0) {
        throw new Error(`${cmd} exited with ${result.status}`);
    }
}
run(python, ['scripts/audit-qt.py', app]);
if (linux) {
    // Linux 直接交付已构建的 AppImage，不再额外压缩，避免二次解包目录。
    const image = path.resolve('output/RhineMusicDatabase-Linux-x86_64.AppImage');
    if (!fs.existsSync(image)) {
        throw Error('AppImage was not built; run npm run build first');
    }
    console.log(image);
    process.exit(0);
}
const version = buildVersion();
const platform = mac ? 'macOS-arm64' : 'Windows-x64';
const name = version.release
    ? `RhineMusicDatabase-v${version.version}-${platform}`
    : `RhineMusicDatabase-${mac ? 'macOS' : 'Windows'}-${Date.now()}`;
const archive = path.resolve(`output/${name}.zip`);
if (mac) {
    run('codesign', ['--verify', '--deep', '--strict', app]);
    run('ditto', ['-c', '-k', '--sequesterRsrc', '--keepParent', app, archive]);
} else {
    // 只排除应用根目录的个人数据；SDK 内同名的数据资源仍需原样保留。
    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'rhine-release-'));
    const cleanApp = path.join(temporary, path.basename(app));
    try {
        fs.cpSync(app, cleanApp, {
            recursive: true,
            filter: (source) => path.relative(app, source).split(path.sep)[0] !== 'data',
        });
        run(python, ['-m', 'zipfile', '-c', archive, cleanApp]);
    } finally {
        fs.rmSync(temporary, { recursive: true, force: true });
    }
}
console.log(archive);
