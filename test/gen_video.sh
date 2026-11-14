#!/usr/bin/env bash
# 生成 M4.0 测试素材：10 秒、30fps、H.264 视频 + AAC 立体声音轨。
# 依赖桌面版全量 FFmpeg（需带 libx264）；只是测试素材，不参与库的构建。
set -euo pipefail
cd "$(dirname "$0")"

FFMPEG="${FFMPEG:-ffmpeg}"

if ! command -v "$FFMPEG" >/dev/null 2>&1; then
  echo "找不到 ffmpeg：请设置 FFMPEG=/path/to/ffmpeg.exe" >&2
  exit 1
fi

gen() {
  local name="$1" size="$2" bitrate="$3" maxrate="$4" bufsize="$5"
  echo "==> $name ($size)"
  "$FFMPEG" -y \
    -f lavfi -i "testsrc2=size=${size}:rate=30:duration=10" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=10" \
    -c:v libx264 -preset veryfast -profile:v high -pix_fmt yuv420p \
    -b:v "$bitrate" -maxrate "$maxrate" -bufsize "$bufsize" \
    -c:a aac -b:a 128k -ar 48000 -ac 2 \
    -shortest -movflags +faststart \
    "$name"
}

gen test_h264_720p.mp4  1280x720  2500k 3000k 6000k
gen test_h264_1080p.mp4 1920x1080 5000k 6000k 12000k

echo "==> 校验"
for f in test_h264_720p.mp4 test_h264_1080p.mp4; do
  "$FFMPEG" -hide_banner -i "$f" 2>&1 | grep -E "Duration|Stream #" || true
done
