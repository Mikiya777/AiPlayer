#!/bin/bash
# Convenience terminal entry; application defaults are configured in main.cpp.
set -euo pipefail
cd "$(dirname "$0")/.."
APP="$PWD/build/repro/app/AiPlayer.app/Contents/MacOS/AiPlayer"
[[ -x "$APP" ]] || { echo '请先运行 scripts/build-repro.sh 编译应用'; exit 1; }
exec "$APP" "$@"
