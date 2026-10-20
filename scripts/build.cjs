// 同一资源与构建流程；平台分支仅处理 Qt 部署和系统签名。
const fs = require('node:fs');
const path = require('node:path');
// 版本与路径统一由 qt-kit 解析；发布版本号由 release-version 读取。
const { buildVersion } = require('./release-version.cjs');
const { spawnSync } = require('node:child_process');
const qtKit = require('./qt-kit.cjs');
const root = path.resolve(__dirname, '..');
process.chdir(root);
const win = process.platform === 'win32';
const mac = process.platform === 'darwin';
const linux = process.platform === 'linux';
if (!win && !mac && !linux) {
    throw Error('Supported platforms: macOS, Linux and Windows');
}
const python = process.env.PYTHON || (win ? 'python' : 'python3');
const nodeName = win ? 'node.exe' : 'node';
// 部署目录按平台区分；Linux 使用 AppImage 标准前缀布局。
const appPaths = new Map([
    [true, 'output/RHINE MUSIC DATABASE.app'],
    [false, linux ? 'output/RhineMusicDatabase.AppDir' : 'output/windows/RHINE MUSIC DATABASE'],
]);
const defaultAppPath = appPaths.get(mac);
/**
 * 串行执行构建或部署命令，失败立即终止。
 * 命令和参数分开传递，不经过 Shell 解释。
 */
function run(command, args = [], env = {}) {
    const result = spawnSync(command, args, {
        stdio: 'inherit',
        env: {
            ...process.env,
            ...env,
        },
        shell: false,
    });
    if (result.error) {
        throw result.error;
    }
    if (result.status !== 0) {
        throw Error(`${command} exited with ${result.status}`);
    }
}

function npmCi(folder) {
    // 直接调用 node scripts/build.cjs 时没有 npm_execpath，退回 PATH 上的 npm。
    const cli = process.env.npm_execpath;
    const executable = win ? 'npm.cmd' : 'npm';
    const command = cli ? process.execPath : executable;
    const args = [
        ...(cli ? [cli] : []),
        'ci',
        '--prefix',
        folder,
        '--omit=dev',
        '--ignore-scripts',
        '--no-audit',
        '--no-fund',
    ];
    run(command, args);
}
/**
 * 准备字体、模型、锁定桥接依赖和运行时资源；保持原有导出与检查顺序。
 */
function prepareResources() {
    fs.mkdirSync('output', {
        recursive: true,
    });
    run(python, ['scripts/check-qt-project.py']);
    run(python, ['scripts/export-assets.py']);
    run(python, ['scripts/bake-archive-lighting.py']);
    for (const file of fs.readdirSync('Assets/fonts')) {
        fs.copyFileSync(path.join('Assets/fonts', file), path.join('output/resources', file));
    }

    fs.cpSync('Assets/quality', 'output/resources/quality', { recursive: true });

    run(process.execPath, ['scripts/check-music-upstream.cjs']);
    const bridge = 'output/resources/MusicBridge';
    fs.mkdirSync(bridge, {
        recursive: true,
    });
    for (const file of [
        'worker.cjs',
        'qq.cjs',
        'kugou.cjs',
        'lyrics-format.cjs',
        'lyrics-source.json',
        'LyricsKit-LICENSE',
        'package.json',
        'package-lock.json',
    ]) {
        fs.copyFileSync(path.join('MusicBridge', file), path.join(bridge, file));
    }
    fs.cpSync('MusicBridge/upstream', path.join(bridge, 'upstream'), {
        recursive: true,
    });
    fs.cpSync('MusicBridge/client', path.join(bridge, 'client'), {
        recursive: true,
    });
    const bridgePackages = {
        '@neteasecloudmusicapienhanced/api': '4.41.1',
        '@yakult-green-tea/qq-music-api': '3.3.1',
        kugoumusicapi: '1.6.2',
    };
    if (
        Object.entries(bridgePackages).some(([name, version]) => {
            try {
                return (
                    JSON.parse(fs.readFileSync(path.join(bridge, 'node_modules', name, 'package.json')))
                        .version !== version
                );
            } catch {
                return true;
            }
        })
    ) {
        npmCi(bridge);
    }
    const binary =
        process.env.RHINE_NODE_BINARY ||
        (mac && process.arch === 'arm64' ? path.resolve('vendor/node-arm64/node') : process.execPath);
    const license = process.env.RHINE_NODE_LICENSE || path.join(path.dirname(binary), 'LICENSE');
    if (!fs.existsSync(binary) || !fs.existsSync(license)) {
        throw Error('Provide RHINE_NODE_BINARY and its matching RHINE_NODE_LICENSE');
    }
    if (mac) {
        const deps = spawnSync('otool', ['-L', binary], {
            encoding: 'utf8',
        });
        if (
            deps.status !== 0 ||
            /\/opt\/|\/Users\/|@rpath/.test(deps.stdout.split('\n').slice(1).join('\n'))
        ) {
            throw Error('Node must be an official standalone distribution');
        }
    }
    if (linux) {
        // 发行版自带的 node 链接了系统 ICU 与 absl，只有官方静态包能随 AppImage 分发。
        const deps = spawnSync('ldd', [binary], {
            encoding: 'utf8',
        });
        if (deps.status !== 0) {
            throw Error('Node must be an official standalone distribution');
        }
        // 官方静态包只依赖 glibc 运行库；出现其他动态依赖说明用的是发行版构建。
        const exposed = deps.stdout
            .split('\n')
            .filter((line) => line.includes('=>'))
            .map((line) => path.basename(line.split('=>')[0].trim()))
            .filter(
                (name) =>
                    !/^(?:ld-linux[\w.-]*|libc|libm|libmvec|libdl|libpthread|librt|libgcc_s|libstdc\+\+)\.so/.test(
                        name,
                    ),
            );
        if (exposed.length) {
            throw Error(`Node must be an official standalone distribution: ${exposed.join(', ')}`);
        }
    }
    fs.copyFileSync(binary, path.join(bridge, nodeName));
    fs.copyFileSync(license, path.join(bridge, 'NODE-LICENSE'));
    fs.chmodSync(path.join(bridge, nodeName), 0o755);
    for (const name of ['dfg-lut.bin', 'ao-kernel.bin', 'ao-noise.bin']) {
        fs.copyFileSync(path.join('Assets/lighting', name), path.join('output/resources', name));
    }
    fs.copyFileSync('Assets/room-pmrem.bin', 'output/resources/room-pmrem.bin');
    // 同步清理旧暂存文件，开发运行与发布包使用相同的纯原生资源。
    for (const name of [
        'runtime.js',
        'timeline.js',
        'qt-logic.js',
        'archive-cassette-geometry.json',
        'archive-assembly-geometry.json',
    ]) {
        fs.rmSync(path.join('output/resources', name), { force: true });
    }
}
/**
 * 验证 Qt kit 后串行配置和编译，返回部署所需的路径与构建类型。
 */
