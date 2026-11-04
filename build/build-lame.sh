#!/bin/bash
# 交叉编译 LAME 3.100 的 libmp3lame（动态库）给 FFmpeg 用。
# 用法: build/build-lame.sh [arm64-v8a|x86_64|all]
#
# 注意：LAME 3.100 自带的 config.sub 不认识 "-ohos"，所以 --host 用
# aarch64-unknown-linux-gnu（只影响 libtool 命名），真正的编译器是下面这个
# --target=*-linux-ohos 的 clang wrapper。
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEVECO_HOME=${DEVECO_HOME:-/d/Program Files/DevEco Studio}
NDK=${NDK:-"$ROOT/.tools/ndk"}
if [ ! -d "$NDK/llvm/bin" ]; then
  NDK="$DEVECO_HOME/sdk/default/openharmony/native"
fi
SRC="$ROOT/third_party/lame-3.100"
OUT="$ROOT/prebuilt"
LOGS="$ROOT/build-logs"
JOBS=${JOBS:-$(nproc)}
LLVM="$NDK/llvm/bin"
SYSROOT="$NDK/sysroot"

[ -x "$SRC/configure" ] || { echo "缺少 LAME 源码: $SRC（先下载 lame-3.100.tar.gz）"; exit 2; }
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
  cat > "$dir/ar" <<EOF
#!/bin/sh
exec "$LLVM/llvm-ar.exe" "\$@"
EOF
  cat > "$dir/ranlib" <<EOF
#!/bin/sh
exec "$LLVM/llvm-ranlib.exe" "\$@"
EOF
  cat > "$dir/nm" <<EOF
#!/bin/sh
exec "$LLVM/llvm-nm.exe" "\$@"
EOF
  chmod +x "$dir"/*
  echo "$dir"
}

build_one() {
  local abi=$1 arch=$2 triple=$3 hostbug=$4
  local tdir bdir
  tdir=$(wrapper "$arch" "$triple")
  bdir="$ROOT/build/lame-$abi"
  rm -rf "$bdir"
  mkdir -p "$bdir"
  cd "$bdir"

  log "configure lame $abi"
  # libtool 默认去 PATH 里找 ld，MSYS2 没装 binutils；直接指定 NDK 的 ld.lld，
  # 并用 --with-gnu-ld 跳过它的 GNU ld 探测（lld 的 version 串它不认）。
  CC="$tdir/cc" AR="$tdir/ar" RANLIB="$tdir/ranlib" LD="$LLVM/ld.lld.exe" \
  CFLAGS="-O2 -fPIC -D__MUSL__" \
  LDFLAGS="-Wl,-z,max-page-size=16384" \
  "$SRC/configure" \
    --host="$hostbug" \
    --build=x86_64-pc-linux-gnu \
    --with-gnu-ld \
    --prefix="$OUT/$abi" \
    --enable-static --disable-shared \
    --disable-frontend --disable-dependency-tracking \
    > "$LOGS/lame-configure-$abi.log" 2>&1 || {
      log "lame configure FAILED ($abi)"; tail -40 "$LOGS/lame-configure-$abi.log"; return 1; }

  log "make lame $abi (-j$JOBS)"
  make -j"$JOBS" > "$LOGS/lame-build-$abi.log" 2>&1 || {
    log "lame make FAILED ($abi)"; tail -40 "$LOGS/lame-build-$abi.log"; return 1; }

  # libmp3lame.a 里已经包含了 mpglib 的目标文件（libtool 会把便利库并进去），
  # 不要再单独链 libmpgdecoder.a，否则会 duplicate symbol。
  local core="$bdir/libmp3lame/.libs/libmp3lame.a"
  [ -f "$core" ] || { log "missing $core"; return 1; }

  mkdir -p "$OUT/$abi/lib" "$OUT/$abi/include/lame"
  log "link libmp3lame.so.0 ($abi)"
  "$tdir/cc" -shared \
    -Wl,-soname,libmp3lame.so.0 \
    -Wl,-z,max-page-size=16384 \
    -o "$OUT/$abi/lib/libmp3lame.so.0.0.0" \
    -Wl,--whole-archive "$core" -Wl,--no-whole-archive \
    -lm >> "$LOGS/lame-build-$abi.log" 2>&1 || {
      log "link libmp3lame FAILED ($abi)"; tail -20 "$LOGS/lame-build-$abi.log"; return 1; }

  ln -sf libmp3lame.so.0.0.0 "$OUT/$abi/lib/libmp3lame.so.0"
  ln -sf libmp3lame.so.0.0.0 "$OUT/$abi/lib/libmp3lame.so"
  cp -f "$SRC/include/lame.h" "$OUT/$abi/include/lame/lame.h"

  ls -l "$OUT/$abi/lib" | grep -i mp3lame || true
}

case "${1:-all}" in
  arm64-v8a) build_one arm64-v8a aarch64 aarch64-linux-ohos aarch64-unknown-linux-gnu ;;
  x86_64)    build_one x86_64 x86_64 x86_64-linux-ohos x86_64-unknown-linux-gnu ;;
  all)       build_one arm64-v8a aarch64 aarch64-linux-ohos aarch64-unknown-linux-gnu
             build_one x86_64 x86_64 x86_64-linux-ohos x86_64-unknown-linux-gnu ;;
  *) echo "usage: $0 [arm64-v8a|x86_64|all]"; exit 2 ;;
esac
