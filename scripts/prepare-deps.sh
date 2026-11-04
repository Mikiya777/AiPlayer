#!/bin/bash
# Source dependency preparation; no mutation of generated CMake files or cached .a files.
set -euo pipefail
cd "$(dirname "$0")/.."
for COMMAND in conan git cmake; do
 command -v "$COMMAND" >/dev/null || { echo "缺少工具：${COMMAND}；请先安装并加入 PATH"; exit 1; }
done
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || {
 echo '当前 InspireFace 打包仅支持原生 macOS arm64。'; exit 1;
}
for FILE in sdk.zip Pikachu; do
 [[ -f "third_party/inspireface/$FILE" ]] || {
  echo "缺少 third_party/inspireface/$FILE；请按 README 准备官方 1.2.3 SDK/模型。"; exit 1;
 }
done
LIBYUV_REV=d088b8f2bd8f29badf5ba015a24d6c38dc69f23a
LIBYUV_SOURCE="${LIBYUV_SOURCE:-$PWD/third_party/libyuv}"
LIBYUV_REPOSITORY="${LIBYUV_REPOSITORY:-https://github.com/lemenkov/libyuv.git}"
if [[ ! -e "$LIBYUV_SOURCE" ]]; then
 git clone --no-checkout "$LIBYUV_REPOSITORY" "$LIBYUV_SOURCE"
 git -C "$LIBYUV_SOURCE" checkout --detach "$LIBYUV_REV"
fi
[[ "$(git -C "$LIBYUV_SOURCE" rev-parse HEAD)" == "$LIBYUV_REV" ]] || {
 echo "libyuv 不是固定版本 ${LIBYUV_REV}；请使用新的 LIBYUV_SOURCE 目录。"; exit 1;
}
[[ -z "$(git -C "$LIBYUV_SOURCE" status --porcelain --untracked-files=no)" ]] || {
 echo 'libyuv 存在已跟踪文件改动；请使用未修改的固定版本。'; exit 1;
}
# Independent package identity; Conan applies patches during source build.
conan create third_party/ffmpeg --user=aiplayer --channel=vtfix -s build_type=Release --build=missing
conan create third_party/inspireface -s build_type=Release --build=missing
echo "依赖已准备。libyuv：$LIBYUV_SOURCE"
echo '下一步：bash scripts/build-repro.sh /你的/Qt/6.11.1/macos'
