# ffmpeg-for-HarmonyOS

> **v0.1.3 Alpha · 早期测试版**
>
> 解码 / 播放链路已真机验证（192 kHz/2ch FLAC 无卡顿、无爆音），**v0.1.1 起加入基础转码能力**
> （音频编码 `aac/alac/flac/mp3(libmp3lame)/opus/pcm_s16le/pcm_s24le`，
> 封装 `mp4/mov/ipod/mp3/flac/wav/matroska/adts/ogg/opus`）。
> **v0.1.3 起 M3.0/M3.1 硬解已合入**，并随 Release 提供 CMake Config。
> **API 仍可能变更**，欢迎试用、集成、提 Issue 和 PR。

## 🚀 v0.1.3 更新亮点

- **新增 E-AC3 解码**：`eac3` 原生解码器编入白名单，解决部分全景声 M4A（E-AC3 音轨）无法解码的问题。
- **CMake Config 随包发布**：Release 包内含 `ffmpeg-ohos-config.cmake`，`find_package(ffmpeg_ohos REQUIRED)`
  自动带入头文件、5 个 `.so`、LAME 依赖和 `-rpath-link`。
- **硬解稳定性**：新增进程内按 mime 的硬件黑名单（某 mime 触发 L2 回退后本进程不再重试）；
  ADTS profile 改为查表映射，修正 HE-AAC / HEv2 的“创建成功但无输出”。
- **文档口径修正**：`libmp3lame.so.0` 明确为**强制运行时依赖**；批量转码默认走软解。

## 🚀 v0.1.1 更新亮点（历史）

- **全新转码能力**：打通底层转码链路，支持输出 **AAC**、**MP3** 和 **Opus** 三种主流音频格式。
- **引入 LAME 引擎**：为了解决 FFmpeg 无原生 MP3 编码器的问题，已成功交叉编译并集成 `libmp3lame.so.0`。
- **Demo 交互升级**：应用内新增三个转码测试按钮，支持一键“解码→重采样→编码→封装”，并在转码后自动播放，真机实测三种格式全部完美运行。
- **细节修复**：补全 `mp3`、`pcm_s24le` 等解码器白名单；彻底移除冗余的 `libavfilter` 以精简体积。

