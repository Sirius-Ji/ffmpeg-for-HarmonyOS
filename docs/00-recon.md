# FFmpeg → HarmonyOS 音频解码器：侦察报告与实施路径

- 日期：2026-11-03
- 工作区：`<repo>`
- 阶段：**仅侦察**。本文档只陈述事实、对比路线、给出路径与判据，不含实现代码。

---

## 0. 结论先行

1. **不需要"从零移植 FFmpeg"**。FFmpeg 编到鸿蒙上已经被社区和官方各走通一遍，源码级几乎不用改；真正缺的是"能直接用的成品库 + 解码后端路由 + 播放闭环"。你翻不到成品，是市场没货，不是技术不可行。
2. **真正的工作量在三个地方**：交叉编译脚本与产物裁剪、NAPI 层的解码流水线、系统 `OH_AudioCodec` 与 FFmpeg 软解的自动路由。
3. **架构选"两段式"**：先做纯 FFmpeg 软解把整条链路跑通（M1–M2），再加系统硬解路由（M3）。反过来先接硬解会同时踩两套坑，排查不动。
4. **音频硬解没有现成轮子**：上游 FFmpeg 的 `--enable-ohcodec` 只接了 h264/hevc（视频），音频的 `OH_AudioCodec` 桥得自己写。这是本项目唯一原创度较高的部分。
5. **许可证先定死**：不开 `--enable-gpl` / `--enable-nonfree`。链接方式（静态 `.a` 进 `libentry.so` vs 独立 `.so`）直接决定 LGPL 合规成本，见 §6.6。

---

## 1. 本机实况（全部实测）

### 1.1 本机那份 ffmpeg 只是 Windows 发行版，不含源码

```
<ffmpeg 发行版目录>\
├── bin\{ffmpeg,ffplay,ffprobe}.exe      # 220 MB 静态单文件
├── doc\  presets\
```

- 版本：`2026-10-22-git-bf0e831606`，libavcodec 63.2.100 / libavformat 63.11.101 / libswresample 7.3.101
- 构建：gyan.dev 全量包，`--enable-gpl --enable-version3 --enable-static`，外加 x264/x265 等一堆 GPL 件
- **没有 `include/`、没有 `lib/`**。它对项目的唯一用途是：在你电脑上做"参考答案"——同一文件、同一参数，对比设备上解出来的 PCM。

结论：源码得重新拉，这条不是问题，但别指望"以我电脑上这份为基础"。

### 1.2 DevEco Studio 与鸿蒙侧能力（够用，且比预想的好）

| 项 | 实测值 |
| --- | --- |
| DevEco Studio | 26.0.0.865（build 261.23567.138.36.2600865） |
| SDK | `sdk\default`，`apiVersion: 26`，`HarmonyOS 26.0.0`（release） |
| NDK | `sdk\default\openharmony\native`：clang 15.0.4（OHOS 版）、sysroot（musl） |
| 目标三元组 | `--target=aarch64-linux-ohos`，预定义宏含 `__OHOS__` `__linux__` `__unix__` |
| sysroot ABI | `aarch64-linux-ohos` / `arm-linux-ohos` / `x86_64-linux-ohos` |
| 构建工具 | CMake 3.28 + Ninja（DevEco 自带，路径 `native\build-tools\cmake\bin`） |
| hvigor / node | hvigor 自带；node v24.14.1 |
| hdc 设备 | `hdc list targets` → **空**（当前没有真机或模拟器连接） |

**sysroot 里对音频有用的东西（已逐个确认符号存在）**：

| 头文件 | 链接库 | 内容 |
| --- | --- | --- |
| `multimedia/player_framework/native_avcodec_audiocodec.h` | `libnative_media_acodec.so` | `OH_AudioCodec_CreateByMime/Configure/PushInputBuffer/QueryInputBuffer…` |
| `native_avcodec_base.h` / `native_avbuffer.h` | `libnative_media_codecbase.so` | MIME 常量、`OH_AVBuffer`、`OH_AVCapability` |
| `native_avdemuxer.h` / `native_avsource.h` | `libnative_media_avdemuxer.so` / `avsource.so` | 系统解封装（本项目大概率不用） |
| `ohaudio/native_audiorenderer.h` | `libohaudio.so` | `OH_AudioRenderer`、`AUDIOSTREAM_LATENCY_MODE_FAST` |
| `napi/native_api.h` | `libace_napi.z.so` | Node-API 入口 |
| `rawfile/raw_file.h` | `librawfile.z.so` | 读 HAP 内 rawfile 资源 |

