# NOTICE

ffmpeg-for-HarmonyOS v0.3.1 is distributed under the **GNU Lesser General
Public License v2.1 or later (LGPL-2.1-or-later)**.

## Shipped shared libraries (8)

| Library | Role | License |
|---|---|---|
| libavcodec.so.63 | codec core | LGPL-2.1-or-later |
| libavformat.so.63 | container I/O | LGPL-2.1-or-later |
| libavutil.so.61 | utility | LGPL-2.1-or-later |
| libswresample.so.7 | audio resample | LGPL-2.1-or-later |
| libavfilter.so.12 | filter graph | LGPL-2.1-or-later |
| libswscale.so.10 | image scale | LGPL-2.1-or-later |
| libmp3lame.so.0 | MP3 encoder | LGPL-2.1-or-later |
| libffz.so.1 | zlib 1.3.1, SONAME-renamed | zlib License |

`libc++_shared.so` is **not shipped**. The host application must provide the
NDK C++ runtime matching its own toolchain.

## Statically linked dependencies

- **FreeType 2.13.2** — FTL (FreeType License), `LICENSES/FreeType-FTL.txt`
- **HarfBuzz 8.3.0** — MIT, `LICENSES/HarfBuzz-MIT.txt`
- **libass 0.17.3** — ISC, `LICENSES/libass-ISC.txt`
- **FriBidi 1.0.16** — LGPL-2.1-or-later, `LICENSES/FriBidi-LGPL-2.1.txt`
- **fontconfig 2.14.2** — MIT-style, `LICENSES/fontconfig-MIT.txt`
- **Expat 2.7.1** — MIT, `LICENSES/expat-MIT.txt`
- **libunibreak 6.1** — zlib-style, `LICENSES/libunibreak-zlib.txt`

These are linked into `libavfilter.so.12` for `drawtext` and `subtitles`.

## FFmpeg build configuration (excerpt)

```
--enable-avfilter --enable-swscale --enable-libfreetype --enable-libharfbuzz
--enable-libass
--enable-filter=drawtext,scale,overlay,format,crop,trim,setpts,subtitles,ass
--enable-decoder=png,mjpeg,srt,ass,webvtt
--enable-demuxer=image2,srt,ass,webvtt
--enable-zlib
--disable-gpl --disable-nonfree --disable-libx264 --disable-libx265
--disable-asm (arm64), --disable-x86asm (x86_64)
```

No GPL, nonfree, x264 or x265 components are enabled. The complete configure
lines and logs are produced by `build/build-ffmpeg.sh`.

## Source and licenses

- FFmpeg: LGPL-2.1-or-later, `LICENSES/FFmpeg-LGPL-2.1.txt`
- zlib: zlib License (see upstream `zlib.h`)
- FreeType: FTL / GPLv2 dual-licensed; this distribution uses FTL
- HarfBuzz: MIT
- libass/FriBidi/fontconfig/Expat/libunibreak: see `LICENSES/`, statically linked for subtitles

The corresponding source archives and build patches are in the repository.
