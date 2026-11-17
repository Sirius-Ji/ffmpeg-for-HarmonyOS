#!/bin/sh
# x86_64 runtime acceptance helper. GUI image download is a user action.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HDC=${HDC:-"D:/Program Files/DevEco Studio/sdk/default/openharmony/toolchains/hdc.exe"}
HAP=${HAP:-"$ROOT/harmony/entry/build/default/outputs/default/entry-default-signed.hap"}
TARGET=${TARGET:-}
[ -n "$TARGET" ] || { echo "set TARGET=<hdc target> from: $HDC list targets"; exit 2; }
"$HDC" -t "$TARGET" shell getprop ro.product.cpu.abi
"$HDC" -t "$TARGET" install -r "$HAP"
"$HDC" -t "$TARGET" shell aa force-stop com.example.ffmpegforharmony
"$HDC" -t "$TARGET" shell aa start -a EntryAbility -b com.example.ffmpegforharmony
cat <<'EOF'
HAP installed on the x86_64 target.
Now tap "ASS Style 审计" and "SRT force 审计" in the app.
Then pull:
  hdc -t $TARGET file recv /data/app/el2/100/base/com.example.ffmpegforharmony/haps/entry/files/audit_ass.mp4 build-logs/x86_audit_ass.mp4
  hdc -t $TARGET file recv .../audit_srt_1x.mp4 build-logs/x86_audit_srt.mp4
Extract frames and mark filenames with abi=x86_64.
EOF
