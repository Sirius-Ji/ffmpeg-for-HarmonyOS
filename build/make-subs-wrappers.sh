#!/bin/bash
set -eu
ROOT=/d/PublicProject/ffmpeg-for-HarmonyOS
NDK="/d/Program Files/DevEco Studio/sdk/default/openharmony/native"
mkdir -p "$ROOT/build/subs-toolchain"
for pair in arm64-v8a:aarch64-linux-ohos x86_64:x86_64-linux-ohos; do
  abi=${pair%%:*}
  tri=${pair##*:}
  cat > "$ROOT/build/subs-toolchain/cc-$abi" <<EOF
#!/bin/sh
exec "$NDK/llvm/bin/clang.exe" --target=$tri --sysroot="$NDK/sysroot" "\$@"
EOF
  cat > "$ROOT/build/subs-toolchain/ar-$abi" <<EOF
#!/bin/sh
exec "$NDK/llvm/bin/llvm-ar.exe" "\$@"
EOF
  cat > "$ROOT/build/subs-toolchain/ranlib-$abi" <<EOF
#!/bin/sh
exec "$NDK/llvm/bin/llvm-ranlib.exe" "\$@"
EOF
  cat > "$ROOT/build/subs-toolchain/strip-$abi" <<EOF
#!/bin/sh
exec "$NDK/llvm/bin/llvm-strip.exe" "\$@"
EOF
  chmod +x "$ROOT/build/subs-toolchain/"*-"$abi"
done
