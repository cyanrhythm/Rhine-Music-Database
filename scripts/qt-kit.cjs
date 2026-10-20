// Qt kit 的版本与默认路径单一来源；构建、格式化与预览共用同一份定义。
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
// minimum 与根 CMakeLists 的 find_package(Qt6 <version>) 一致；release 是 CI 与发布产物的固定版本。
const versions = Object.freeze({
    minimum: '6.8.0',
    development: '6.12.0',
    release: '6.8.3',
});
/**
 * 返回 Qt kit 根目录。Windows 没有可移植的默认安装路径，必须显式设置 QT_ROOT。
 */
function defaultRoot() {
    if (process.env.QT_ROOT) {
        return process.env.QT_ROOT;
    }
    return process.platform === 'darwin' ? path.join(os.homedir(), 'Qt', versions.development, 'macos') : '';
}
/**
 * 按主次补丁比较版本；不足的位补 0，避免 "6.8" 与 "6.8.0" 被判为不同。
 */
function compare(first, second) {
    const left = String(first).split('.');
    const right = String(second).split('.');
    for (let index = 0; index < Math.max(left.length, right.length); index += 1) {
        const difference = (Number.parseInt(left[index], 10) || 0) - (Number.parseInt(right[index], 10) || 0);
        if (difference !== 0) {
            return difference;
        }
    }
    return 0;
}
/**
 * 读取 kit 的实际版本；构建期用它核对最低要求并提示私有头带来的版本绑定。
 */
function detect(root) {
    if (!root) {
        return '';
    }
    const config = path.join(root, 'lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake');
    if (fs.existsSync(config)) {
        const match = fs.readFileSync(config, 'utf8').match(/set\(PACKAGE_VERSION "([^"]+)"\)/);
        if (match) {
            return match[1];
        }
    }
    const qtpaths = path.join(root, 'bin', process.platform === 'win32' ? 'qtpaths.exe' : 'qtpaths');
    if (!fs.existsSync(qtpaths)) {
        return '';
    }
    const result = spawnSync(qtpaths, ['--qt-version'], { encoding: 'utf8' });
    return result.status === 0 ? result.stdout.trim() : '';
}
module.exports = { versions, defaultRoot, compare, detect };
