#!/bin/bash
# 把 prebuilt/<abi>/lib 下“带大版本号”的 .so 拷进 HAP 工程。
# 只拷 libavcodec.so.63 这种名字：DT_NEEDED 里写的就是它，无版本号的副本不需要。
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)

for abi in arm64-v8a x86_64; do
  src="$ROOT/prebuilt/$abi/lib"
  dst="$ROOT/harmony/entry/libs/$abi"
  [ -d "$src" ] || { echo "skip $abi: 没有 $src"; continue; }
  mkdir -p "$dst"
  rm -f "$dst"/libav*.so* "$dst"/libswresample.so* "$dst"/libmp3lame.so*
  cp -f "$src"/libav*.so.[0-9]* "$dst"/ 2>/dev/null || true
  cp -f "$src"/libswresample.so.[0-9]* "$dst"/ 2>/dev/null || true
  cp -fL "$src"/libmp3lame.so.[0-9]* "$dst"/ 2>/dev/null || true
  # 去掉 libavcodec.so.63.1.100 这类完整版本号副本
  find "$dst" -maxdepth 1 -type f -name '*.so.[0-9]*.[0-9]*.[0-9]*' -delete
  # 头文件也一起带过去，工程自包含
  rm -rf "$dst/include"
  cp -r "$ROOT/prebuilt/$abi/include" "$dst/include"
  echo "== $abi"
  ls -l "$dst" | sed 's/^/   /'
done