function buildQt() {
    // Linux 的 Qt 通常由发行版装在 /usr，其余平台从 kit 目录读取。
    const qt = qtKit.defaultRoot();
    if (!qt || !fs.existsSync(path.join(qt, 'lib/cmake/Qt6/Qt6Config.cmake'))) {
        throw Error('Set QT_ROOT to a Qt desktop kit with Shader Tools and private headers');
    }
    // 私有头把二进制绑定到具体 kit：低于最低要求直接失败，与发布版本不同只提示。
    const qtVersion = qtKit.detect(qt);
    if (qtVersion && qtKit.compare(qtVersion, qtKit.versions.minimum) < 0) {
        throw Error(`Qt ${qtVersion} is older than the required ${qtKit.versions.minimum}`);
    }
    // 最低版本由 CMakeLists 与 qt-kit.cjs 共同定义，两处漂移时立即失败。
    const cmake = fs.readFileSync('CMakeLists.txt', 'utf8');
    const required = cmake.match(/find_package\(\s*Qt6\s+([\d.]+)\s+REQUIRED/);
    if (required && !qtKit.versions.minimum.startsWith(required[1])) {
        throw Error(
            `CMakeLists requires Qt ${required[1]} but scripts/qt-kit.cjs pins ${qtKit.versions.minimum}`,
        );
    }
    if (qtVersion && qtVersion !== qtKit.versions.release) {
        const message =
            `Qt ${qtVersion} differs from the pinned release kit ${qtKit.versions.release}; ` +
            'private RHI headers tie the binary to this exact version';
        // CI 与发布构建必须使用固定版本；本地开发允许用其他补丁版本并只提示。
        if (process.env.GITHUB_ACTIONS || process.env.RHINE_STRICT_QT) {
            throw Error(message);
        }
        console.warn(message);
    }
    const config = process.env.RHINE_BUILD_TYPE || 'Release';
    const version = buildVersion();
    run('cmake', [
        '-S',
        '.',
        '-B',
        'output/qt-build',
        '-G',
        'Ninja',
        `-DCMAKE_PREFIX_PATH=${qt}`,
        `-DCMAKE_BUILD_TYPE=${config}`,
        '-DRHINE_PACKAGE_APP=OFF',
        `-DRHINE_VERSION=${version.version}`,
        `-DRHINE_RELEASE_BUILD=${version.release ? 'ON' : 'OFF'}`,
    ]);
    run('cmake', ['--build', 'output/qt-build']);
    return {
        qt,
        config,
    };
}
/**
 * 判断 Windows 上实际使用的编译器家族。
 * MSVC 与 MinGW 的 C++ 运行库布局完全不同，部署不能共用同一段逻辑。
 */
function windowsToolchain(qt) {
    const requested = (process.env.RHINE_TOOLCHAIN || '').toLowerCase();
    if (requested === 'msvc' || requested === 'mingw') {
        return requested;
    }
    const cache = 'output/qt-build/CMakeCache.txt';
    if (fs.existsSync(cache)) {
        const entry = fs
            .readFileSync(cache, 'utf8')
            .split(/\r?\n/)
            .find((line) => line.startsWith('CMAKE_CXX_COMPILER:'));
        if (entry) {
            const compiler = path.basename(entry.slice(entry.indexOf('=') + 1)).toLowerCase();
            if (compiler === 'cl.exe') {
                return 'msvc';
            }
            if (compiler.endsWith('.exe')) {
                return 'mingw';
            }
        }
    }
    return fs.existsSync(path.join(qt, 'bin', 'libstdc++-6.dll')) ? 'mingw' : 'msvc';
}
/**
 * Qt 插件的相对根目录：官方 kit 是 <kit>/plugins，系统包是 <prefix>/share/qt6/plugins。
 */
function qtPlugins(qt) {
    for (const relative of ['plugins', path.join('share', 'qt6', 'plugins')]) {
        const directory = path.join(qt, relative);
        if (fs.existsSync(directory)) {
            return directory;
        }
    }
    return path.join(qt, 'plugins');
}
/**
 * 定位 MinGW 运行库目录。
 * GCC 的 -print-file-name 只查自身库目录，MSYS2 与 Qt 官方 kit 把 DLL 放在编译器同级 bin。
 */
function mingwRuntimeDirectory() {
    for (const command of ['g++', 'c++', 'x86_64-w64-mingw32-g++', 'clang++']) {
        const result = spawnSync(command, ['-print-file-name=libstdc++-6.dll'], { encoding: 'utf8' });
        const file = result.status === 0 ? result.stdout.trim() : '';
        if (file && fs.existsSync(file)) {
            return path.dirname(file);
        }
    }
    const compilers = ['g++.exe', 'c++.exe', 'x86_64-w64-mingw32-g++.exe', 'clang++.exe'];
    for (const directory of (process.env.PATH || '').split(path.delimiter)) {
        if (!directory || !compilers.some((name) => fs.existsSync(path.join(directory, name)))) {
            continue;
        }
        if (fs.existsSync(path.join(directory, 'libstdc++-6.dll'))) {
            return directory;
        }
    }
    return '';
}
/**
 * 读取可执行文件的导入表。
 * 大体积 FFmpeg 库的导入表会超过 spawnSync 默认的 1 MB 缓冲，必须显式放宽，
 * 否则调用方会拿到空列表并静默漏掉整棵依赖树。
 */
function importedLibraries(executable) {
    const result = spawnSync('objdump', ['-p', executable], {
        encoding: 'utf8',
        maxBuffer: 64 * 1024 * 1024,
    });
    if (result.error || result.status !== 0) {
        throw new Error(
            `Cannot inspect imports of ${path.basename(executable)}: ${result.error || result.status}`,
        );
    }
    return [...result.stdout.matchAll(/DLL Name:\s*(\S+)/g)].map((match) => match[1]);
}
/**
 * 列出部署目录内所有可执行文件与动态库，作为依赖闭包的起点。
 */
function binariesIn(app) {
    return fs
        .readdirSync(app, { recursive: true })
        .map((name) => path.join(app, name))
        .filter((file) => /\.(?:exe|dll)$/i.test(file));
}
/**
 * MinGW 运行库部署
 * GCC 的包把 C++ 运行库与 Qt 的第三方依赖都放在工具链 bin 里；
 * 这里按导入表求传递闭包，缺 C++ 运行库时明确失败而不是静默出包。
 */
function deployMinGWishRuntime(app, qt) {
    const runtime = mingwRuntimeDirectory();
    if (!runtime) {
        throw new Error('MinGW runtime not found; run the build from a shell with the compiler on PATH');
    }
    const sources = [...new Set([path.join(qt, 'bin'), runtime])];
    const pending = binariesIn(app);
    const deployed = [];
    while (pending.length) {
        for (const name of importedLibraries(pending.shift())) {
            const target = path.join(app, name);
            if (fs.existsSync(target)) {
                continue;
            }
            const source = sources.map((dir) => path.join(dir, name)).find((file) => fs.existsSync(file));
            if (!source) {
                continue;
            }
            fs.copyFileSync(source, target);
            deployed.push(name);
            pending.push(target);
        }
    }
    if (!fs.existsSync(path.join(app, 'libstdc++-6.dll'))) {
        throw new Error('MinGW C++ runtime missing; refusing to write an incomplete package');
    }
    console.log(`MinGW runtime: ${deployed.length} dependencies deployed`);
}

/**
 * MSVC 运行库部署
 * 仅从当前开发环境的可分发目录取 DLL；缺少运行库时禁止生成不完整的发布包。
 */
function deployWindowsRuntime(app, config) {
    let redist = process.env.VCToolsRedistDir;
    if (!redist) {
        const base = path.join(process.env.VCINSTALLDIR || '', 'Redist/MSVC');
        if (!fs.existsSync(base)) {
            throw new Error('MSVC redist directory missing; build in a Visual Studio developer shell');
        }
        const versions = fs.readdirSync(base).filter((name) => /^\d+\.\d+\.\d+$/.test(name));
        versions.sort((first, second) => second.localeCompare(first, undefined, { numeric: true }));
        if (!versions.length) {
            throw new Error('MSVC redistributable runtime missing');
        }
        redist = path.join(base, versions[0]);
    }
    const debug = config === 'Debug';
    const architecture = path.join(redist, ...(debug ? ['debug_nonredist', 'x64'] : ['x64']));
    const directory = fs
        .readdirSync(architecture)
        .find((name) => (debug ? /^Microsoft\.VC\d+\.DebugCRT$/ : /^Microsoft\.VC\d+\.CRT$/).test(name));
    if (!directory) {
        throw new Error('MSVC x64 runtime DLL directory missing');
    }
    const runtime = path.join(architecture, directory);
    const libraries = fs.readdirSync(runtime).filter((name) => name.toLowerCase().endsWith('.dll'));
    const suffix = debug ? 'd.dll' : '.dll';
    for (const name of ['msvcp140' + suffix, 'vcruntime140' + suffix, 'vcruntime140_1' + suffix]) {
        if (!libraries.includes(name)) {
            throw new Error(`MSVC runtime library missing: ${name}`);
        }
    }
    for (const name of libraries) {
        fs.copyFileSync(path.join(runtime, name), path.join(app, name));
    }
}

/**
 * 根据 Qt 插件实际依赖部署 FFmpeg 动态库，避免 Mac 发布包静默回落到原生后端。
 * 只复制当前 kit 中被引用的库，兼容不同 Qt 补丁版本使用的 FFmpeg 主版本。
 */
function deployMacAudioLibraries(qt, app) {
    const frameworks = path.join(app, 'Contents/Frameworks');
    fs.mkdirSync(frameworks, { recursive: true });
    const plugin = path.join(app, 'Contents/PlugIns/multimedia/libffmpegmediaplugin.dylib');
    const pending = [plugin];
    const deployed = new Set();
    while (pending.length) {
        const binary = pending.pop();
        const result = spawnSync('otool', ['-L', binary], { encoding: 'utf8' });
        if (result.status !== 0) {
            throw new Error(`Cannot inspect multimedia dependency: ${binary}`);
        }
        const pattern =
            /@(?:rpath|loader_path)\/(lib(?:avcodec|avformat|avutil|avfilter|avdevice|swresample|swscale)\.\d+(?:\.\d+)*\.dylib)/g;
        for (const match of result.stdout.matchAll(pattern)) {
            const name = match[1];
            if (deployed.has(name)) {
                continue;
            }
            const source = path.join(qt, 'lib', name);
            fs.copyFileSync(source, path.join(frameworks, name));
            deployed.add(name);
            pending.push(source);
        }
    }
    // 插件位于 Contents/PlugIns/multimedia；音频库位于 Contents/Frameworks。
    run('install_name_tool', ['-add_rpath', '@loader_path/../../Frameworks', plugin]);
}

// AppImage 只随包分发 Qt 与音视频运行时；系统图形栈、glibc 与桌面服务留给发行版。
const linuxExcludedLibraries = new Set([
    'ld-linux-x86-64.so.2',
    'libc.so.6',
    'libm.so.6',
    'libmvec.so.1',
    'libdl.so.2',
    'libpthread.so.0',
    'librt.so.1',
    'libresolv.so.2',
    'libutil.so.1',
    'libnsl.so.1',
    'libgcc_s.so.1',
    'libstdc++.so.6',
    'libz.so.1',
    'libbz2.so.1.0',
    'liblzma.so.5',
    'libzstd.so.1',
    'libexpat.so.1',
    'libffi.so.8',
    'libpcre2-8.so.0',
    'libglib-2.0.so.0',
    'libgobject-2.0.so.0',
    'libgio-2.0.so.0',
    'libgmodule-2.0.so.0',
    'libmount.so.1',
    'libblkid.so.1',
    'libselinux.so.1',
    'libsystemd.so.0',
    'libudev.so.1',
    'libdbus-1.so.3',
    'libfontconfig.so.1',
    'libfreetype.so.6',
    'libharfbuzz.so.0',
    'libgraphite2.so.3',
    'libpng16.so.16',
    'libGL.so.1',
    'libGLX.so.0',
    'libGLdispatch.so.0',
    'libOpenGL.so.0',
    'libEGL.so.1',
    'libGLESv2.so.2',
    'libgbm.so.1',
    'libdrm.so.2',
    'libX11.so.6',
    'libX11-xcb.so.1',
    'libXau.so.6',
    'libXdmcp.so.6',
    'libXext.so.6',
    'libXrender.so.1',
    'libXi.so.6',
    'libXfixes.so.3',
    'libXcursor.so.1',
    'libXrandr.so.2',
    'libXinerama.so.1',
    'libXcomposite.so.1',
    'libXdamage.so.1',
    'libxcb.so.1',
    'libxkbcommon.so.0',
    'libxkbcommon-x11.so.0',
    'libwayland-client.so.0',
    'libwayland-cursor.so.0',
    'libwayland-egl.so.1',
    'libvulkan.so.1',
    'libpulse.so.0',
    'libasound.so.2',
]);
// xcb 扩展、Wayland 后端与音频服务跟随发行版，版本差异由用户系统统一升级。
const linuxExcludedPattern =
    /^lib(?:xcb-|wayland-|drm[_-]|vulkan|GLESv|GLX|X11|Xext|Xrender|Xi\.|Xfixes|Xcursor|Xrandr|Xinerama|Xcomposite|Xdamage|Xau|Xdmcp|pulse|asound|nss|nspr|smime|softokn|freebl|plc|plds)/;

function isExcludedLinuxLibrary(name) {
    return linuxExcludedLibraries.has(name) || linuxExcludedPattern.test(name);
}

/**
 * 解析 Qt 运行时的实际位置
 * 不假设 kit 目录结构，直接读取已构建程序解析到的 Qt 库目录。
 */
function linuxQtLibraryDirectory(binary) {
    const result = spawnSync('ldd', [binary], {
        encoding: 'utf8',
    });
    if (result.status !== 0) {
        throw new Error('Cannot inspect Qt runtime dependencies');
    }
    const match = /^\s*libQt6Core\.so\.6 => (\S+) \(0x/m.exec(result.stdout);
    if (!match) {
        throw new Error('Qt6Core is not linked into the executable');
    }
    return path.dirname(match[1]);
}

/**
 * 读取 Qt 安装布局
 * 系统 Qt 的前缀是 /usr，插件和 QML 模块在 /usr/lib/qt6 下，统一由 qmake 给出。
 */
function linuxQtQuery(qt, key) {
    // 官方 Qt 包通常只装 qmake 而没有 qmake6；只要 kit 自带 qmake 就只用它，
    // 否则会把系统 Qt 的路径混进来，导致打包版本与编译版本不一致。
    const kit = [path.join(qt, 'bin/qmake6'), path.join(qt, 'bin/qmake')].filter((file) =>
        fs.existsSync(file),
    );
    const commands = kit.length ? kit : ['qmake6', 'qmake'];
    for (const command of commands) {
        const result = spawnSync(command, ['-query', key], {
            encoding: 'utf8',
        });
        if (result.status === 0 && result.stdout.trim()) {
            return result.stdout.trim();
        }
    }
    throw new Error(`Cannot locate the Qt ${key} directory`);
}

function linuxQtQmlDirectory(qt) {
    return linuxQtQuery(qt, 'QT_INSTALL_QML');
}

/**
 * 定位 Qt 自带工具
 * 优先使用 kit 自己的 bin 目录，避免 PATH 中混入其他 Qt 版本或被裁剪掉的工具。
 */
function linuxQtTool(qt, name) {
    // Qt 6 的工具按发行版不同分散在 libexec 与 bin：官方二进制包放在 libexec，
    // 部分发行版重打包到 /usr/bin。逐个探活，任何可执行的候选都接受。
    const searched = [];
    for (const key of ['QT_INSTALL_LIBEXECS', 'QT_INSTALL_BINS']) {
        let directory;
        try {
            directory = linuxQtQuery(qt, key);
        } catch {
            continue;
        }
        searched.push(path.join(directory, name));
    }
    searched.push(name);
    for (const candidate of searched) {
        // 相对名交给 PATH 解析；绝对路径先确认存在，避免把 qmake 的错误当作成功。
        if (path.isAbsolute(candidate) && !fs.existsSync(candidate)) {
            continue;
        }
        const probe = spawnSync(candidate, ['-help'], {
            encoding: 'utf8',
        });
        // qmlimportscanner 的 -help 也返回非零，能启动即视为可用。
        if (!probe.error) {
            return candidate;
        }
    }
    throw new Error(`Cannot locate the Qt tool ${name}; searched ${searched.join(', ')}`);
}
/**
 * 复制插件、QML 模块与 ELF 依赖闭包
 * 依赖按 ldd 结果递归收集，只跳过系统图形栈与 glibc，避免与用户驱动冲突。
 */
function deployLinuxRuntime(qt, app, binary) {
    const libraries = path.join(app, 'usr/lib');
    const plugins = path.join(app, 'usr/plugins');
    fs.mkdirSync(libraries, {
        recursive: true,
    });
    const qtLibraries = linuxQtLibraryDirectory(binary);
    // 只按依赖闭包收集 Qt 库；系统 Qt 目录里的 WebEngine、Widgets 等与面板无关。
    const sources = [binary];
    const pluginSource = linuxQtQuery(qt, 'QT_INSTALL_PLUGINS');
    // Wayland 平台插件依赖 shell 集成与装饰插件，必须整组跟随部署。
    for (const folder of [
        'platforms',
        'imageformats',
        'tls',
        'multimedia',
        'platforminputcontexts',
        'wayland-shell-integration',
        'wayland-decoration-client',
        'wayland-graphics-integration-client',
    ]) {
        const directory = path.join(pluginSource, folder);
        if (!fs.existsSync(directory)) {
            continue;
        }
        for (const file of fs.readdirSync(directory)) {
            if (!file.endsWith('.so')) {
                continue;
            }
            // 平台插件名随打包方式变化：发行版是 libqwayland.so，
            // 官方 Qt 包拆成 libqwayland-egl.so 与 libqwayland-generic.so。
            if (
                folder === 'platforms' &&
                !/^libq(?:xcb|wayland(?:-\w+)?|offscreen|minimal)\.so$/.test(file)
            ) {
                continue;
            }
            const destination = path.join(plugins, folder, file);
            fs.mkdirSync(path.dirname(destination), {
                recursive: true,
            });
            fs.copyFileSync(path.join(directory, file), destination);
            sources.push(destination);
        }
    }
    // Wayland 是桌面 Linux 的主流会话；插件缺失时应用会退到 XWayland，
    // 仍能运行但行为不原生。官方 Qt 包把它放在 linux_gcc_64 的附属归档里，
    // 由 aqt 的 --autodesktop 安装，因此这里只提示，不阻断构建。
    const platforms = fs.readdirSync(path.join(plugins, 'platforms'));
    if (!platforms.some((name) => name.startsWith('libqwayland'))) {
        console.warn('Qt Wayland platform plugin missing; Wayland sessions will fall back to XWayland');
    }
    const qmlSource = linuxQtQmlDirectory(qt);
    const scanned = spawnSync(
        linuxQtTool(qt, 'qmlimportscanner'),
        [
            '-rootPath',
            path.join(root, 'Qt/qml'),
            '-importPath',
            qmlSource,
            '-importPath',
            path.join(root, 'Qt/qml'),
        ],
        {
            encoding: 'utf8',
            maxBuffer: 64 * 1024 * 1024,
        },
    );
    if (scanned.status !== 0) {
        // 保留工具自身输出，构建机缺模块时能直接看出缺哪一个。
        const detail = `${scanned.stderr || ''}${scanned.stdout || ''}`
            .trim()
            .split('\n')
            .slice(-5)
            .join('\n');
        throw new Error(`Cannot scan QML imports${detail ? `: ${detail}` : ''}`);
    }
    const modules = new Set();
    for (const entry of JSON.parse(scanned.stdout.replace(/^[^{[]*/, ''))) {
        if (entry.path && entry.path.startsWith(qmlSource + path.sep)) {
            modules.add(path.relative(qmlSource, entry.path));
        }
    }
    for (const module of modules) {
        const destination = path.join(app, 'usr/qml', module);
        fs.cpSync(path.join(qmlSource, module), destination, {
            recursive: true,
        });
        // QML 模块自带的插件同样引用 Qt 库（如 QuickControls2、QuickTemplates2），
        // 只复制文件而不纳入依赖闭包，会留下启动后才暴露的缺库。
        sources.push(...sharedObjects(destination));
    }
    // 插件自身也引用 Qt 与音视频库；与程序一起进入同一个依赖闭包。
    const deployed = new Set();
    while (sources.length) {
        const source = sources.pop();
        const result = spawnSync('ldd', [source], {
            encoding: 'utf8',
        });
        if (result.status !== 0) {
            continue;
        }
        for (const line of result.stdout.split('\n')) {
            const match = /^\s*(\S+) => (\S+) \(0x[0-9a-f]+\)$/.exec(line);
            if (!match) {
                continue;
            }
            let [, name, location] = match;
            // ldd 对解释器给出的是路径而不是 SONAME，这类条目始终由系统提供。
            if (name.includes('/')) {
                continue;
            }
            name = path.basename(name);
            if (deployed.has(name) || isExcludedLinuxLibrary(name) || !fs.existsSync(location)) {
                continue;
            }
            deployed.add(name);
            const destination = path.join(libraries, name);
            if (!fs.existsSync(destination)) {
                fs.copyFileSync(location, destination);
                fs.chmodSync(destination, 0o755);
            }
            sources.push(location);
        }
    }
    fs.writeFileSync(
        path.join(app, 'usr/bin/qt.conf'),
        '[Paths]\nPrefix=..\nPlugins=plugins\nQmlImports=qml\n',
    );
}

/**
 * 生成 AppImage
 * 桌面条目、图标与 AppRun 位于 AppDir 根目录，运行库按前缀目录布局。
 */
// 打包工具按官方发行版锁定，构建机不需要预装。
const appImageToolVersion = '1.9.1';
const appImageToolChecksums = {
    x86_64: 'ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0',
    aarch64: '',
};

/**
 * 取得可用的 appimagetool
 * 优先使用 APPIMAGE_TOOL 指定的可执行文件，其次系统 PATH，最后下载锁定版本。
 */
function resolveAppImageTool(architecture) {
    if (process.env.APPIMAGE_TOOL) {
        return process.env.APPIMAGE_TOOL;
    }
    const system = spawnSync('appimagetool', ['--version'], {
        encoding: 'utf8',
    });
    if (!system.error) {
        return 'appimagetool';
    }
    const expected = appImageToolChecksums[architecture];
    if (!expected) {
        throw new Error(
            `Install appimagetool or set APPIMAGE_TOOL for ${architecture}; no pinned checksum is recorded`,
        );
    }
    const directory = path.join('output', 'appimagetool');
    const tool = path.join(directory, `appimagetool-${architecture}.AppImage`);
    if (!fs.existsSync(tool)) {
        fs.mkdirSync(directory, {
            recursive: true,
        });
        const url =
            `https://github.com/AppImage/appimagetool/releases/download/${appImageToolVersion}` +
            `/appimagetool-${architecture}.AppImage`;
        run('curl', ['--fail', '--location', '--retry', '3', '--output', tool, url]);
        const digest = spawnSync('sha256sum', [tool], {
            encoding: 'utf8',
        }).stdout.split(' ')[0];
        if (digest !== expected) {
            fs.rmSync(tool, {
                force: true,
            });
            throw new Error(`appimagetool checksum mismatch: ${digest}`);
        }
        fs.chmodSync(tool, 0o755);
    }
    return tool;
}

function packageAppImage(app) {
    const architecture = process.arch === 'arm64' ? 'aarch64' : 'x86_64';
    const image = path.resolve(`output/RhineMusicDatabase-Linux-${architecture}.AppImage`);
    fs.rmSync(image, {
        force: true,
    });
    // 无 FUSE 的机器（容器、CI）也能打包；有 FUSE 时行为一致。
    run(resolveAppImageTool(architecture), ['--appimage-extract-and-run', app, image], {
        ARCH: architecture,
    });
    return image;
}

/**
 * 递归列出目录下的共享库
 * 显式递归而不依赖 readdirSync 的 recursive 选项，兼容旧版 Node。
 */
function sharedObjects(directory) {
    const found = [];
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
        const full = path.join(directory, entry.name);
        if (entry.isDirectory()) {
            found.push(...sharedObjects(full));
        } else if (entry.name.endsWith('.so')) {
            found.push(full);
        }
    }
    return found;
}

/**
 * 写入 AppImage 根目录的入口文件
 * AppRun 只补运行环境变量并转交参数，程序自身仍按 AppDir 前缀查找资源。
 */
function prepareLinuxAppDir(app) {
    const desktop = 'rhine-music-database.desktop';
    fs.writeFileSync(
        path.join(app, desktop),
        [
            '[Desktop Entry]',
            'Type=Application',
            'Name=RHINE MUSIC DATABASE',
            'GenericName=Music Library',
            'Comment=原生音乐资料库',
            'Exec=RhineMusicDatabase %U',
            'Icon=rhine-music-database',
            'Terminal=false',
            'Categories=AudioVideo;Audio;Player;',
            'StartupWMClass=RhineMusicDatabase',
            'X-AppImage-Name=RhineMusicDatabase',
            '',
        ].join('\n'),
    );
    fs.copyFileSync('Assets/icons/icon-256.png', path.join(app, 'rhine-music-database.png'));
    fs.copyFileSync('Assets/icons/icon-256.png', path.join(app, '.DirIcon'));
    const appRun = path.join(app, 'AppRun');
    fs.writeFileSync(
        appRun,
        [
            '#!/bin/sh',
            '# AppImage 入口：只补运行环境，参数原样转交。',
            'set -eu',
            'here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)',
            'export LD_LIBRARY_PATH="$here/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"',
            'export QT_PLUGIN_PATH="$here/usr/plugins"',
            'export QML_IMPORT_PATH="$here/usr/qml"',
            'export QT_QPA_PLATFORM_PLUGIN_PATH="$here/usr/plugins/platforms"',
            'exit_code=0',
            '"$here/usr/bin/RhineMusicDatabase" "$@" || exit_code=$?',
            'exit "$exit_code"',
            '',
        ].join('\n'),
    );
    fs.chmodSync(appRun, 0o755);
}

/**
 * 组装独立应用并执行平台部署和签名，所有文件沿用原资源清单。
 */
function deployApp(qt, config, existingBundle = '') {
    const app = path.resolve(existingBundle || defaultAppPath);
    if (!existingBundle && fs.existsSync(app)) {
        fs.renameSync(app, app + '.previous-' + Date.now());
    }
    // 各平台资源目录：macOS 在 bundle 内，Linux 与程序同目录，Windows 在其下一级。
    const resourcePaths = new Map([
        [true, 'Contents/Resources'],
        [false, linux ? 'usr/bin/resources' : 'resources'],
    ]);
    const resources = path.join(app, resourcePaths.get(mac));
    if (mac && !existingBundle) {
        fs.cpSync('output/qt-build/RhineMusicDatabase.app', app, {
            recursive: true,
        });
    } else if (linux) {
        // AppImage 使用带前缀的 AppDir；程序与资源同目录，便于沿用现有资源查找顺序。
        // 走安装规则而非直接复制构建产物，才能带上相对 $ORIGIN 的 RPATH。
        run('cmake', ['--install', 'output/qt-build', '--prefix', path.join(app, 'usr')]);
        fs.chmodSync(path.join(app, 'usr/bin/RhineMusicDatabase'), 0o755);
    } else if (!mac) {
        fs.mkdirSync(app, {
            recursive: true,
        });
        fs.copyFileSync('output/qt-build/RhineMusicDatabase.exe', path.join(app, 'RHINE MUSIC DATABASE.exe'));
    }

    fs.mkdirSync(resources, { recursive: true });
    if (mac) {
        fs.mkdirSync('output/qt-icon', {
            recursive: true,
        });
        run(path.join(app, 'Contents/MacOS/RhineMusicDatabase'), ['--make-icon', 'output/qt-icon'], {
            QT_QPA_PLATFORM: 'offscreen',
        });
        run(python, ['scripts/package-icon.py', 'output/qt-icon', path.join(resources, 'AppIcon.icns')]);
        for (const plugin of [
            'platforms/libqcocoa.dylib',
            'platforms/libqoffscreen.dylib',
            'imageformats/libqjpeg.dylib',
            'imageformats/libqgif.dylib',
            'imageformats/libqico.dylib',
            'tls/libqsecuretransportbackend.dylib',
            'multimedia/libdarwinmediaplugin.dylib',
            'multimedia/libffmpegmediaplugin.dylib',
        ]) {
            const dest = path.join(app, 'Contents/PlugIns', plugin);
            fs.mkdirSync(path.dirname(dest), {
                recursive: true,
            });
            fs.copyFileSync(path.join(qt, 'plugins', plugin), dest);
        }
        deployMacAudioLibraries(qt, app);
        if (config === 'Debug') {
            for (const plugin of ['local', 'server', 'preview', 'quickprofiler', 'quickeventreplay']) {
                const dest = path.join(app, 'Contents/PlugIns/qmltooling', `libqmldbg_${plugin}.dylib`);
                fs.mkdirSync(path.dirname(dest), {
                    recursive: true,
                });
                fs.copyFileSync(path.join(qt, 'plugins/qmltooling', path.basename(dest)), dest);
            }
        }
        run(path.join(qt, 'bin/macdeployqt'), [
            app,
            '-qmldir=' + path.join(root, 'Qt/qml'),
            '-always-overwrite',
            '-no-plugins',
        ]);
        fs.writeFileSync(
            path.join(resources, 'qt.conf'),
            '[Paths]\nPrefix=.\nPlugins=PlugIns\nQmlImports=Resources/qml\n',
        );
        fs.rmSync(path.join(app, 'Contents/PlugIns/sqldrivers'), {
            recursive: true,
            force: true,
        });
    } else if (win) {
        const executable = path.join(app, 'RHINE MUSIC DATABASE.exe');
        // windeployqt 的 debug/release 跟随 Qt 自身构建类型，与应用构建类型无关。
        const qtDebug = fs.existsSync(path.join(qt, 'bin', 'Qt6Cored.dll'));
        run(path.join(qt, 'bin/windeployqt.exe'), [
            qtDebug ? '--debug' : '--release',
            '--qmldir',
            path.join(root, 'Qt/qml'),
            '--no-translations',
            '--no-system-d3d-compiler',
            '--no-compiler-runtime',
            executable,
        ]);
        if (windowsToolchain(qt) === 'mingw') {
            deployMinGWishRuntime(app, qt);
        } else {
            deployWindowsRuntime(app, config);
        }
        // windeployqt 默认只带 Windows 平台插件；离线发布包测试也需要 offscreen 插件。
        const pluginPlatforms = path.join(qtPlugins(qt), 'platforms');
        const offscreen = ['qoffscreen.dll', 'qoffscreend.dll'].find((name) =>
            fs.existsSync(path.join(pluginPlatforms, name)),
        );
        if (!offscreen) {
            throw new Error('Qt kit has no offscreen platform plugin for offline tests');
        }
        fs.mkdirSync(path.join(app, 'platforms'), {
            recursive: true,
        });
        fs.copyFileSync(path.join(pluginPlatforms, offscreen), path.join(app, 'platforms', offscreen));
        fs.writeFileSync(path.join(app, 'qt.conf'), '[Paths]\nPlugins=.\nQmlImports=qml\n');
        fs.rmSync(path.join(app, 'sqldrivers'), {
            recursive: true,
            force: true,
        });
    }
    if (linux) {
        prepareLinuxAppDir(app);
        deployLinuxRuntime(qt, app, path.join(app, 'usr/bin/RhineMusicDatabase'));
    }
    // 先部署 Qt，再装入大量 SDK 文件，避免部署工具逐个扫描非原生依赖。
    run(process.execPath, ['scripts/package-resources.cjs', resources]);
    fs.copyFileSync('LICENSE', path.join(resources, 'LICENSE'));
    fs.copyFileSync('vendor/qrcodegen/qrcodegen.hpp', path.join(resources, 'qrcodegen-LICENSE.hpp'));
    fs.cpSync('licenses/Qt', path.join(resources, 'Qt-Licenses'), {
        recursive: true,
    });
    // Linux 的 Qt 由发行版安装，许可文件位置不固定，沿用仓库内的清单。
    const qtLicenses = linux ? '' : path.resolve(qt, '../../Licenses');
    if (qtLicenses && fs.existsSync(qtLicenses)) {
        fs.cpSync(qtLicenses, path.join(resources, 'Qt-Licenses'), {
            recursive: true,
        });
    }
    if (mac) {
        run('codesign', ['--force', '--sign', '-', path.join(resources, 'MusicBridge/node')]);
        run('codesign', ['--force', '--deep', '--sign', '-', app]);
    }
    if (linux) {
        const image = packageAppImage(app);
        const version = buildVersion();
        if (version.release) {
            fs.copyFileSync(image, `output/RhineMusicDatabase-v${version.version}-Linux-x86_64.AppImage`);
        }
        return image;
    }
    return app;
}
prepareResources();
if (process.argv[2] === '--cmake-package') {
    // Qt Creator 的链接产物原地打包，不再次启动 CMake 或覆盖其他构建目录。
    const [, , , bundle, qt, config] = process.argv;
    if (!mac || !bundle || !qt || !config) {
        throw new Error('CMake packaging requires a macOS bundle, Qt kit and build configuration');
    }
    console.log(deployApp(qt, config, path.resolve(bundle)));
} else {
    const { qt, config } = buildQt();
    console.log(deployApp(qt, config));
}