工具链里有个好用的小东西：`llvm\bin\aarch64-unknown-linux-ohos-clang` 是个 171 字节的 sh 脚本，自动补 `-target aarch64-linux-ohos --sysroot=… -D__MUSL__`，在 MSYS2 里可以直接当 `--cc` 用。

**平台自带音频解码能力（NDK 头文件里的 MIME 常量，按引入版本）**：

| 版本 | 新增音频 MIME |
| --- | --- |
| API 9 | AAC |
| API 10 | FLAC、Vorbis、MPEG(MP3) |
| API 11 | AMR-NB/WB、Opus、G.711μ、VIVID |
| API 12 | APE |
| API 18 | RAW |
| API 20 | G.711A |
| API 22 | ALAC、AC3、EAC3、WMAv1/v2/Pro |
| API 23 | TwinVQ、iLBC、TrueHD、DVAudio、DTS、Cook |

注意两件事：
- 官方文档的"支持格式表"里 **Opus 只出现 MIME 常量、没进支持表**，AVPlayer 的 ogg 也只写 Vorbis。Opus 能不能真解，必须上真机试（**推测：可能不行**）。
- 这些 MIME/API 是**编译期常量**，不是运行期保证。老设备（API 12/18）上 `OH_AudioCodec_CreateByMime` 完全可能返回空。

### 1.3 本机缺一个 POSIX 构建环境（第一个硬门槛）

| 命令 | 状态 |
| --- | --- |
| `bash` | 只有 Git for Windows 自带的 `bash.exe` |
| `make` / `cmake` / `ninja` / `perl` / `nasm` | **都不在 PATH**（cmake/ninja 可从 DevEco SDK 里取） |
| `git` | 有，但默认 schannel 后端**所有 https 都失败**：`SEC_E_NO_CREDENTIALS`；加 `-c http.sslBackend=openssl` 后正常 |
| `python` | miniconda（`C:\miniconda3`），https 正常 |

FFmpeg 的 `configure` 是 shell 脚本产物，最终要 `make`。所以 **M0 第一件事是装 MSYS2**（`pacman -S make diffutils gcc`，gcc 用来当 `--host-cc` 编 host 侧小工具）。WSL 也行，但多一层路径映射，不推荐。

### 1.4 网络与沙箱（会坑你）

- 可达：`gitee.com` 200、`codeload.github.com` 200、`conda-forge` 200、`pypi` 200
- 不可达：`ffmpeg.org` 直接超时（`git.ffmpeg.org` 同样），所以**别用官方 git 地址**
- ohpm 仓库根路径返回 401，属正常（包拉取按路径走），构建模板不依赖它
- 本工作区的 Windows 权限一开始缺 `WRITE_OWNER`，导致 pwsh 连启动都失败（`SetNamedSecurityInfoW 失败 (Win32 5)`）；已用诊断脚本修复，备份与回滚命令在 `<仓库父目录>\_acl-recovery\`
- **根因（2026-11-03 二次排查确认）**：DSH 的 pwsh 跑在 **Low 完整性级别**（`whoami /groups` → `S-1-16-4096`），而 fs 工具（write/read）建的目录是 Medium。Low 进程按 Windows 强制策略**不能写 Medium 目录**，所以现象是"fs 工具建的目录（`.recon`/`docs`）里跑 clang、cmd 就 `Access denied`，工作区根目录（Low 标签）却能写"。
  - 解法一：构建目录用 pwsh 自己 `New-Item -ItemType Directory` 建（继承 Low 标签），已验证 clang/git 能写
  - 解法二：MSYS2 的 bash 在受限沙箱里**根本起不来**（`couldn't create signal pipe, Win32 error 5`，命名管道被拦），所以 FFmpeg 的 `configure && make` 只能在**完全权限**下跑
