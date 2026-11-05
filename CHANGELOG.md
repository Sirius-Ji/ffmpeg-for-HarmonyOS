# Changelog

所有值得注意的变更都记在这里。版本号遵循 [SemVer](https://semver.org/lang/zh-CN/)。

## v0.1.5（2026-11-05）

### 可复现构建验证（空版本）

本次**未修改任何 C++ / 构建脚本**，只改文档与校验脚本；对 v0.1.4 源码原样重编双 ABI，
5 个 `.so` 的 sha256 与 v0.1.4 **完全一致（10/10）**：

**arm64-v8a**

```
2A3B6984793783BF979704E7B07BBB728145AF9B406901270179BA60F9DF530C  libavcodec.so.63
EDB964283A84D81F9591B71743EA2CC27F2DFD609F4027FC2838689A6AA158F4  libavformat.so.63
CB97BA8C34B3D5ADD6316479541AD2A30F6E3E8D5B2F6B0943D2F7EC557643E7  libavutil.so.61
2090EC7257951D8B3D2F0A16D1A520FA908A5511309071BAA379EE416D3878D5  libswresample.so.7
966E6195E13763A4411FD2F465506756DC945CE473D05DB6559D69FC0920A1DB  libmp3lame.so.0
```

**x86_64**

```
B5AEB3A66522885FDE4993AA328123C7825E7100F7EFC402DBFC6057D272C339  libavcodec.so.63
C6FB159A06632416DD501976A5A6E1BCC48E6AAC78AF8B0D514A0A1C1D114596  libavformat.so.63
F04EC698CD83FD818D92871F3120D3FA7206940B78C0C353B27C904EFBB1DA9D  libavutil.so.61
7C7F4795387FFDABCD379FB473471DF85D8AFA736E675FB234E3B989B63F59F4  libswresample.so.7
BC11A401217A1B4760A728748C710ECDD3BD2244558AF0A42091C4B0DD798876  libmp3lame.so.0
```

### CI / 校验强化

- `check-release.py` 删除 eac3/config.cmake 固定文案断言，改为解析 `BUILD-INFO.txt` 的
  `release_notes` 字段，必须包含当前版本 Tag。
- 新增 **CMake 消费测试**：解包后用 3 行 `CMakeLists.txt` 跑 `find_package(ffmpeg_ohos REQUIRED)`，失败即中止发布。
- 新增 `--source-only` 模式，供 CI 做源码 / 文档 / 组件白名单一致性检查。
- `.github/workflows/ci.yml`：push / PR 到 `main` 时自动运行。

### 文档

- README 增加 **ABI 稳定性承诺**：锁定 SONAME `.so.63` / `.so.61` / `.so.7`，n10 升级视为破坏性更新。

## v0.1.4（2026-11-05）

### 变更

- **可复现构建**：`build/build-ffmpeg.sh` 加入 `-ffile-prefix-map` 与 `SOURCE_DATE_EPOCH`，
  未改动的库重新构建后 sha256 保持稳定。
- `transcodeTo` / `transcodeByPath` 新增可选 `bitrateKbps`（32–512），
  默认 AAC 128 / MP3 192 / Opus 96；FLAC/WAV 忽略该参数。
- README：§2 转码封装措辞修正为 `ipod（m4a）`；CMake 集成明确包名是 `ffmpeg_ohos`（下划线）。

### 修复

- `check-release.py` 新增严格校验：包内 5 个 `.so` 的 sha256 必须与 `BUILD-INFO.txt` 记录一致。

## v0.1.3（2026-11-05）

### 新增

- 编入 **E-AC3 解码器**（`eac3`，FFmpeg 原生实现），修复部分全景声 M4A（E-AC3 音轨）无法解码。
- Release 包新增 **`ffmpeg-ohos-config.cmake`**：`find_package(ffmpeg_ohos REQUIRED)` 自动带入
  头文件、5 个 `.so`、LAME 依赖和 `-rpath-link`。
- 进程内 **硬件黑名单**：某 mime 触发 L2 运行期回退后，本进程后续直接走软解，避免反复重试。

### 变更

- **AAC ADTS profile 改为查表映射**（LC/HE/HEv2），修正 HE-AAC 硬解“创建成功但无输出”。
- **批量转码默认软解**：NAPI 转码接口明确不创建 `OH_AudioCodec` 实例，转码瓶颈在编码器。
- 文档口径修正：`libmp3lame.so.0` 明确为**强制运行时依赖**；M3 状态改为 M3.0/M3.1 已合入。
- README 增补**已知限制**：网易云「臻音全景声」等 Dolby Atmos（AC-4）m4a 不支持（FFmpeg 主线无 AC-4 解码器）。

### 修复

- AAC 硬解送 EOS 时偶发 `INVALID_VAL`，不再误触发 fallback / 硬件黑名单，避免播放尾部被截断。
- 音频流选择改为自行扫描并精确报错：`unsupported audio codec: <name> (not compiled in)`，
  并输出每条流的 `codec` / `codec_tag`，不再把“不支持的编码”误报成“没有音频流”。
- 硬解回退只统计数据路径上的失败；EOS 阶段的瞬时错误不拉黑。

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
