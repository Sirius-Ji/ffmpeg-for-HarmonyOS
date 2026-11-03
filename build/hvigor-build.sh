#!/bin/bash
# 命令行构建 HAP（不打开 IDE）。环境变量是三个坑的总和：
#   NODE_HOME —— hvigor 的 node
#   JAVA_HOME —— PackageHap 要 spawn java（DevEco 自带 JBR）
#   DEVECO_SDK_HOME —— SDK 根目录
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEVECO="${DEVECO_HOME:-/d/Program Files/DevEco Studio}"

export NODE_HOME="$DEVECO/tools/node"
export JAVA_HOME="$DEVECO/jbr"
export DEVECO_SDK_HOME="$DEVECO/sdk"
export PATH="$JAVA_HOME/bin:$NODE_HOME:$PATH"

cd "$ROOT/harmony"
"$NODE_HOME/node.exe" "$DEVECO/tools/hvigor/bin/hvigorw.js" \
    --mode module -p product=default -p module=entry@default \
    assembleHap --no-daemon "$@"

echo
echo "== 产物 =="
find "$ROOT/harmony/entry/build" -name '*.hap' -type f -exec ls -lh {} \;