可开箱即用的面向 **OpenHarmony / HarmonyOS NEXT** 的纯血鸿蒙 FFmpeg 音视频基础库（**基座**）。
目标不是toC的播放器 App，而是面向鸿蒙开发者的能被直接集成的三方库，最终进入 OpenHarmony 供应链（TPC / [tpc_c_cplusplus](https://gitee.com/openharmony-sig/tpc_c_cplusplus)）。

- 上游：FFmpeg `n9.0`（commit `d32b387`），交叉编译到 `aarch64-linux-ohos` / `x86_64-linux-ohos`
- 产物：带 SONAME 的动态库 + 完整头文件，**5 个 `.so`**（4 个 FFmpeg + `libmp3lame`），
  CMake 集成成本 `64 KB` 级别（4 个 IMPORTED 目标 + `libmp3lame` 作为运行时依赖）
- 许可：**LGPL-2.1-or-later**（未启用 GPL / nonfree），见 [NOTICE.md](NOTICE.md)
- 状态：M0 交叉编译 ✅、M1 真机解码 ✅、M2 播放链路真机验证 ✅、v0.1.1 转码能力 ✅、**M3.0/M3.1 平台硬解已合入** ✅

> 仓库里的 `harmony/` 只是**验证载体**（一个只有 Play/Stop 的空白页），不是产品。库的价值在 `prebuilt/` 的产物和 `build/` 的构建脚本。

---

## 1. 能力边界

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| 容器解封装 | ✅ | `mov/mp4/m4a`、`matroska`、`ogg`、`flac`、`ape`、`dsf`、`wav`、`mp3`、`aac(ADTS)`、`tta`、`wv`、`caf`、`asf` |
| 音频软解 | ✅ | `aac` `alac` `ape` `ac3` `eac3` `dca(DTS)` `flac` `mp3` `opus` `pcm_s16le` `wmav2`，统一输出 S16 交错 |
| 音频编码 | ✅ | `aac` / `alac` / `flac` / `mp3`（libmp3lame）/ `opus` / `pcm_s16le` / `pcm_s24le` |
| 容器封装 | ✅ | `mp4` / `mov` / `ipod`（M4A 兼容性最好） / `mp3` / `flac` / `wav` / `matroska` / `adts` / `ogg` / `opus` |
| 重采样 / 声道归一 | ✅ | `libswresample`，任意输入 → 指定采样率/声道（默认 48 kHz / 2ch） |
| 音频播放输出 | ✅ | `OHAudio`（数据写入回调）；demo 支持系统文件选择器选歌、fd 直读、48 kHz / 2ch / S16 输出，含解码耗时 / CPU / RSS 日志 |
| 视频硬解封装 | ✅（编译进） | `h264_oh` / `hevc_oh`，走 `--enable-ohcodec`；需要调用方提供 surface，demo 未接 |
| 音频硬解（平台解码器） | ✅ M3.0/M3.1 | `OH_AudioCodec` 同步模式：AAC / MP3 / FLAC；L0 能力过滤 + L1/L2 回退 + 熔断/黑名单 |
| 视频软解 / 视频编码 | ❌ | 未编入；本库是**音频**基座，不碰视频编解码 |
| 网络协议 / HLS / DASH | ❌ | `--disable-network`，只留 `file` 协议 |
| 字幕 / 加密 / DRM | ❌ | 未编入 |

**定位**：音频**解码播放基座 + 基础转码能力**——能解、能放、能转（转封装、音频转码），
配合鸿蒙原生应用做本地播放、格式转换、提取/重编码音轨。**不做**视频编码、不做播放器业务。

**明确不做的：UI、播放器状态机之外任何业务封装、命令行工具**（OpenHarmony 也不允许 app fork+exec）。

---

## 2. 集成到现有工程（CMake）

从 Release 下载对应 ABI 的包，解压到工程的 `third_party/ffmpeg-ohos/<abi>/`：

| Release 包 | 解压到 |
| --- | --- |
| `ffmpeg-n9.0-ohos-arm64-v8a.zip` | `third_party/ffmpeg-ohos/arm64-v8a/` |
| `ffmpeg-n9.0-ohos-x86_64.zip` | `third_party/ffmpeg-ohos/x86_64/` |

解压后每个目录里是 `include/` 和 `lib/`（**5 个带 SONAME 的 `.so`**：4 个 FFmpeg + `libmp3lame`），
以及 `lib/cmake/ffmpeg_ohos/ffmpeg-ohos-config.cmake`。

**方式 A（v0.1.3+ 推荐）**：直接用随包发布的 CMake Config：

```cmake
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-ohos/${OHOS_ARCH})
find_package(ffmpeg_ohos REQUIRED)
target_link_libraries(entry PRIVATE ffmpeg_ohos::ffmpeg)
```

include 路径、5 个 `.so`（含 `libmp3lame.so.0`）和 `-rpath-link` 都由 config 自动处理。

**方式 B（手动）**：

```cmake
set(FF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-ohos/${OHOS_ARCH})

# 四个 IMPORTED 目标。注意 IMPORTED_LOCATION 必须写带大版本号的文件名，
# 这样 DT_NEEDED 才是 libavcodec.so.63，和运行时目录里的文件名对得上。
add_library(ff_avcodec SHARED IMPORTED)
set_target_properties(ff_avcodec PROPERTIES
    IMPORTED_LOCATION ${FF_DIR}/lib/libavcodec.so.63
    INTERFACE_INCLUDE_DIRECTORIES ${FF_DIR}/include)

add_library(ff_avformat SHARED IMPORTED)
set_target_properties(ff_avformat PROPERTIES
    IMPORTED_LOCATION ${FF_DIR}/lib/libavformat.so.63
    INTERFACE_INCLUDE_DIRECTORIES ${FF_DIR}/include)

add_library(ff_avutil SHARED IMPORTED)
set_target_properties(ff_avutil PROPERTIES
    IMPORTED_LOCATION ${FF_DIR}/lib/libavutil.so.61
    INTERFACE_INCLUDE_DIRECTORIES ${FF_DIR}/include)

add_library(ff_swresample SHARED IMPORTED)
set_target_properties(ff_swresample PROPERTIES
    IMPORTED_LOCATION ${FF_DIR}/lib/libswresample.so.7
    INTERFACE_INCLUDE_DIRECTORIES ${FF_DIR}/include)

target_link_libraries(entry PRIVATE
    ff_avformat ff_avcodec ff_avutil ff_swresample)
target_link_options(entry PRIVATE "-Wl,-rpath-link,${FF_DIR}/lib")
```

打包时把同名 `.so` 放进 HAP 的 `libs/<abi>/`（`build-profile.json5` 的 `abiFilters` 记得列两个 ABI）：

```
entry/libs/arm64-v8a/libavcodec.so.63
entry/libs/arm64-v8a/libavformat.so.63
entry/libs/arm64-v8a/libavutil.so.61
entry/libs/arm64-v8a/libswresample.so.7
entry/libs/arm64-v8a/libmp3lame.so.0      # 强制依赖：缺了它整个 NAPI 模块都起不来
```

> ⚠️ **`libmp3lame.so.0` 是硬依赖，不是可选项。**
> `libavcodec.so.63` 的 `DT_NEEDED` 里写着它，只要漏掉这一个文件，`libavcodec` 就加载失败，
> 整个 NAPI 模块初始化会直接崩（不是"MP3 转码不可用"，是**整个库都用不了**）。
> 关掉 MP3 编码重新编译 FFmpeg 才能不带它；否则必须和 4 个 FFmpeg 库放在同一目录。
> 用 `build/install-into-hap.sh` 同步可以避免手动漏拷。

**三条硬性约束**（踩过的坑，别重犯）：

1. 文件名必须带大版本号（`libavcodec.so.63`），改成 `libavcodec.so` 会加载失败——`SONAME` 是前者。
2. `libc++_shared.so` 由 hvigor 自动打包；如果你把 `.so` 放在别处，ABI 目录要自己保证一致。
3. 库是 **16 KB 页对齐**（`PT_LOAD align = 0x4000`），别用老工具链重链成 4 KB。

> **Breaking Change（v0.1.1 起）**：`libavfilter` 已彻底移除（库、头文件、`include/libavfilter/` 都没有了）。
> 旧代码里的 `#include <libavfilter/avfilter.h>` 和 `ff_avfilter` 目标会直接编译失败，
> 完整迁移说明见 [CHANGELOG.md](CHANGELOG.md)。

C++ 侧的参考实现（解封装 → 软解 → 重采样 → OHAudio 播放）在
[`harmony/entry/src/main/cpp/decoder.cpp`](harmony/entry/src/main/cpp/decoder.cpp) 和
[`audio_player.cpp`](harmony/entry/src/main/cpp/audio_player.cpp)，可以直接抄。

转码侧在 [`transcoder.cpp`](harmony/entry/src/main/cpp/transcoder.cpp)：解封装 → 软解 →
重采样到 48 kHz/2ch → **AAC 编码** → **mp4（m4a）封装**，全程走 fd，不落临时文件。

> **批量转码默认走软解**：NAPI 转码接口不会创建 `OH_AudioCodec` 实例。
> 系统硬解实例有上限，且转码瓶颈在编码，硬解只会增加不稳定性；需要硬解请在播放链路使用。

```ts
const inFile = fs.openSync(src, fs.OpenMode.READ_ONLY);
const outFile = fs.openSync(dst, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
const res = native.transcodeToAac(inFile.fd, outFile.fd);  // { ok, data }
fs.closeSync(inFile);
fs.closeSync(outFile);
```

demo 里有五个转码按钮（**AAC / MP3 / Opus / FLAC / WAV**）和一个**路径 API** 按钮，
会把内置 `play.m4a` 转成 `filesDir/transcoded.{m4a,mp3,opus,flac,wav}` 并直接播放结果；
路径按钮走 `transcodeByPath()`，由 native 层直接打开两个文件，不经过 ArkTS 的 fd。

> LAME 交叉编译、三种编码器帧长差异、`avcodec_get_supported_config` 这些坑，
> 完整记录在 [docs/06-transcode.md](docs/06-transcode.md)。

---

## 3. 参考播放器（demo 实现，不属于库本体）

`harmony/` 里的播放器是**参考实现**，用来证明这套库能出声，不是产品接口：

```
系统文件选择器 (DocumentViewPicker)
   └─ URI → fs.openSync(uri) → fd
        └─ playerOpenFd(fd)  // native 内部 dup，ArkTS 立刻 close 自己的 fd
             └─ 解码线程 + 2 秒环形缓冲(384KB) + OHAudio 写回调
                  ├─ 预填 200ms 才 Start 渲染器（消掉启动欠载）
                  ├─ EOF 且缓冲空 → 回调返回 0，位置不再推进
                  └─ 每 2 秒打一行 [player] perf
```

性能日志长这样（HiLog，tag `FFM1`；下面数字是示例，实测值随设备和文件变化）：

```
I FFM1 : [player] perf decode_avg=0.003ms/frame calls=312 cpu=18% rss=48213KB ring=180KB written=12288KB
```

> 48 kHz 输出的实时预算是 **20.8 µs/帧**（1/48000 秒）。0.003 ms ≈ 3 µs，
> 相当于只吃掉约 15% 的单核实时预算——192 kHz 源经 4:1 重采样之后，性能冗余依然很大。

`playerStatus()` 返回的 JSON 里可直接读这些量：

| 字段 | 含义 |
| --- | --- |
| `positionMs` / `audioMs` | 播放时钟（`GetFramesWritten`） / 真实音频推进（不含补的静音） |
| `underflow` / `gaps` | 系统欠载计数 / 我们补静音的次数 |
| `decodeAvgUs` / `decodeCalls` | 平均每帧解码耗时（微秒） / 解码调用次数 |
| `cpuPercent` / `rssKb` | 进程 CPU 占比 / 常驻内存 |
| `pcmHash` / `pcmPeak` | 自检哈希与峰值（仅内置样例） |

---

## 4. 从源码构建

前置：Windows + DevEco Studio（自带 NDK/clang/CMake）+ MSYS2（`make`、`bash`）。

```bash
# 1) 拉 FFmpeg 源码（n9.0）
git -c http.sslBackend=openssl clone --depth 1 --branch n9.0 \
    https://gitee.com/mirrors/ffmpeg.git third_party/ffmpeg

# 2) 交叉编译两套 ABI（配置见 build/build-ffmpeg.sh）
bash build/build-ffmpeg.sh all

# 3) 校验产物（ABI / SONAME / 许可开关）
bash build/verify-artifacts.sh

# 4) 生成 Release 资源包
python build/make-release.py
# → dist/release/ffmpeg-n9.0-ohos-{arm64-v8a,x86_64}.zip + SHA256SUMS.txt
```

产物布局：

```
prebuilt/<abi>/
├── include/lib{avcodec,avformat,avutil,swresample}/ + include/lame/   # 头文件
└── lib/libavcodec.so.63  libavformat.so.63  libavutil.so.61  libswresample.so.7  libmp3lame.so.0
```

---

## 5. 许可证与合规

- FFmpeg 以 **LGPL-2.1-or-later** 分发；本项目编译时未开 `--enable-gpl` / `--enable-nonfree` / `--enable-version3`，
  产物里没有 x264/x265 等 GPL 组件。`configure` 输出可自证：`License: LGPL version 2.1 or later`。
- 产物是**动态库**，应用动态链接即可满足 LGPL-2.1 §6(b) 的"共享库机制"路径；闭源商用不需要公开自己的代码。
- 分发二进制时请附上 [LICENSES/LGPL-2.1.txt](LICENSES/LGPL-2.1.txt) 与 [NOTICE.md](NOTICE.md)，并提供 FFmpeg 源码获取方式。
- **专利与许可分开算**：AAC / MP3 / Opus 等编解码专利池授权与 LGPL 无关，商用请自行评估。
- `libmp3lame`（LAME 3.100）同样是 **LGPL-2.1**，以动态库形式随包发布，源码获取方式见 [NOTICE.md](NOTICE.md)。

---

## 6. 版本与路线图

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| M0 | FFmpeg n9.0 交叉编译到 ohos，双 ABI，16 KB 页对齐，LGPL 配置 | ✅ |
| M1 | NAPI 打通 open → read → decode（整文件解到 WAV，与桌面逐字节对拍） | ✅ 真机验证 |
| M2 | 流式解码 + 环形缓冲 + `OH_AudioRenderer` 出声（Play/Stop） | ✅ 真机验证 |
| M3 | `OH_AudioCodec` 同步模式做主力解码，FFmpeg 退为兜底 | ⏳ |
| M4 | 按 TPC 规范补齐 `HPKBUILD` / `README.OpenSource` / CI，提交 tpc_c_cplusplus | ⏳ |

---

## 7. 目录结构

```
build/          # 构建脚本：交叉编译、产物校验、符号审计、Release 打包
docs/           # 侦察报告 / 构建实录 / M1 验证记录
prebuilt/       # 交叉编译产物（gitignore，走 Release 分发）
harmony/        # DevEco 验证工程（只有 Play/Stop 和一个自检按钮）
test/           # 桌面参考 PCM 与对拍脚本
LICENSES/       # 第三方许可证全文
```

---

## 8. 已知限制

- 只支持 **file / fd 输入**；网络流要自己喂自定义 `AVIOContext`。
- 播放器是**单实例**、**无 seek**（大文件只能从头播）、无后台播放；暂停用 `OH_AudioRenderer_Pause`。
- 44.1 kHz 源会重采样到 48 kHz（OHAudio 输出固定），需要位真还原请用 M1 的整文件解码接口。
- **MP3 编码走 libmp3lame 3.100**（LGPL-2.1，动态库已随包发布），不是 FFmpeg 原生实现。
- **Opus 编码是 FFmpeg 原生实验性实现**：调用 `avcodec_open2` 前必须设
  `avctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL`，且只支持 48 kHz / 2ch。
  帧长对齐到 960（20 ms）；我们的转码器按 `enc->frame_size` 切片，不写死。
- 视频只编了硬解封装（`h264_oh` / `hevc_oh`），**没有视频编码、也没有视频软解**。
- **外部 URI / 云盘 I/O**：受限于系统的底层 I/O 机制，直接读取外部云盘或未下载完的虚拟文件 URI 时，
  若发生内核级阻塞，底层会在 **1.5 秒超时后 detach 该解码线程**（泄漏一个线程，App 不会 ANR）。
  **建议集成方在应用层（ArkTS）先把网络文件下载到本地沙箱，再把本地路径传给基座解码/转码**
  ——demo 的"选歌播放"现在就是这么做的（带复制进度条）。
- 大文件建议先拷贝到应用沙箱再 `playerOpen(path)`；`playerOpenFd` 仍然可用，但只适合**本地真实文件**的 fd。