- 顺带：`mklink /J` 与 `New-Item -ItemType Junction` 都被拒（Low + 受限），想给带空格的 NDK 路径做别名得在完全权限下做

---

## 2. 生态实况：三条已有路线，没有一条是"成品"

### 2.1 `tpc_c_cplusplus` 的 FFmpeg 移植（最省事的起点）

- 位置：`gitee.com/openharmony-sig/tpc_c_cplusplus/thirdparty/FFmpeg`
- 版本钉在 **n6.0**，`archs=("armeabi-v7a" "arm64-v8a")`
- **没有源码补丁**：仓里唯一的 `FFmpeg_oh_test.patch` 只改 `tests/md5.sh`（把 `md5sum -b` 改成 `md5sum`）。也就是说 FFmpeg 6.0 在 musl/ohos 上"原样能编"
- configure 的核心就是"当 linux 交叉编译"：

```sh
./configure --enable-neon --enable-asm --enable-network \
  --disable-vulkan --enable-cross-compile --enable-librtmp --disable-x86asm --enable-openssl \
  --enable-static --enable-shared --disable-doc --disable-htmlpages \
  --target-os=linux --arch=$arch --cc=${CC} --ld=${CC} --strip=${STRIP} \
  --sysroot=${OHOS_SDK}/native/sysroot
```

- 它的 HAP 集成文档推荐**静态 `.a` 直接链进 entry**，并强制加 `-Wl,-Bsymbolic`（库里有汇编，防止符号被抢占）。照抄能跑，但有两个副作用：LGPL 合规（§6.6）和包体（每个 HAP 都带一份 FFmpeg）。

### 2.2 系统侧 `openharmony/third_party_ffmpeg`（真正的适配层在这）

- 这个是编进系统镜像的组件，仓库已归档（license 元数据写的是 GPL-2.0），**不能直接拿来塞进你的 HAP**
- 但它的 `ohos_config.sh` 是唯一一份"鸿蒙补丁清单"，做法不是改源码，而是 configure 完之后 `sed` 改 `config.h`：

```sh
sed -i 's/HAVE_SYSCTL 1/HAVE_SYSCTL 0/g'                 config.h
sed -i 's/HAVE_GLOB 1/HAVE_GLOB 0/g'                     config.h
sed -i 's/HAVE_GMTIME_R 1/HAVE_GMTIME_R 0/g'             config.h
sed -i 's/HAVE_LOCALTIME_R 1/HAVE_LOCALTIME_R 0/g'       config.h
sed -i 's/HAVE_PTHREAD_CANCEL 1/HAVE_PTHREAD_CANCEL 0/g' config.h   # ohos 不导出 pthread_cancel
```

- 它的组件白名单证明"只做音频解码"的裁剪量很小：`mp2/mp3/mp3float/aac/aac_latm/ape/flac/vorbis/opus/amrnb/amrwb/ac3/gsm_ms/wmav1/wmav2/wmapro/alac/ilbc/twinvq/dvaudio/cook` + 一批 adpcm，且**没开 GPL**
- 参考价值：**先别急着打补丁**。现代 configure 在我们的 sysroot 上大概率自己就能检对（`libc.so` stub 里确实没有 `pthread_cancel`），等 configure 真报错再照抄这几行 sed。

### 2.3 上游 FFmpeg 的 ohos 支持（只有视频）

- 2025-07 合入 `libavcodec/ohdec.c` + `libavutil/hwcontext_oh.[ch]`：`h264_ohcodec` / `hevc_ohcodec`，走 `OH_VideoDecoder_CreateByName`
- `configure` 里有 `--enable-ohcodec`，检测的是 `native_avcodec_videodecoder.h`，链接 `-lnative_media_vdec -lnative_media_venc …`
- **音频没有任何 oh 后端**。所以：
  - 视频轨：可以直接用上游这套（如果以后要做视频）
  - 音频轨：自己写桥，写在 `libentry.so` 里，或者以后给上游提 PR（社区确实缺这个）

