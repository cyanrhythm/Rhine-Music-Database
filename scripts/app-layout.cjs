// 构建、测试和分发共用的三端应用布局。
const layouts = {
    darwin: {
        app: 'output/RHINE MUSIC DATABASE.app',
        resourceDirectory: 'Contents/Resources',
        executable: 'Contents/MacOS/RhineMusicDatabase',
    },
    linux: {
        app: 'output/RhineMusicDatabase.AppDir',
        resourceDirectory: 'usr/bin/resources',
        executable: 'usr/bin/RhineMusicDatabase',
    },
    win32: {
        app: 'output/windows/RHINE MUSIC DATABASE',
        resourceDirectory: 'resources',
        executable: 'RHINE MUSIC DATABASE.exe',
    },
};
const layout = layouts[process.platform];
if (!layout) {
    throw new Error('Supported platforms: macOS, Linux and Windows');
}
module.exports = layout;
