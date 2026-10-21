// 从独立工作目录启动发布包，避免开发机 Qt 路径掩盖漏部署的 DLL、插件和资源。
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { resourceDirectory, executable } = require('./app-layout.cjs');
const mac = process.platform === 'darwin';
const win = process.platform === 'win32';
const linux = process.platform === 'linux';
if (!mac && !linux && !win) {
    throw new Error('Package smoke test requires macOS, Linux or Windows');
}
const platforms = { darwin: 'macOS', win32: 'Windows', linux: 'Linux' };
const platform = platforms[process.platform];
const app = path.resolve(process.argv[2]);
const resources = path.join(app, resourceDirectory);
assert(
    fs.existsSync(path.join(resources, 'scene-fixture.json')),
    'Package is missing the offline scene fixture',
);
if (mac) {
    assert(
        fs.existsSync(path.join(app, 'Contents/PlugIns/platforms/libqoffscreen.dylib')),
        'Package is missing the offscreen platform plugin',
    );
} else if (linux) {
    assert(
        fs.existsSync(path.join(app, 'usr/plugins/platforms/libqoffscreen.so')),
        'Package is missing the offscreen platform plugin',
    );
} else {
    assert(
        fs.existsSync(path.join(app, 'platforms/qoffscreen.dll')) ||
            fs.existsSync(path.join(app, 'platforms/qoffscreend.dll')),
        'Package is missing the offscreen platform plugin',
    );
}
const env = {
    ...process.env,
};
for (const name of Object.keys(env)) {
    if (
        // LD_LIBRARY_PATH 的优先级高于 RUNPATH，CI 的 install-qt-action 会设置它，
        // 不清理就会加载宿主 Qt，冒烟测试验证不到随包运行时。
        /^(?:PATH|LD_LIBRARY_PATH|QT_PLUGIN_PATH|QT_QPA_PLATFORM_PLUGIN_PATH|QML_IMPORT_PATH|QML2_IMPORT_PATH|RHINE_RESOURCES|QT_MEDIA_BACKEND)$/i.test(
            name,
        )
    ) {
        delete env[name];
    }
}
const systemRoot = process.env.SystemRoot || process.env.SYSTEMROOT;
if (!mac && !linux) {
    assert(systemRoot, 'Windows system directory is required');
}
env.PATH = win ? `${systemRoot}\\System32;${systemRoot}` : '/usr/bin:/bin';
env.QT_QPA_PLATFORM = 'offscreen';
env.QT_LOGGING_RULES = 'qt.multimedia.plugin=true';
// 无显示服务器的构建机没有 GL 设备，只有 Windows 默认强制 GPU 参考比较。
env.RHINE_GPU_TEST = process.env.RHINE_GPU_TEST || (win ? '1' : '0');
if (linux) {
    // 强制走软件光栅，避免无 DRI 设备时回落到驱动初始化失败。
    env.LIBGL_ALWAYS_SOFTWARE = '1';
}
env.RHINE_SOFTWARE_GPU = '1';
const cwd = fs.mkdtempSync(path.join(os.tmpdir(), 'rhine package smoke '));
async function main() {
    try {
        await new Promise((resolve, reject) => {
            const child = spawn(path.join(app, executable), ['--self-test'], {
                cwd,
                env,
                stdio: ['ignore', 'pipe', 'pipe'],
            });
            let failure;
            let stderr = '';
            const stop = (reason) => {
                failure = reason;
                child.kill();
            };
            const timer = setTimeout(
                () => stop(new Error('Package smoke test timed out after 120 seconds')),
                120000,
            );
            child.stdout.on('data', (data) => process.stdout.write(data));
            child.stderr.on('data', (data) => {
                process.stderr.write(data);
                stderr = (stderr + data.toString()).slice(-16384);
                // Qt 6.8 的 GUI 程序在插件失败时会先弹框，不能等它自己退出。
                if (!failure && /Could not (?:find|load) the Qt platform plugin/.test(stderr)) {
                    stop(new Error('Package could not initialize its bundled Qt platform plugin'));
                }
            });
            child.on('error', (error) => {
                clearTimeout(timer);
                reject(error);
            });
            child.on('close', (status, signal) => {
                clearTimeout(timer);
                if (failure) {
                    reject(failure);
                } else if (status !== 0) {
                    reject(new Error(`Package failed: exit=${status}, signal=${signal}`));
                } else if (!/Loading media backend "ffmpeg"/.test(stderr)) {
                    reject(new Error('Package did not initialize its default FFmpeg backend'));
                } else {
                    resolve();
                }
            });
        });
        console.log(
            `Deployed ${platform} application, isolated Qt paths and bundled resources passed offline (GPU=${env.RHINE_GPU_TEST})`,
        );
    } finally {
        fs.rmSync(cwd, {
            recursive: true,
            force: true,
        });
    }
}
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