### 2.4 其他

- `ohos_ijkplayer` 已迁到 FFmpeg 8 + h264_ohcodec，是"播放器内核"形态，不是库。想省事可以拿它改，但它把渲染/控制面都绑死了，改造成本不比自研低。
- 2025-10 有一篇 CSDN 文章《在鸿蒙上使用 FFmpeg》，做法就是改 `HPKBUILD`（删 rtmp/openssl 依赖、`archs=("arm64-v8a")`、`--disable-programs`）后 `./build.sh FFmpeg`，产物丢进 `libs/arm64-v8a`。**这条路径是通的**，可以直接当 M0 的参照。
- 2026-04 那篇《移植 FFmpeg 8.1 到鸿蒙 PC》脚本里带 `--enable-gpl --enable-nonfree`，**别照抄**，那是不可再分发的组合。

---

## 3. 三条路线（含"不做"选项）

| | A. 纯 FFmpeg 软解 | B. FFmpeg 解封装 + 系统硬解 | C. 只包 AVPlayer |
| --- | --- | --- | --- |
| 工作量 | 小 | 中 | 极小 |
| 格式覆盖 | 全覆盖（含 DSD/TAK/WavPack/TTA/DTS） | 覆盖 = FFmpeg ∩ 平台（Opus/WMA 存疑，DSD 平台没有） | 只到 AAC/MP3/FLAC/Vorbis/APE/AMR |
| 功耗/发热 | 差（24/192 明显） | 好 | 好 |
| 可控性 | 完全（可做 EQ、格式转换、频谱） | 完全 | 几乎没有 |
| 包体 | +6~10 MB（arm64，裁剪后） | 同左 | +0 |
| 适合 | 第一阶段 | 最终形态 | 不满足需求 |

**推荐：A 打底，B 收尾。** 理由不是"稳妥"，而是 A 阶段的代码（解封装、重采样、输出、NAPI 控制面、错误处理）在 B 阶段 100% 复用，只多一个 `pick_decoder()` 路由。反过来先写 B，你得同时调 FFmpeg 和 `OH_AudioCodec` 两套 buffer 模型，出问题无法二分。

**明确不做**：
- 把 ffmpeg 编成命令行塞进 HAP 再 `exec`：手机侧只允许 fork，不允许 fork+exec，直接出局
- 用 AVPlayer 代替解码：拿不到 PCM，做不了后处理，格式也少
- 32 位 `armeabi-v7a`：NEXT 主力机是 arm64，先不做（省一半交叉编译时间）

---

## 4. 推荐路径：M0 → M4

时间估算按"每天 4 小时"的业余节奏给区间。

### M0 构建链：能编出 ohos 产物（0.5–1 天）

1. 装 MSYS2，`pacman -S --needed make diffutils gcc`（gcc 只用于 `--host-cc`）
2. 拉源码（固定 tag，别跟 master）：

```sh
cd /d/PublicProject/ffmpeg-for-HarmonyOS
git -c http.sslBackend=openssl clone --depth=1 https://gitee.com/mirrors/ffmpeg.git third_party/ffmpeg
cd third_party/ffmpeg && git fetch --depth=1 origin tag n7.1 && git checkout n7.1
```

3. `build/build-ffmpeg.sh` 骨架（音频最小集，LGPL）：

