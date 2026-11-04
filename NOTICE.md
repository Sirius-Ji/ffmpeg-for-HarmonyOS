# 第三方组件与许可声明（NOTICE）

本仓库分发的二进制产品是 **FFmpeg 的交叉编译产物**。FFmpeg 以 LGPL-2.1-or-later 为主许可，
本项目在编译时**没有**开启 `--enable-gpl`、`--enable-nonfree`、`--enable-version3`，
因此产物整体按 **LGPL-2.1-or-later** 分发。

| 项 | 值 |
| --- | --- |
| 上游 | FFmpeg（https://github.com/FFmpeg/FFmpeg） |
| 版本 | `n9.0`（commit `d32b387`） |
| 目标平台 | OpenHarmony / HarmonyOS NEXT，`aarch64-linux-ohos`、`x86_64-linux-ohos` |
| 许可证全文 | [LICENSES/LGPL-2.1.txt](LICENSES/LGPL-2.1.txt) |
| 构建脚本 | [build/build-ffmpeg.sh](build/build-ffmpeg.sh) |

## 编译开关（可复现）

```sh
--target-os=linux --arch=<aarch64|x86_64> --enable-cross-compile
--enable-shared --disable-static
--disable-programs --disable-doc --disable-avdevice --disable-swscale
--disable-network --disable-iconv --disable-autodetect --disable-vulkan
--disable-everything
--enable-avformat --enable-avcodec --enable-avutil --enable-swresample
--enable-ohcodec
--enable-decoder=h264_oh,hevc_oh,aac,ac3,alac,ape,dca,flac,mp3,mp3float,opus,pcm_s16le,pcm_s24le,pcm_s32le,wmav2
--enable-encoder=aac,alac,flac,pcm_s16le,pcm_s24le
--enable-parser=aac,ac3,dca,flac,mpegaudio,opus
--enable-demuxer=mov,matroska,ogg,flac,ape,dsf,wav,mp3,aac,tta,wv,caf,asf
--enable-muxer=mp4,mov,ipod,mp3,flac,wav,matroska,adts
--enable-bsf=aac_adtstoasc
--enable-protocol=file
--extra-ldflags="-Wl,-z,max-page-size=16384"
```

未启用的组件里包含全部 GPL 部分（x264 / x265 / xvid 等）、`--enable-nonfree` 组件（fdk-aac 等）。
v0.1.1 起增加了基础转码能力（`aac/alac/flac/pcm_s16le/pcm_s24le` 编码、常用封装器），
**未**启用 MP3 编码（需要 libmp3lame）与任何视频编码。

发布包只带 4 个库：`libavcodec.so.63`、`libavformat.so.63`、`libavutil.so.61`、`libswresample.so.7`。
构建不包含 `libavfilter`：本版本不提供滤镜能力。

## 分发与合规

- 产物是**动态链接库**（带 SONAME：`libavcodec.so.63` 等），应用通过动态链接使用，
  这符合 LGPL-2.1 §6(b) 的"使用合适的共享库机制"路径。
- 分发本产物时请一并保留本文件与 `LICENSES/LGPL-2.1.txt`，并提供 FFmpeg 源码获取方式
  （上游地址或本仓库构建脚本，二选一即可，两者都能让接收者重新编译）。
- 如果你修改了 FFmpeg 源码，请**保留原有 SONAME** 并公开你的修改。
- **专利与许可证是两件事**：AAC / MP3 等编解码器的专利池授权与 LGPL 无关，
  商业分发前请自行评估。
