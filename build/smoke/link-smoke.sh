#!/bin/bash
# 以 HAP 将要采用的方式链接一个 .so：include + link prebuilt/<abi>
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NDK=${NDK:-"$ROOT/.tools/ndk"}
[ -d "$NDK/llvm/bin" ] || NDK="${DEVECO_HOME:-/d/Program Files/DevEco Studio}/sdk/default/openharmony/native"
LLVM="$NDK/llvm/bin"

for pair in "arm64-v8a aarch64" "x86_64 x86_64"; do
  set -- $pair
  abi=$1 arch=$2
  pre="$ROOT/prebuilt/$abi"
  [ -d "$pre/lib" ] || continue
  out="$ROOT/build/smoke/libffsmoke-$abi.so"
  echo "== link $abi"
  "$LLVM/clang.exe" --target="$arch-linux-ohos" --sysroot="$NDK/sysroot" \
    -fPIC -shared -O2 \
    -I"$pre/include" \
    -L"$pre/lib" -Wl,-rpath-link,"$pre/lib" \
    -lavformat -lavcodec -lavutil \
    -o "$out" "$ROOT/build/smoke/link_smoke.c"
  echo "   built $(stat -c %s "$out") B"
  "$LLVM/llvm-readelf.exe" -d "$out" | sed -n 's/.*Shared library: \[\(.*\)\]/   NEEDED \1/p'
  "$LLVM/llvm-nm.exe" -D "$out" | grep -E ' T (ff_smoke_)' | sed 's/^/  /'
done