```sh
NDK="<DevEco 安装目录>/sdk/default/openharmony/native"
CC="$NDK/llvm/bin/aarch64-unknown-linux-ohos-clang"      # shim 自动带 -target/-sysroot/-D__MUSL__
CXX="$NDK/llvm/bin/aarch64-unknown-linux-ohos-clang++"

./configure \
  --target-os=linux --arch=aarch64 --enable-cross-compile \
  --cc="$CC" --cxx="$CXX" --ld="$CC" \
  --ar="$NDK/llvm/bin/llvm-ar" --ranlib="$NDK/llvm/bin/llvm-ranlib" \
  --nm="$NDK/llvm/bin/llvm-nm"  --strip="$NDK/llvm/bin/llvm-strip" \
  --host-cc="/mingw64/bin/gcc" \
  --enable-shared --enable-static \
  --disable-programs --disable-doc --disable-avdevice --disable-postproc \
  --disable-swscale --disable-network --disable-iconv --disable-zlib \
  --disable-vulkan --disable-autodetect \
  --disable-everything \
  --enable-avformat --enable-avcodec --enable-avutil --enable-swresample \
  --enable-protocol=file --enable-protocol=pipe \
  --enable-demuxer=mov,matroska,ogg,flac,ape,dsf,wav,mp3,aac,tta,wv,caf,asf,aiff \
  --enable-decoder=aac,aac_latm,mp3,mp3float,flac,ape,alac,vorbis,opus,ac3,eac3,truehd,dts,wmav1,wmav2,wmapro,tak,wavpack,tta,dsd_lsbf,dsd_msbf,dsd_lsbf_planar,dsd_msbf_planar \
  --enable-parser=aac,flac,mpegaudio,vorbis,opus,ac3,dca \
  --enable-bsf=null
make -j"$(nproc)"
```

`--enable-shared --enable-static` 两种都编，M1 先静态（省事、和 tpc 配方一致），要不要切动态纯看你 §7 第 4 条怎么定。

**验收判据（缺一不可）**：
- `llvm-readelf -h libavcodec.so.*` → `Machine: AArch64`
- `llvm-readelf -d` → 每个库都有 `SONAME`（没有 SONAME 会被链接器写成绝对路径，装机后加载失败，这是鸿蒙的硬要求）
- `./ffbuild/config.mak` 里 **没有** `CONFIG_GPL=yes` / `CONFIG_NONFREE=yes`
- 产物总大小记录基线（预期 6–10 MB 未 strip）

**风险**：configure 若在 `pthread_cancel`、`getauxval`、`asm` 上翻车，照 §2.2 的 sed 清单补，别改源码。

### M1 最小闭环：ArkTS → NAPI → 解出 PCM（1–3 天）

目标：**先不要出声**。把"打开文件 → 解出 PCM → 落盘 / 回传给 ArkTS"跑通。

- DevEco 建 Native C++ 工程，`entry/src/main/cpp/` 放：`napi_init.cpp`（模块注册）+ `decoder.cpp`（avformat/avcodec 生命周期）+ `CMakeLists.txt`
- `.a` 或 `.so` 放 `entry/libs/arm64-v8a/`
- 静态链接时 CMake 必须带 `-Wl,-Bsymbolic`，否则符号会被系统库抢占（tpc 文档明确要求）
- NAPI 只暴露最小面：`open(path)` / `getInfo()` / `read(buffer: ArrayBuffer)` / `seek(ms)` / `close()`
- 文件先拷到应用沙箱 `filesDir` 再传路径进来（rawfile 那套要自定义 AVIOContext，M1 不碰）

**验收判据**：同一 AAC/MP3/FLAC 文件，设备侧解出的 PCM 与桌面 `ffmpeg -i x -f s16le` 的结果对齐（长度一致、PCM 差值不超过 1 个 LSB）；hilog 打出 `demuxer/codec/sample_rate/channels/frames` 与服务端一致。

### M2 出声：接 OHAudio（2–3 天）

- `OH_AudioStreamBuilder` → `OH_AudioRenderer`，回调里从环形缓冲取 PCM
- 普通音乐**不要**开 `AUDIOSTREAM_LATENCY_MODE_FAST`（官方明确"音乐播放不建议"，而且 fast 模式不支持变速，前置条件还多）
- `libswresample` 做一次统一：任意采样率/声道 → 设备接受的 S16/F32
- 播放控制：play/pause/seek/flush/volume，seek 时 `avformat_seek_file` + `avcodec_flush_buffers` + 清空环形缓冲

**验收判据**：连续播放 10 分钟无 underrun（自己打计数器）；切歌 < 200 ms；seek 响应 < 300 ms；后台挂起恢复不炸。

