#!/bin/bash
# FreeType 2.13.2 and HarfBuzz 8.3.0 are reused from prebuilt/<abi>
# (the existing v0.2.2 libavfilter closure); they are not rebuilt here.
set -eu
ROOT=/d/PublicProject/ffmpeg-for-HarmonyOS
for pair in arm64-v8a:aarch64-linux-gnu x86_64:x86_64-linux-gnu; do
  abi=${pair%%:*}
  host=${pair##*:}
  subs="$ROOT/prebuilt/$abi/subs"
  ft="$ROOT/prebuilt/$abi"
  rm -rf "build/libass-$abi"
  mkdir -p "build/libass-$abi"
  cd "build/libass-$abi"
  CC="$ROOT/build/subs-toolchain/cc-$abi" \
  AR="$ROOT/build/subs-toolchain/ar-$abi" \
  RANLIB="$ROOT/build/subs-toolchain/ranlib-$abi" \
  STRIP="$ROOT/build/subs-toolchain/strip-$abi" \
  LD="$ROOT/build/subs-toolchain/cc-$abi" \
  PKG_CONFIG="$ROOT/build/pkg-config-subs" \
  FT_PREFIX="$ft" HB_PREFIX="$ft" SUBS_PREFIX="$subs" \
  "$ROOT/third_party/libass-0.17.3/configure" --host="$host" \
    --prefix="$subs" --enable-static --disable-shared --disable-asm \
    --enable-libunibreak > "$ROOT/build-logs/libass-$abi-config.log" 2>&1
  make -j8 > "$ROOT/build-logs/libass-$abi-make.log" 2>&1
  make install > "$ROOT/build-logs/libass-$abi-install.log" 2>&1
  cd "$ROOT"
  echo "$abi OK"
done
