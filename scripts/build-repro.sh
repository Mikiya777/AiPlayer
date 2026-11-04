#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
QT_PREFIX="${1:?用法：bash scripts/build-repro.sh /实际/Qt/6.11.1/macos}"
[[ -f "$QT_PREFIX/lib/cmake/Qt6/Qt6Config.cmake" ]] || { echo 'Qt 路径不正确'; exit 1; }
LIBYUV_SOURCE="${LIBYUV_SOURCE:-$PWD/third_party/libyuv}"
[[ "$(git -C "$LIBYUV_SOURCE" rev-parse HEAD)" == d088b8f2bd8f29badf5ba015a24d6c38dc69f23a ]] || {
 echo '先运行 scripts/prepare-deps.sh，或设置已准备的 LIBYUV_SOURCE'; exit 1;
}
[[ -z "$(git -C "$LIBYUV_SOURCE" status --porcelain --untracked-files=no)" ]] || {
 echo 'libyuv 有改动，不能作为固定版本复现'; exit 1;
}
# Separate output tree. Existing build/Release and its local VT experiment stay intact.
CONAN_ARGS=(install . -of build/repro -s build_type=Release --build=missing)
if [[ -f conan.lock ]]; then CONAN_ARGS+=(--lockfile="$PWD/conan.lock"); fi
conan "${CONAN_ARGS[@]}"
TOOLCHAIN="$PWD/build/repro/build/Release/generators/conan_toolchain.cmake"
[[ -f "$TOOLCHAIN" ]] || { echo "未找到 toolchain：$TOOLCHAIN"; exit 1; }
cmake -S . -B build/repro/app -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
 -DAIPLAYER_LIBYUV_SOURCE="$LIBYUV_SOURCE" -DAIPLAYER_BUILD_UI_TESTS=OFF -DAIPLAYER_BUILD_GL_TESTS=OFF
cmake --build build/repro/app --target aiplayer_app --parallel "${BUILD_JOBS:-4}"
echo '构建完成：build/repro/app/AiPlayer.app（开发构建；Qt 部署/签名不在此步骤内）'