### M3 硬解路由：接 `OH_AudioCodec`（3–5 天）

- 路由函数 `pick_decoder(AVCodecID)`：把 FFmpeg codec id 映射到平台 MIME（aac→`audio/mp4a-latm`、mp3→`audio/mpeg`、flac→`audio/flac`…），先 `OH_AVCodec_GetCapabilityByCategory(mime, false, HARDWARE)` 探活，再 `OH_AudioCodec_CreateByMime`
- **优先用同步模式**（`OH_MD_KEY_ENABLE_SYNC_MODE=1`，API 20+）：`QueryInputBuffer` / `QueryOutputBuffer` 取送 buffer，天然适配 FFmpeg 的 pull 循环；设备低于 API 20 就退回回调模式，自己加一层环形缓冲
- 压缩包整包推进去，`extradata` 走 `OH_MD_KEY_CODEC_CONFIG` 或带 `CODEC_DATA` 标志的首包
- **必须有回退**：CreateByMime 返回空、Configure 失败、连续 N 帧无输出，一律 fallback 到 FFmpeg 软解，并打日志

**验收判据**：拿 AAC 文件对比 M1 的软解，解码线程 CPU 时间下降 ≥ 50%；强制关掉硬解后行为与 M1 一致（这条是防回归的关键）；Opus/WMA/DSD 仍走软解且正常出声。

### M4 兼容性与发布（3–5 天）

- 样本矩阵：容器（m4a/mp3/flac/ogg/ape/dsf/mka/wv/tta）× 采样率（44.1k/48k/96k/192k）× 声道（1/2/6）
- 设备矩阵：至少 2 台不同 API 版本的真机 + x86_64 模拟器（模拟器要另编一份 `--arch=x86_64`）
- 产物：`docs/BUILD.md`（怎么编）、`LICENSE`（LGPL 文本 + FFmpeg 源码获取方式）、预编译产物放 Releases 而不是塞仓库

---

## 5. 仓库骨架

```
ffmpeg-for-HarmonyOS/
├── README.md
├── docs/00-recon.md              ← 本文
├── build/
│   ├── build-ffmpeg.sh           # 交叉编译（MSYS2 里跑）
│   └── clean.sh
├── patches/                      # 仅当 configure 真的编不过才加
├── third_party/ffmpeg/           # submodule，钉 tag
├── prebuilt/arm64-v8a/{include,lib}   # 构建产物，供 HAP 直接引
├── harmony/                      # DevEco 工程（含 entry/src/main/cpp）
│   └── entry/src/main/cpp/
│       ├── napi_init.cpp
│       ├── audio_pipeline.cpp    # 解封装 → 路由 → 解码 → 重采样 → 输出
│       ├── dec_ffmpeg.cpp        # 软解后端
│       ├── dec_ohcodec.cpp       # OH_AudioCodec 后端（M3）
│       └── CMakeLists.txt
└── test/assets/                  # 样本音频（体积大的别进 git）
```

分层设计（M1 就把接口定死，M3 只换实现）：

```
ArkTS
  └─ libaudio_decoder.so (NAPI)
      ├─ 输入：avformat（路径 / 自定义 AVIO 读 rawfile）
      ├─ 路由：pick_decoder(codec_id) → ohcodec | ffmpeg
      ├─ 解码：两条后端实现同一个接口 open/feed/drain/flush/close
      ├─ 转换：libswresample → 设备格式
      └─ 输出：OH_AudioRenderer + 环形缓冲
```

---

## 6. 坑清单（每条带触发条件与对策）

