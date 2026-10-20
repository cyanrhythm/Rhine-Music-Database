#!/usr/bin/env bash
# 统一从仓库根目录调用 npm 脚本；dash 不支持 pipefail，缺少 bash 时退化为 set -eu。
set -eu
if [ -n "${BASH_VERSION:-}" ]; then
    set -o pipefail
fi
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir/.."
exec node scripts/test.cjs
