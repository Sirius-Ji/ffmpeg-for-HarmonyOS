#!/bin/bash
# Cross-build FFmpeg (n9.0) for HarmonyOS NEXT / OpenHarmony, API20+ baseline.
# 用法: build/build-ffmpeg.sh [arm64-v8a|x86_64|all]
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# NDK 位置：优先 NDK 变量，其次 .tools/ndk 软链，最后按 DevEco 默认安装位置（可用 DEVECO_HOME 覆盖）
DEVECO_HOME=${DEVECO_HOME:-/d/Program Files/DevEco Studio}
NDK=${NDK:-"$ROOT/.tools/ndk"}
if [ ! -d "$NDK/llvm/bin" ]; then
  NDK="$DEVECO_HOME/sdk/default/openharmony/native"
fi
SRC="$ROOT/third_party/ffmpeg"
OUT="$ROOT/prebuilt"
LOGS="$ROOT/build-logs"
JOBS=${JOBS:-$(nproc)}
LLVM="$NDK/llvm/bin"
SYSROOT="$NDK/sysroot"

# 路径含空格时不能把 --sysroot 直接塞进 configure（它拼进 CFLAGS 时不加引号），
# wrapper 里已经带了一份 sysroot，所以这种情况直接不传。
SYSFLAG=""
case "$SYSROOT" in
  *" "*) SYSFLAG="" ;;
  *)     SYSFLAG="--sysroot=$SYSROOT" ;;
esac

mkdir -p "$OUT" "$LOGS" "$ROOT/build"

log() { printf '\n==> %s\n' "$*"; }

wrapper() { # arch triple
  local arch=$1 triple=$2 dir="$ROOT/build/toolchain/$arch"
  mkdir -p "$dir"
  cat > "$dir/cc" <<EOF
#!/bin/sh
exec "$LLVM/clang.exe" --target=$triple --sysroot="$SYSROOT" "\$@"
EOF
  cat > "$dir/cxx" <<EOF
#!/bin/sh
exec "$LLVM/clang++.exe" --target=$triple --sysroot="$SYSROOT" "\$@"
EOF
  # configure 会不加引号地展开 \$nm/\$ar/\$ranlib/\$strip，NDK 路径里有空格就会断，
  # 所以这些也统一走无空格路径的 wrapper。
  local t
  for t in ar ranlib nm strip; do
    cat > "$dir/$t" <<EOF
#!/bin/sh
exec "$LLVM/llvm-$t.exe" "\$@"
EOF
  done
  chmod +x "$dir"/*
  echo "$dir"
}

build_one() {
  local abi=$1 arch=$2 triple=$3
  local tdir bdir
  tdir=$(wrapper "$arch" "$triple")
  bdir="$ROOT/build/$abi"
  mkdir -p "$bdir"
  cd "$bdir"

  local extra=""
  [ "$arch" = "x86_64" ] && extra="--disable-x86asm"

  log "configure $abi"
  "$SRC/configure" \
    --target-os=linux \
    --arch="$arch" \
    --enable-cross-compile \
    --cc="$tdir/cc" \
    --cxx="$tdir/cxx" \
    --ld="$tdir/cc" \
    --ar="$tdir/ar" \
    --ranlib="$tdir/ranlib" \
    --nm="$tdir/nm" \
    --strip="$tdir/strip" \
    --host-cc="${HOSTCC:-$tdir/cc}" \
    $SYSFLAG \
    --extra-cflags="-D__MUSL__ -I$OUT/$abi/include" \
    --extra-ldflags="-Wl,-z,max-page-size=16384 -L$OUT/$abi/lib" \
    --prefix="$OUT/$abi" \
    --enable-shared --disable-static \
    --disable-programs --disable-doc --disable-avdevice --disable-avfilter \
    --disable-swscale --disable-network --disable-iconv --disable-autodetect \
    --disable-vulkan \
    --disable-everything \
    --enable-avformat --enable-avcodec --enable-avutil --enable-swresample \
    --enable-ohcodec \
    --enable-decoder=h264_oh,hevc_oh,aac,ac3,alac,ape,dca,flac,mp3,mp3float,opus,pcm_s16le,pcm_s24le,pcm_s32le,wmav2 \
    --enable-encoder=aac,alac,flac,pcm_s16le,pcm_s24le,libmp3lame,opus \
    --enable-parser=aac,ac3,dca,flac,mpegaudio,opus \
    --enable-demuxer=mov,matroska,ogg,flac,ape,dsf,wav,mp3,aac,tta,wv,caf,asf \
    --enable-muxer=mp4,mov,ipod,mp3,flac,wav,matroska,adts,ogg,opus \
    --enable-libmp3lame \
    --enable-bsf=aac_adtstoasc \
    --enable-protocol=file \
    $extra \
    > "$LOGS/configure-$abi.log" 2>&1 || {
      log "configure FAILED ($abi)"; tail -40 "$LOGS/configure-$abi.log"; return 1; }

  log "make $abi (-j$JOBS)"
  make -j"$JOBS" > "$LOGS/build-$abi.log" 2>&1 || {
    log "make FAILED ($abi)"; tail -40 "$LOGS/build-$abi.log"; return 1; }

  log "install $abi -> $OUT/$abi"
  make install > "$LOGS/install-$abi.log" 2>&1 || {
    log "install FAILED ($abi)"; tail -20 "$LOGS/install-$abi.log"; return 1; }

  log "$abi done: $(du -sh "$OUT/$abi" | cut -f1)"
}

case "${1:-all}" in
  arm64-v8a) build_one arm64-v8a aarch64 aarch64-linux-ohos ;;
  x86_64)    build_one x86_64 x86_64 x86_64-linux-ohos ;;
  all)       build_one arm64-v8a aarch64 aarch64-linux-ohos
             build_one x86_64 x86_64 x86_64-linux-ohos ;;
  *) echo "usage: $0 [arm64-v8a|x86_64|all]"; exit 2 ;;
esac
