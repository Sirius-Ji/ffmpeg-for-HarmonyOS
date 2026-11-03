# M0：把 FFmpeg 编到鸿蒙上

> 状态：**M0 已完成**（2026-11-03），两套 ABI 产物已在 `prebuilt/` 下编出并通过静态校验；运行期加载待真机验证。

## 目标

- 用 DevEco 自带 NDK（clang 15 / musl sysroot）交叉编译 FFmpeg **n9.0**，产出 `arm64-v8a` 与 `x86_64` 两套 `.so`
- 打开 `--enable-ohcodec`，拿到 `h264_oh` / `hevc_oh` 两个鸿蒙硬解封装
- 不编任何软件音频解码器（解码权交给系统 `OH_AudioCodec`），只留解封装 + swresample
- 产物落到 `prebuilt/<abi>/{include,lib}`，供 HAP 直接引用

## 前置

| 项 | 值 |
| --- | --- |
| NDK | `<DevEco 安装目录>\sdk\default\openharmony\native`（注意路径带空格） |
| MSYS2 | `.tools/msys64`（base tarball，tuna 镜像下载 42.8 MB） |
| make | GNU Make 4.4.1 |
| 源码 | `third_party/ffmpeg`，tag `n9.0`（commit `d32b387`） |
| 目标 API | 20（`compatibleSdkVersion`），本机只装了 API 26 SDK |

## 一条命令

```bash
# 在 MSYS2 的 bash 里（<repo>\.tools\msys64\usr\bin\bash.exe）
cd <repo>
./build/build-ffmpeg.sh arm64-v8a     # 或 x86_64 / all
./build/verify-artifacts.sh
```

构建日志在 `build-logs/`，中间产物在 `build/<abi>/`。

## configure 实参（脚本里固化）

```sh
--target-os=linux --arch=aarch64 --enable-cross-compile
--cc=build/toolchain/aarch64/cc --ld=同 cc           # wrapper: clang --target=aarch64-linux-ohos --sysroot=...
--ar/--ranlib/--nm/--strip=build/toolchain/aarch64/*  # wrapper，见坑 3
--host-cc=build/toolchain/aarch64/cc                  # 见坑 2
--enable-shared --disable-static
--disable-programs --disable-doc --disable-avdevice --disable-swscale
--disable-network --disable-iconv --disable-autodetect --disable-vulkan
--disable-everything
--enable-avformat --enable-avcodec --enable-avutil --enable-swresample
--enable-ohcodec
--enable-decoder=h264_oh,hevc_oh
--enable-demuxer=mov,matroska,ogg,flac,ape,dsf,wav,mp3,aac,tta,wv,caf,asf
--enable-protocol=file
```

## 踩到的坑（都是真机实测）

### 1. NDK 路径带空格，`--sysroot` 会被 configure 拆开

`--sysroot=<带空格的安装路径>\...` 进 configure 后拼进 CFLAGS 时没有引号，`/d/Program` 就被当成命令。做法：**不传 `--sysroot`**，让 wrapper 内部带引号地传。

### 2. host 编译器：gcc 装不上，用交叉 clang 顶

msys 的 `gcc` 包在本机 **反复在 `gcc.exe` 这一个文件名上解包失败**（`Can't create '/usr/bin/gcc.exe'`，但同目录 `touch gcc.exe` 正常）——高度怀疑 360 拦编译器落盘。绕法：`--host-cc` 指交叉 clang（clang 15 支持 C11，`test_host_cc` 能过），tpc 官方移植脚本也是这么干的。

### 3. configure 里 `$nm $TMPO` 没有引号

`third_party/ffmpeg/configure:6558` 是 `sym=$($nm $TMPO | awk ...)`，`$nm` 指向带空格的 NDK 路径时直接断成 `/d/Program`。做法：给 `ar/ranlib/nm/strip` 也各生成一个无空格路径的 wrapper。

### 4. MSYS2 在受限沙箱里起不来

`bash.exe` 报 `couldn't create signal pipe, Win32 error 5`——受限模式禁用命名管道。所以 `configure && make` 只能在完全权限下跑。另外 DSH 的 pwsh 是 Low 完整性级别，fs 工具建出来的目录是 Medium，Low 子进程写不进去；构建目录要用 pwsh 自己建（继承 Low 标签）。

### 5. 别用 `.a` 静态链进 HAP（许可证）

见 `docs/00-recon.md` §6.6：动态 `.so` + 保留 SONAME 才走得通 LGPL §6(b)。

## 产物验收（`build/verify-artifacts.sh`）

逐个库检查：

1. `llvm-readelf -h` → `Machine: AArch64` / `X86-64`
2. `llvm-readelf -d` → **必须有 SONAME**（鸿蒙加载器按 SONAME 找文件；没有的话链接器会把绝对路径写进 dynamic 段，装机必挂）
3. `NEEDED` 只能出现 `libc.so` / `libnative_media_*` / `libnative_window.so` 这类系统库
4. `llvm-nm -D -u` 列出未定义的 `OH_*` 符号，逐个对头文件的 `introduced=` 版本，确认 ≤ API 20
5. `ffbuild/config.mak` 里 `CONFIG_GPL` / `CONFIG_NONFREE` 必须为 no

