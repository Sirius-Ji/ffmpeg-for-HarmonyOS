#!/bin/bash
# 编一个能在设备 /data/local/tmp 直跑的自检程序，绕开 HAP 签名。
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NDK="${DEVECO_HOME:-/d/Program Files/DevEco Studio}/sdk/default/openharmony/native"
OUT="$ROOT/build/device-test"

mkdir -p "$OUT"
"$NDK/llvm/bin/clang++.exe" --target=aarch64-linux-ohos --sysroot="$NDK/sysroot" \
  -O2 -fPIE -pie \
  -I"$ROOT/harmony/entry/src/main/cpp" \
  -I"$ROOT/prebuilt/arm64-v8a/include" \
  "$OUT/main.cpp" \
  "$ROOT/harmony/entry/src/main/cpp/decoder.cpp" \
  -L"$ROOT/prebuilt/arm64-v8a/lib" -Wl,-rpath-link,"$ROOT/prebuilt/arm64-v8a/lib" \
  -L"$NDK/llvm/lib/aarch64-linux-ohos" \
  -lavformat -lavcodec -lavutil -lswresample -lc++_shared \
  -o "$OUT/ffmtest"

cp -f "$NDK/llvm/lib/aarch64-linux-ohos/libc++_shared.so" "$OUT/libc++_shared.so"
echo "built: $(stat -c %s "$OUT/ffmtest") B"
"$NDK/llvm/bin/llvm-readelf.exe" -d "$OUT/ffmtest" | sed -n 's/.*Shared library: \[\(.*\)\]/  NEEDED \1/p'
