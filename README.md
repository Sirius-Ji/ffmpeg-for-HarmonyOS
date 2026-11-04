# ffmpeg-for-HarmonyOS

面向 **OpenHarmony / HarmonyOS NEXT** 的 FFmpeg 音视频基础库（**基座**）。
目标不是做个播放器 App，而是把它做成能被别人直接集成的三方库，最终进入 OpenHarmony 供应链（TPC / [tpc_c_cplusplus](https://gitee.com/openharmony-sig/tpc_c_cplusplus)）。

- 上游：FFmpeg `n9.0`（commit `d32b387`），交叉编译到 `aarch64-linux-ohos` / `x86_64-linux-ohos`
- 产物：带 SONAME 的动态库 + 完整头文件，`64 KB` 级别的集成成本（4 个 IMPORTED 目标）
- 许可：**LGPL-2.1-or-later**（未启用 GPL / nonfree），见 [NOTICE.md](NOTICE.md)
- 状态：M0 交叉编译 ✅、M1 真机解码 ✅、M2 播放链路已实现（待真机听声验证）、M3 平台硬解路由 ⏳

> 仓库里的 `harmony/` 只是**验证载体**（一个只有 Play/Stop 的空白页），不是产品。库的价值在 `prebuilt/` 的产物和 `build/` 的构建脚本。

---

## 1. 能力边界

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| 容器解封装 | ✅ | `mov/mp4/m4a`、`matroska`、`ogg`、`flac`、`ape`、`dsf`、`wav`、`mp3`、`aac(ADTS)`、`tta`、`wv`、`caf`、`asf` |
| 音频软解 | ✅ | `aac` `alac` `ape` `ac3` `dca(DTS)` `flac` `opus` `wmav2`，统一输出 S16 交错 |
| 重采样 / 声道归一 | ✅ | `libswresample`，任意输入 → 指定采样率/声道（默认 48 kHz / 2ch） |
| 音频播放输出 | ✅ | `OHAudio`（`OH_AudioRenderer`，数据写入回调模式），demo 里是 48 kHz / 2ch / S16 |
| 视频硬解封装 | ✅（编译进） | `h264_oh` / `hevc_oh`，走 `--enable-ohcodec`；需要调用方提供 surface，demo 未接 |
| 音频硬解（平台解码器） | ⏳ M3 | `OH_AudioCodec` 同步模式路由（API 20+） |
| 视频软解 | ❌ | 未编入（`--disable-everything` 下只开了上面那些） |
| 网络协议 / HLS / DASH | ❌ | `--disable-network`，只留 `file` 协议 |
| 字幕 / 加密 / DRM | ❌ | 未编入 |
| 滤镜 | ❌ | `libavfilter` 编进来了但 **filter 未启用**（`--disable-everything` 的副作用），要用得改构建开关重编 |

**明确不做的**：UI、播放器状态机之外任何业务封装、命令行工具（OpenHarmony 也不允许 app fork+exec）。

---

## 2. 集成到现有工程（CMake）

假设你把 release 包解压到工程的 `third_party/ffmpeg-ohos/<abi>/`（`<abi>` 为 `arm64-v8a` 或 `x86_64`）：

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
```

**三条硬性约束**（踩过的坑，别重犯）：

1. 文件名必须带大版本号（`libavcodec.so.63`），改成 `libavcodec.so` 会加载失败——`SONAME` 是前者。
2. `libc++_shared.so` 由 hvigor 自动打包；如果你把 `.so` 放在别处，ABI 目录要自己保证一致。
3. 库是 **16 KB 页对齐**（`PT_LOAD align = 0x4000`），别用老工具链重链成 4 KB。

C++ 侧的参考实现（解封装 → 软解 → 重采样 → OHAudio 播放）在
[`harmony/entry/src/main/cpp/decoder.cpp`](harmony/entry/src/main/cpp/decoder.cpp) 和
[`audio_player.cpp`](harmony/entry/src/main/cpp/audio_player.cpp)，可以直接抄。

---

## 3. 从源码构建

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
├── include/lib{avcodec,avformat,avutil,swresample,avfilter}/   # 头文件
└── lib/libavcodec.so.63  libavformat.so.63  libavutil.so.61  libswresample.so.7  libavfilter.so.12
```

---

## 4. 许可证与合规

- FFmpeg 以 **LGPL-2.1-or-later** 分发；本项目编译时未开 `--enable-gpl` / `--enable-nonfree` / `--enable-version3`，
  产物里没有 x264/x265 等 GPL 组件。`configure` 输出可自证：`License: LGPL version 2.1 or later`。
- 产物是**动态库**，应用动态链接即可满足 LGPL-2.1 §6(b) 的"共享库机制"路径；闭源商用不需要公开自己的代码。
- 分发二进制时请附上 [LICENSES/LGPL-2.1.txt](LICENSES/LGPL-2.1.txt) 与 [NOTICE.md](NOTICE.md)，并提供 FFmpeg 源码获取方式。
- **专利与许可分开算**：AAC / MP3 等编解码专利池授权与 LGPL 无关，商用请自行评估。

---

## 5. 版本与路线图

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| M0 | FFmpeg n9.0 交叉编译到 ohos，双 ABI，16 KB 页对齐，LGPL 配置 | ✅ |
| M1 | NAPI 打通 open → read → decode（整文件解到 WAV，与桌面逐字节对拍） | ✅ 真机验证 |
| M2 | 流式解码 + 环形缓冲 + `OH_AudioRenderer` 出声（Play/Stop） | ✅ 代码完成 / ⏳ 真机听声 |
| M3 | `OH_AudioCodec` 同步模式做主力解码，FFmpeg 退为兜底 | ⏳ |
| M4 | 按 TPC 规范补齐 `HPKBUILD` / `README.OpenSource` / CI，提交 tpc_c_cplusplus | ⏳ |

---

## 6. 目录结构

```
build/          # 构建脚本：交叉编译、产物校验、符号审计、Release 打包
docs/           # 侦察报告 / 构建实录 / M1 验证记录
prebuilt/       # 交叉编译产物（gitignore，走 Release 分发）
harmony/        # DevEco 验证工程（只有 Play/Stop 和一个自检按钮）
test/           # 桌面参考 PCM 与对拍脚本
LICENSES/       # 第三方许可证全文
```

---

## 7. 已知限制

- 只支持 **file 协议**；网络流要自己喂自定义 `AVIOContext`。
- 播放器是**单实例**、无 seek / 无后台播放（M2 范围），暂停靠 `OH_AudioRenderer_Pause`。
- 44.1 kHz 源会重采样到 48 kHz（OHAudio 输出固定），需要位真还原请用 M1 的整文件解码接口。
- `libavfilter` 未启用 filter，暂时别指望滤镜链。