## 结果

### arm64-v8a（2026-11-03 21:02–21:17）

| 项 | 结果 |
| --- | --- |
| configure 耗时 | ≈ 11 分钟（MSYS 下每个探测都要 fork 一次，比 Linux 慢一个数量级） |
| make -j24 耗时 | ≈ 4 分钟，290 个 .o |
| 产物 | `prebuilt/arm64-v8a/lib/` + `include/`（含 `libavutil/hwcontext_oh.h`） |
| 体积 | libavutil 671 KB、libavformat 537 KB、libavcodec 1081 KB、libswresample 90 KB、libavfilter 123 KB（第二轮加了软解白名单，libavcodec 从 371 KB 涨到 1081 KB） |
| 页对齐 | `PT_LOAD Align = 0x4000`（16 KB，`--extra-ldflags="-Wl,-z,max-page-size=16384"` 生效） |
| 软解白名单 | aac / alac / ape / ac3 / dca(DTS) / flac / opus / wmav2 + 对应 parser |
| SONAME | 有：`libavcodec.so.63` / `libavformat.so.63` / `libavutil.so.61` / `libswresample.so.7` |
| NEEDED | `libavutil.so.61`、`libnative_media_vdec.so`、`libnative_media_codecbase.so`、`libnative_media_core.so`、`libc.so` —— **ohcodec 真的链上了** |
| FLAGS | `SYMBOLIC BIND_NOW`（ffmpeg 自己就带 `-Wl,-Bsymbolic`，不用手加） |
| 许可 | `CONFIG_GPL=0 CONFIG_NONFREE=0 CONFIG_VERSION3=0`，configure 输出 `License: LGPL version 2.1 or later` |
| ohcodec | `CONFIG_OHCODEC=1`、`CONFIG_H264_OH_DECODER=1`、`CONFIG_HEVC_OH_DECODER=1`，`ohdec.o` 里能看到 `ff_h264_oh_decoder` / `ff_hevc_oh_decoder` |
| 符号审计 | 39 个 `OH_*` 未定义符号，**最高引入版本 API 11**（基线 20）→ 通过，明细见 `prebuilt/arm64-v8a/oh-symbol-audit.md` |

### x86_64（模拟器，2026-11-03 21:18–21:40）

同样的配置，只换了 `--arch=x86_64` 和 `--disable-x86asm`（本机没有 nasm，模拟器性能不重要）：

| 库 | Machine | SONAME | 体积 |
| --- | --- | --- | --- |
| libavutil.so.61 | X86-64 | `libavutil.so.61` | 690 KB |
| libavformat.so.63 | X86-64 | `libavformat.so.63` | 572 KB |
| libavcodec.so.63 | X86-64 | `libavcodec.so.63` | 410 KB |
| libswresample.so.7 | X86-64 | `libswresample.so.7` | 108 KB |

`CONFIG_GPL/NONFREE/VERSION3` 全 0；`CONFIG_H264_OH_DECODER` / `CONFIG_HEVC_OH_DECODER` 全 1；符号审计同样 PASS。


### 下游链接验证（`build/smoke/link-smoke.sh`）

按 HAP 里的引用方式（`-I prebuilt/<abi>/include -L prebuilt/<abi>/lib -lavformat -lavcodec -lavutil`）编了一个 13 KB 的测试 `.so`：

```
== link arm64-v8a        == link x86_64
   NEEDED libavformat.so.63   NEEDED libavformat.so.63
   NEEDED libavcodec.so.63    NEEDED libavcodec.so.63
   NEEDED libavutil.so.61     NEEDED libavutil.so.61
   NEEDED libc.so             NEEDED libc.so
```

导出符号正常（`ff_smoke_probe` / `ff_smoke_version` / `ff_smoke_oh_video_decoders`）。说明这套 `include/lib` 布局对下游是可直接用的，`libentry.so` 照这个写法就行。

### 关于"能不能加载"（重要，别被体积表骗了）


本机 `hdc list targets` 是空的，**运行期加载没法在这里验证**。已完成的静态证据：

1. 链接期：所有 `OH_*` 符号都能在 NDK stub 里解析（否则 lld 直接报 undefined）
2. 引入版本：最高 API 11，API 20 设备上这些符号一定存在
3. 依赖：NEEDED 只有 `libc.so` + `libnative_media_*`，没有绝对路径
4. 符号版本：`.gnu.version` 全是 `*global*`，没有要求特定符号版本

剩下两件事只能在真机上过：

- **4 KB 页对齐**：现在 `PT_LOAD Align = 0x1000`。如果目标机是 16 KB 页，需要 `-Wl,-z,max-page-size=16384` 重链（加进 `--extra-ldflags` 即可）
- **HAP 打包**：`NEEDED` 写的是 `libavcodec.so.63`，所以 `entry/libs/arm64-v8a/` 里**文件名必须带版本后缀**（`libavcodec.so.63`，不是 `libavcodec.so`）。`make install` 在 Windows 上把三个名字都写成了实体文件，打包时只拷 `.so.63` 这一份就行，别把三份都塞进去

