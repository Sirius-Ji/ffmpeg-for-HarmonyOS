# Changelog

所有值得注意的变更都记在这里。版本号遵循 [SemVer](https://semver.org/lang/zh-CN/)。

## v0.1.2（2026-11-05）

### 新增

- `transcodeTo(srcFd, dstFd, format)` 的 `format` 扩展到 `flac`、`wav`（原有 `aac` / `mp3` / `opus`）。
- 新增 `transcodeByPath(srcPath, dstPath, format)`：native 层 `open()` 两个路径后复用同一条转码管线，
  ArkTS 侧不用再 `fs.openSync` 取 fd。

### 变更

- M4A 输出容器从 `mp4` 改回 **`ipod`**（movenc 的 M4A 变体），保证 `©lyr` 歌词与 iTunes 元数据兼容性。
- 转码目标采样率改为「按编码器能力选择」：AAC/MP3/Opus 固定 48 kHz；
  FLAC/WAV 跟随源采样率。新增 `supported_samplerates` 兜底——LAME 最高 48 kHz，
  源采样率更高时自动降到编码器允许的最高值并打日志，不再依赖"刚好没超"。

### 修复

- `libmp3lame.so.0` 运行时依赖警告写进 `CMakeLists.txt` 与 README：
  必须与 4 个 FFmpeg 动态库一起放进 `libs/<abi>/`，否则 `libavcodec.so` 的 DT_NEEDED 解析失败。

## v0.1.1（2026-11-04）

### 新增

- 转码链路：AAC（mp4/ipod）、MP3（libmp3lame）、Opus（原生实验性编码器）。
- LAME 3.100 交叉编译产物 `libmp3lame.so.0`，作为第 5 个发布库。
- Demo 三个转码按钮，转码后自动播放。

### 变更

- 解码器白名单补 `mp3`、`mp3float`、`pcm_s24le`、`pcm_s32le`；
  编码器补 `alac`、`pcm_s24le`；muxer 补 `ipod`、`adts`、`ogg`、`opus`。
- `check-release.py` 增加「组件 `=yes` + 包里 5 个库 + README 里 `.so` 数量与包名」硬校验。

### Breaking Changes —— 移除 libavfilter

v0.1.1 起**彻底删除 `libavfilter`**（库、头文件、`include/libavfilter/` 目录都不再随包发布）：

- 不要 `#include <libavfilter/avfilter.h>`，链接时会找不到符号；
- 不要加 `ff_avfilter` 的 IMPORTED 目标；
- 需要滤镜请自行在构建时打开 `--enable-avfilter` 并重新编译，或引入其它方案。

FFmpeg 其余 4 个库（avcodec / avformat / avutil / swresample）的接口不变。

## v0.1.0（2026-11-04）

- 首个 Alpha 版本：FFmpeg n9.0 交叉编译（arm64-v8a / x86_64，16 KB 页对齐，LGPL-2.1）。
- M1 整文件解码（与桌面逐字节对拍）、M2 OHAudio 播放链路（文件选择器 + fd 直读 + 大文件）。
