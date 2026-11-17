#!/bin/bash
# Expat 2.7.1 is built under prebuilt/<abi>/subs.
# FreeType is reused from prebuilt/<abi> (existing v0.2.2 closure).
set -eu
ROOT=/d/PublicProject/ffmpeg-for-HarmonyOS
chmod +x "$ROOT/build/pkg-config-subs"
for pair in arm64-v8a:aarch64-linux-gnu x86_64:x86_64-linux-gnu; do
  abi=${pair%%:*}
  host=${pair##*:}
  subs="$ROOT/prebuilt/$abi/subs"
  ft="$ROOT/prebuilt/$abi"
  rm -rf "build/fontconfig-$abi"
  mkdir -p "build/fontconfig-$abi"
  cd "build/fontconfig-$abi"
  CC="$ROOT/build/subs-toolchain/cc-$abi" \
  AR="$ROOT/build/subs-toolchain/ar-$abi" \
  RANLIB="$ROOT/build/subs-toolchain/ranlib-$abi" \
  STRIP="$ROOT/build/subs-toolchain/strip-$abi" \
  LD="$ROOT/build/subs-toolchain/cc-$abi" \
  PKG_CONFIG="$ROOT/build/pkg-config-subs" \
  PYTHON="/c/miniconda3/python.exe" \
  FT_PREFIX="$ft" HB_PREFIX="$ft" SUBS_PREFIX="$subs" \
  "$ROOT/third_party/fontconfig-2.14.2/configure" --host="$host" \
    --prefix="$subs" --enable-static --disable-shared --disable-docs \
    --with-expat="$subs" > "$ROOT/build-logs/fontconfig-$abi-config.log" 2>&1
  make -j8 > "$ROOT/build-logs/fontconfig-$abi-make.log" 2>&1
  make install > "$ROOT/build-logs/fontconfig-$abi-install.log" 2>&1
  cd "$ROOT"
  echo "$abi OK"
done
