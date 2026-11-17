#!/bin/bash
set -eu
ROOT=/d/PublicProject/ffmpeg-for-HarmonyOS
cd "$ROOT"
for pair in arm64-v8a:aarch64-linux-ohos x86_64:x86_64-linux-ohos; do
  abi=${pair%%:*}
  tri=${pair##*:}
  host=aarch64-linux-gnu
  [ "$abi" = "x86_64" ] && host=x86_64-linux-gnu
  rm -rf "build/fribidi-$abi"
  mkdir -p "build/fribidi-$abi"
  cd "build/fribidi-$abi"
  CC="$ROOT/build/subs-toolchain/cc-$abi" \
  AR="$ROOT/build/subs-toolchain/ar-$abi" \
  RANLIB="$ROOT/build/subs-toolchain/ranlib-$abi" \
  STRIP="$ROOT/build/subs-toolchain/strip-$abi" \
  LD="$ROOT/build/subs-toolchain/cc-$abi" \
  "$ROOT/third_party/fribidi-1.0.16/configure" --host="$host" \
    --prefix="$ROOT/prebuilt/$abi/subs" --enable-static --disable-shared \
    > "$ROOT/build-logs/fribidi-$abi-config.log" 2>&1
  make -j8 > "$ROOT/build-logs/fribidi-$abi-make.log" 2>&1
  make install > "$ROOT/build-logs/fribidi-$abi-install.log" 2>&1
  cd "$ROOT"
  echo "$abi OK"
done
