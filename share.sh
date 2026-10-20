#!/usr/bin/env bash
# 发布入口只负责定位脚本目录；dash 不支持 pipefail，缺少 bash 时退化为 set -eu。
set -eu
if [ -n "${BASH_VERSION:-}" ]; then
    set -o pipefail
fi
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec bash "$script_dir/scripts/share-qt.sh" "$@"