| # | 坑 | 触发条件 | 对策 |
| --- | --- | --- | --- |
| 6.1 | **SDK 只有 API 26**，编出的 `.so` 引用了高版本才有的符号 | 目标设备 API < 用到的符号引入版本（如 API 12 设备 + API 22 的 MIME 常量/API 20 的同步模式） | 编译用低版本 SDK（SDK Manager 里装 API 12/18）；高版本 API 用 `dlsym` 动态取 |
| 6.2 | **Opus 到底支不支持是个谜** | `CreateByMime("audio/opus")` 返回空 | 直接走软解，别赌；真机实测后写进文档 |
| 6.3 | **`OH_AudioCodec` 同步模式要 API 20** | 设备 API < 20 | 回调模式 + 自己环形缓冲，两条路都要写 |
| 6.4 | **没设 SONAME 的预编译 `.so`** | 用自己打包的 `libavcodec.so` 进 HAP | `llvm-readelf -d` 检查，缺 SONAME 就加 `-Wl,-soname,libavcodec.so.N` |
| 6.5 | **`-Wl,-Bsymbolic` 漏了** | 静态 `.a` 链进 `libentry.so` 后运行期符号解析到系统库的版本 | 加 `-Wl,-Bsymbolic`，这是 tpc 文档踩过后写的硬要求 |
| 6.6 | **LGPL 合规** | 静态 `.a` 链进 HAP 后想让用户"能替换库并重链" | 走 LGPL §6(a)：自己开源 + 提供 `libentry.so` 的目标文件；或改动态 `.so` 走 §6(b) |
| 6.7 | **rawfile 不能直接喂 avformat** | 想把 HAP 内资源当输入 | 自定义 `AVIOContext` 的 read 回调包 `OH_ResourceManager`，或先拷到 `cacheDir` |
| 6.8 | **pwsh 里不能跑构建** | 在 DSH 会话里 `make` | 换 MSYS2/DevEco 终端，或给会话完全权限 |
| 6.9 | **`avformat` 默认走网络栈** | 打开本地文件时无意触发 network 初始化 | `--disable-network`，`file` 协议单独开 |
| 6.10 | **模拟器是 x86_64** | 只编了 arm64 想用模拟器验证 | 加一份 `--arch=x86_64` 构建（sysroot 里已有 `x86_64-linux-ohos`） |

---

## 7. 待拍板（这 5 条定了才好开工）

1. **最低支持 API**：12（NEXT 5.0 首发）还是 18/20？直接决定能不能用同步模式、要不要装老 SDK。
2. **交付形态**：只给自己用的播放器 App，还是要做成给别人引用的 HAR/库？后者要额外设计接口稳定性与文档。
3. **ABI**：只 arm64-v8a，还是加 x86_64（模拟器验证）。
4. **许可证策略**：项目开源（静态 `.a` 可行）还是将来闭源（留动态 `.so` 的余地）。
5. **要不要视频**：如果将来要视频，上游的 `h264_ohcodec` 可以直接复用，构建时别把 `--disable-everything` 的名单卡太死。

---

## 附：本次侦察的证据索引

| 证据 | 位置 / 命令 |
| --- | --- |
| ffmpeg 版本与构建参数 | `<ffmpeg 发行版目录>\bin\ffmpeg.exe -version` |
| SDK 版本 | `sdk\default\sdk-pkg.json` → `apiVersion: 26` |
| 工具链可用性 | 交叉编译探针 `.recon/toolchain_probe.c` → AArch64 DYN，`NEEDED libc.so` |
| 平台 MIME 清单 | `native_avcodec_base.h` 中 `OH_AVCODEC_MIMETYPE_AUDIO_*` |
| NDK 库符号归属 | `llvm-nm -D libnative_media_acodec.so` → `OH_AudioCodec_*` |
| 上游 ohcodec | `gitee.com/mirrors/ffmpeg` configure 中 `--enable-ohcodec`、`h264_oh_decoder_deps` |
| 官方移植脚本 | `tpc_c_cplusplus/thirdparty/FFmpeg/HPKBUILD`、`docs/hap_integrate.md` |
| 系统适配层 | `openharmony/third_party_ffmpeg/ohos_config.sh`（sed 清单） |
| 网络可达性 | `.recon/net_check.py` 输出 |
| 本机权限事故 | `<仓库父目录>\_acl-recovery\acl-report-*.jsonl` |

`.recon/` 目录是本次侦察的探针脚本，保留作证据，可随时删。
