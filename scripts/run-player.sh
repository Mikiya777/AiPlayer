#!/bin/bash
# Start the verified optimization configuration from the independent source build.
set -euo pipefail
cd "$(dirname "$0")/.."
APP="$PWD/build/repro/app/AiPlayer.app/Contents/MacOS/AiPlayer"
[[ -x "$APP" ]] || { echo '请先运行 scripts/build-repro.sh 编译应用'; exit 1; }
exec env \
 AIPLAYER_VIDEO_DECODER="${AIPLAYER_VIDEO_DECODER:-auto}" \
 AIPLAYER_RECORD_CONVERTER="${AIPLAYER_RECORD_CONVERTER:-libyuv}" \
 AIPLAYER_RECORD_ENCODER="${AIPLAYER_RECORD_ENCODER:-h264_videotoolbox}" \
 AIPLAYER_RECORD_READBACK="${AIPLAYER_RECORD_READBACK:-direct}" \
 "$APP" "$@"
