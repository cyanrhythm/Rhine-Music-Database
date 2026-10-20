#!/usr/bin/env bash
# MSYS2 mingw64 发布构建：校验工具链与运行时，再串行执行构建、回归和打包。
# 所有路径在交给 Windows 版 node 之前都会转成 Windows 形式，避免 /mingw64 被当成盘根路径。
set -euo pipefail

usage() {
    cat <<'EOF'
用法: ./build-msys-mingw64.sh [选项]

  --version vX.Y.Z    正式发布版本号；缺省为开发版（产物带时间戳）
  --dev               强制开发版，忽略 RHINE_VERSION
  --no-test           跳过 npm test
  --no-share          只构建，不生成 ZIP
  --clean             构建前删除 output/windows 下的旧部署副本
  --allow-local-node  允许使用非官方独立分发的 Node（不推荐）
  -h, --help          显示本帮助

环境变量:
  QT_ROOT             Qt kit 根目录，默认 /mingw64
  RHINE_NODE_BINARY   官方独立分发的 node.exe；缺省取 PATH 上的 node
  RHINE_NODE_LICENSE  对应 LICENSE；缺省取 node.exe 同目录的 LICENSE
  PYTHON              Python 命令，默认 python
EOF
}

version=""
run_test=1
run_share=1
clean=0
allow_local_node=0
while [ $# -gt 0 ]; do
    case "$1" in
    --version)
        [ $# -ge 2 ] || {
            echo "--version 需要一个版本号" >&2
            exit 2
        }
        version="$2"
        shift 2
        ;;
    --dev)
        version=""
        shift
        ;;
    --no-test)
        run_test=0
        shift
        ;;
    --no-share)
        run_share=0
        shift
        ;;
    --clean)
        clean=1
        shift
        ;;
    --allow-local-node)
        allow_local_node=1
        shift
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        echo "未知参数: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

die() {
    printf '错误: %s\n' "$*" >&2
    exit 1
}

warn() {
    printf '提示: %s\n' "$*" >&2
}

step() {
    printf '\n=== %s ===\n' "$*"
}

# Windows 版 node 不认识 MSYS2 的 /mingw64 这类路径，统一转成 C:/... 形式。
winpath() {
    cygpath -m -- "$1"
}

# 比较两个点分版本；左不大于右时为真。
version_le() {
    [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n 1)" = "$1" ]
}

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo"

command -v cygpath >/dev/null 2>&1 || die "未找到 cygpath，请在 MSYS2 提供的 shell 中运行"

# MINGW64 之外的环境（例如从 MSYS shell 启动）补上工具链目录。
if [ "${MSYSTEM:-}" != "MINGW64" ] && [ -d /mingw64/bin ]; then
    warn "MSYSTEM=${MSYSTEM:-未设置}，已把 /mingw64/bin 加入 PATH"
    PATH="/mingw64/bin:$PATH"
    export PATH
fi

step "检查工具链"
missing=""
for tool in g++ cmake ninja objdump python node npm; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
[ -z "$missing" ] || die "缺少命令:$missing（MSYS2 下安装 mingw-w64-x86_64-toolchain 与 nodejs）"
printf 'g++ %s / cmake %s / node %s\n' \
    "$(g++ -dumpversion)" \
    "$(cmake --version | head -n 1 | awk '{print $3}')" \
    "$(node --version)"

step "检查 Qt kit"
qt_root="${QT_ROOT:-/mingw64}"
[ -d "$qt_root" ] || die "QT_ROOT 不存在: $qt_root"
for module in lib/cmake/Qt6/Qt6Config.cmake lib/cmake/Qt6GuiPrivate lib/cmake/Qt6MultimediaPrivate; do
    [ -e "$qt_root/$module" ] || die "Qt kit 缺少 $module；本项目需要带私有头的 kit（Qt6GuiPrivate、Qt6MultimediaPrivate）"
done
qt_version=""
if [ -f "$qt_root/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake" ]; then
    qt_version=$(sed -n 's/^set(PACKAGE_VERSION "\(.*\)")$/\1/p' \
        "$qt_root/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake" | head -n 1)
fi
if [ -z "$qt_version" ] && [ -x "$qt_root/bin/qtpaths.exe" ]; then
    qt_version=$("$qt_root/bin/qtpaths.exe" --qt-version)
fi
[ -n "$qt_version" ] || warn "无法读取 Qt 版本，跳过版本校验"
if [ -n "$qt_version" ]; then
    minimum=$(node -p "require('./scripts/qt-kit.cjs').versions.minimum")
    release=$(node -p "require('./scripts/qt-kit.cjs').versions.release")
    version_le "$minimum" "$qt_version" || die "Qt $qt_version 低于最低要求 $minimum"
    if [ "$qt_version" != "$release" ]; then
        warn "Qt $qt_version 与发布锁定版本 $release 不同；私有头把产物绑定到 kit，本包不能与官方产物混用"
    fi
    printf 'Qt %s（最低 %s，发布锁定 %s）\n' "$qt_version" "$minimum" "$release"
