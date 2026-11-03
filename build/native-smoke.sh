#!/bin/bash
# 用 DevEco 自带的 CMake + ohos 工具链离线编译 NAPI 模块（arm64），
# 用于不打开 IDE 也能验证 native 代码可编译可链接。
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEVECO_HOME=${DEVECO_HOME:-/d/Program Files/DevEco Studio}
CMAKE_DIR="$DEVECO_HOME/sdk/default/openharmony/native/build-tools/cmake/bin"
CMAKE="$CMAKE_DIR/cmake.exe"
TC="$DEVECO_HOME/sdk/default/openharmony/native/build/cmake/ohos.toolchain.cmake"
ARCH=${1:-arm64-v8a}
OUT="$ROOT/build/napi/$ARCH"

# ninja.exe 和 cmake.exe 在同一个目录，塞进 PATH 免得 CMake 找不到生成器
export PATH="$CMAKE_DIR:$PATH"

rm -rf "$OUT"
"$CMAKE" -S "$ROOT/harmony/entry/src/main/cpp" -B "$OUT" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$TC" \
  -DOHOS_ARCH="$ARCH" \
  -DOHOS_PLATFORM=OHOS \
  -DCMAKE_BUILD_TYPE=Debug > "$ROOT/build-logs/cmake-$ARCH.log" 2>&1 || {
    echo "CONFIGURE_FAIL"; tail -30 "$ROOT/build-logs/cmake-$ARCH.log"; exit 1; }

"$CMAKE" --build "$OUT" >> "$ROOT/build-logs/cmake-$ARCH.log" 2>&1 || {
  echo "BUILD_FAIL"; tail -40 "$ROOT/build-logs/cmake-$ARCH.log"; exit 1; }

echo "BUILD_OK"
ls -l "$OUT/libentry.so"
