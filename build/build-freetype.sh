#!/bin/bash
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${NDK:-"$ROOT/.tools/ndk"}
if [ ! -d "$NDK/llvm/bin" ]; then NDK="/d/Program Files/DevEco Studio/sdk/default/openharmony/native"; fi
SRC="$ROOT/third_party/freetype-2.13.2"
TC="$ROOT/build/freetype-toolchain"
mkdir -p "$TC"
for abi_triple in "arm64-v8a:aarch64-linux-ohos" "x86_64:x86_64-linux-ohos"; do
  abi=${abi_triple%%:*}; triple=${abi_triple##*:}
  host=$(echo "$triple" | sed "s/-ohos/-linux-gnu/")
  cat > "$TC/cc-$abi" <<EOF
#!/bin/sh
exec "$NDK/llvm/bin/clang" --target=$triple --sysroot="$NDK/sysroot" "\$@"
EOF
  cat > "$TC/ar-$abi" <<EOF
#!/bin/sh
exec "$NDK/llvm/bin/llvm-ar" "\$@"
EOF
  cat > "$TC/ranlib-$abi" <<EOF
#!/bin/sh
exec "$NDK/llvm/bin/llvm-ranlib" "\$@"
EOF
  cat > "$TC/strip-$abi" <<EOF
#!/bin/sh
exec "$NDK/llvm/bin/llvm-strip" "\$@"
EOF
  cat > "$TC/ld-$abi" <<EOF
#!/bin/sh
exec "$NDK/llvm/bin/ld.lld" "\$@"
EOF
  cat > "$TC/nm-$abi" <<EOF
#!/bin/sh
exec "$NDK/llvm/bin/llvm-nm" "\$@"
EOF
  chmod +x "$TC/"*-"$abi"
  mkdir -p "$ROOT/build/freetype-$abi" "$ROOT/prebuilt/$abi"
  cd "$ROOT/build/freetype-$abi"
  CC="$TC/cc-$abi" AR="$TC/ar-$abi" RANLIB="$TC/ranlib-$abi" STRIP="$TC/strip-$abi" \
    LD="$TC/ld-$abi" NM="$TC/nm-$abi" CC_FOR_BUILD="$NDK/llvm/bin/clang" \
    "$SRC/configure" --host="$host" --prefix="$ROOT/prebuilt/$abi" \
    --enable-static --disable-shared --without-zlib --without-png --without-bzip2 \
    --without-harfbuzz --without-brotli --without-dlg > "$ROOT/build-logs/freetype-$abi-configure.log" 2>&1
  touch "$ROOT/build/freetype-$abi/ftexport.sym"
  make -j"${JOBS:-8}" > "$ROOT/build-logs/freetype-$abi-make.log" 2>&1
  make install > "$ROOT/build-logs/freetype-$abi-install.log" 2>&1
  echo "freetype $abi OK"
done