fi
# Qt 自己会在 Windows SDK 里找 fxc，这里只做存在性提示，不阻塞构建。
find_fxc() {
    if command -v fxc.exe >/dev/null 2>&1; then
        command -v fxc.exe
        return 0
    fi
    for candidate in \
        "/c/Program Files (x86)/Windows Kits/10/bin/"*/x64/fxc.exe \
        "/c/Program Files/Windows Kits/10/bin/"*/x64/fxc.exe; do
        if [ -f "$candidate" ]; then
            printf '%s' "$candidate"
            return 0
        fi
    done
    return 1
}
if ! find_fxc >/dev/null; then
    warn "未在 PATH 或 Windows SDK 中找到 fxc.exe；HLSL 着色器变体可能无法编译，D3D11 后端需要它"
fi

step "检查随包 Node 运行时"
node_binary="${RHINE_NODE_BINARY:-$(command -v node)}"
[ -f "$node_binary" ] || die "RHINE_NODE_BINARY 不存在: $node_binary"
# 发布包的 Node 必须能独立运行；链接了 MSYS2 运行库的 node 换台机器就起不来。
node_deps=$(objdump -p "$(winpath "$node_binary")" 2>/dev/null | sed -n 's/.*DLL Name: \(.*\)$/\1/p' || true)
if printf '%s\n' "$node_deps" | grep -Eqi 'lib(stdc\+\+|gcc_|winpthread)'; then
    if [ "$allow_local_node" -eq 1 ]; then
        warn "Node 依赖 MSYS2 运行库，按 --allow-local-node 继续；产出的包可能无法在干净机器上启动"
    else
        die "Node 依赖 MSYS2 运行库（$(printf '%s\n' "$node_deps" | grep -Ei 'lib(stdc\+\+|gcc_|winpthread)' | tr '\n' ' ' | sed 's/ *$//')）
请从 https://nodejs.org/dist/ 下载官方 win-x64 独立包，并用 RHINE_NODE_BINARY 指向其中的 node.exe。
确实要用当前 node 时加 --allow-local-node。"
    fi
fi
node_license="${RHINE_NODE_LICENSE:-$(dirname -- "$node_binary")/LICENSE}"
[ -f "$node_license" ] || die "缺少 Node LICENSE: $node_license"
printf 'Node %s\n' "$("$node_binary" --version)"

if [ "$clean" -eq 1 ]; then
    step "清理旧部署副本"
    find output/windows -mindepth 1 -maxdepth 1 -type d -name '*.previous-*' -print -exec rm -rf {} + 2>/dev/null || true
fi

if [ ! -d node_modules ]; then
    step "安装开发依赖"
    npm ci --include=dev --no-audit --no-fund
fi

# node 脚本里的路径必须是 Windows 形式；PATH 仍用 MSYS2 的写法。
export QT_ROOT="$(winpath "$qt_root")"
export RHINE_NODE_BINARY="$(winpath "$node_binary")"
export RHINE_NODE_LICENSE="$(winpath "$node_license")"
export RHINE_BUILD_TYPE="${RHINE_BUILD_TYPE:-Release}"
export PATH="$qt_root/bin:$PATH"
if [ -n "$version" ]; then
    case "$version" in
    v[0-9]*.[0-9]*.[0-9]*) ;;
    *) die "版本号必须形如 v1.2.3，当前: $version" ;;
    esac
    export RHINE_VERSION="$version"
    printf '\n发布版本: %s\n' "$version"
else
    unset RHINE_VERSION
    printf '\n开发版构建（不设置 --version 时产物带时间戳，VERSIONINFO 标记为开发版）\n'
fi
printf '构建类型: %s\n' "$RHINE_BUILD_TYPE"

step "构建并部署"
node scripts/build.cjs

if [ "$run_test" -eq 1 ]; then
    step "离线回归"
    node scripts/test.cjs
else
    warn "已跳过 npm test"
fi

if [ "$run_share" -eq 0 ]; then
    step "完成"
    printf '部署目录: %s\n' "$(winpath "$repo/output/windows/RHINE MUSIC DATABASE")"
    exit 0
fi

step "生成发布 ZIP"
node scripts/share.cjs

step "完成"
archive=$(find output -maxdepth 1 -name '*.zip' -type f -printf '%T@ %p\n' | sort -n | tail -n 1 | cut -d' ' -f2-)
if [ -n "$archive" ]; then
    size=$((($(wc -c <"$archive") + 1048575) / 1048576))
    printf '产物: %s (%s MB)\n' "$(winpath "$repo/$archive")" "$size"
else
    warn "未在 output/ 下找到 ZIP"
fi
