#!/bin/bash
# 校验交叉编译产物：ABI、SONAME、依赖、许可开关、体积。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/prebuilt"
NDK=${NDK:-"$ROOT/.tools/ndk"}
[ -d "$NDK/llvm/bin" ] || NDK="${DEVECO_HOME:-/d/Program Files/DevEco Studio}/sdk/default/openharmony/native"
LLVM="$NDK/llvm/bin"

fail=0
for abi in arm64-v8a x86_64; do
  libdir="$OUT/$abi/lib"
  [ -d "$libdir" ] || continue
  echo "================ $abi ================"
  for f in "$libdir"/lib{avformat,avcodec,avutil,swresample,avfilter}.so.[0-9]* ; do
    [ -e "$f" ] || continue
    base=$(basename "$f")
    machine=$("$LLVM/llvm-readelf.exe" -h "$f" | sed -n 's/^ *Machine: *//p')
    soname=$("$LLVM/llvm-readelf.exe" -d "$f" | sed -n 's/.*Library soname: \[\(.*\)\]/\1/p')
    needed=$("$LLVM/llvm-readelf.exe" -d "$f" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')
    flags=$("$LLVM/llvm-readelf.exe" -d "$f" | sed -n 's/.*(FLAGS) *//p')
    size=$(stat -c %s "$f")
    printf '%-20s %-9s soname=%-16s %8d B  flags=%s\n' "$base" "$machine" "${soname:-<none>}" "$size" "${flags:-none}"
    printf '   needed: %s\n' "$needed"
    [ -n "$soname" ] || { echo "   !! 缺 SONAME"; fail=1; }
    case "$abi" in
      arm64-v8a) [ "$machine" = "AArch64" ] || { echo "   !! ABI 不对"; fail=1; } ;;
      x86_64)    case "$machine" in *X86-64*) ;; *) echo "   !! ABI 不对"; fail=1 ;; esac ;;
    esac
  done
done

echo "================ 许可开关（config.h） ================"
for abi in arm64-v8a x86_64; do
  ch="$ROOT/build/$abi/config.h"
  [ -f "$ch" ] || continue
  printf '%s: ' "$abi"
  grep -E '^#define CONFIG_(GPL|NONFREE|VERSION3) ' "$ch" | tr '\n' ' '
  echo
done

echo "================ ohcodec 组件 ================"
for abi in arm64-v8a x86_64; do
  cc="$ROOT/build/$abi/config_components.h"
  [ -f "$cc" ] || continue
  printf '%s: ' "$abi"
  grep -E '^#define CONFIG_(H264|HEVC)_OH_DECODER ' "$cc" | tr '\n' ' '
  echo
done

echo "================ 体积 ================"
du -sh "$OUT"/* 2>/dev/null
exit $fail
