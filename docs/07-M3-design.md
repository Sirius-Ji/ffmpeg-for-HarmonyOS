# M3 架构设计：OH_AudioCodec 硬解路由

> 状态：设计稿，等确认后开工。
> 目标：AAC / MP3 / FLAC 走系统硬解，失败或无能力时无缝回退 FFmpeg 软解；对上层 `ReadPcm` 接口零改动。

## 1. 目标与非目标

**目标**

- 在 `AudioDecoder` 内部增加一条「系统解码器」通路，优先用 `OH_AudioCodec`（API 20 同步模式）解 AAC/MP3/FLAC；
- 任何一步失败（创建、配置、启动、运行中报错、超时无输出）都回退到现有 FFmpeg 软解，**上层无感知**；
- 用现有 `[player] perf cpu=` 日志量化收益，AAC 长播场景目标 CPU 下降 ≥30%；
- M1 的 `DecodeAllToWav` 保持 FFmpeg 单路径（位对位自检不能变）。

**非目标（M3 明确不做）**

- 视频硬解（已有 `h264_oh`/`hevc_oh` 封装，另一个里程碑）；
- Opus / Vorbis 硬解——FFmpeg 解封装后拿到的是裸包，丢了 Ogg 页帧，无法直接喂 `OH_AudioCodec`，M3 保持软解；
- 硬解 seek/拖动（当前播放器本来就无 seek）。

## 2. 插入点：只有一个出口

现有链路：

```
av_read_frame → avcodec_send_packet/receive_frame → AVFrame → swr → ReadPcm → 播放器 ring
```

`ReadPcm(out, maxFrames, outRate, outChannels)` 是唯一出口，播放器、转码器都只认它。
所以硬解路由**只替换「AVFrame 产生」这一段**，swr 及以下完全不动：

```
                         ┌─ FFmpegDecoder（现有 avcodec 路径）─┐
av_read_frame → 路由 ────┤                                   ├→ PcmChunk 队列 → swr → ReadPcm
                         └─ OhCodecDecoder（新增同步模式）─────┘
```

## 3. 组件

```
decoder.h/.cpp
├── AudioDecoder            // 保留：解封装、探测、ReadPcm、swr、Eof
│   ├── Demuxer             // 现有 avformat 部分，不动
│   ├── PcmChunkQueue       // 新增：解码后端产出的 PCM 小块（含格式/率/声道）
│   └── DecodeBackend*      // 新增：抽象后端
│        ├── FFmpegBackend  // 现有 send_packet/receive_frame，产出 AVFrame→PcmChunk
│        └── OhCodecBackend // 新增：OH_AudioCodec 同步模式
└── ohcodec_backend.h/.cpp  // 只依赖 multimedia/player_framework 头
```

`PcmChunk`：

```cpp
struct PcmChunk {
    std::vector<uint8_t> data;     // 交错 PCM
    AVSampleFormat fmt;            // S16 / FLT（按后端实际输出）
    int sampleRate;
    int channels;
    int64_t ptsSamples;            // 累计输出采样数，用于回退时 seek 定位
};
```

后端接口（最小面）：

```cpp
class DecodeBackend {
public:
    virtual int Open(const AVCodecParameters *par, int streamIndex) = 0;
    virtual int Decode(const AVPacket *pkt, std::vector<PcmChunk> *out) = 0; // pkt=nullptr 表示 flush
    virtual void Close() = 0;
};
```

`AudioDecoder::StreamFillPending()` 改为：先从 `pcmQueue_` 取块喂 swr；队列空则驱动后端解一包。
FFmpeg 路径的产出与现在完全一致，保证 M1 哈希不变。

## 4. 硬解后端：同步模式时序

SDK 实测到的 API（本机 `sdk/default/openharmony/native`，API 26）：

| 用途 | API | 起始版本 |
| --- | --- | --- |
| 创建 | `OH_AudioCodec_CreateByMime(mime, false)` | 11 |
| 配置 | `OH_AudioCodec_Configure(codec, OH_AVFormat*)` → `Prepare` → `Start` | 11 |
| 同步模式开关 | `OH_MD_KEY_ENABLE_SYNC_MODE` = true | **20** |
| 输入 | `QueryInputBuffer(us)` → `GetInputBuffer(idx)` → `SetBufferAttr` → `PushInputBuffer(idx)` | 20 |
| 输出 | `QueryOutputBuffer(us)` → `GetOutputBuffer(idx)` → `GetBufferAttr` → 拷贝 → `FreeOutputBuffer(idx)` | 20 |
| 配置键 | `OH_MD_KEY_AUD_SAMPLE_RATE` / `OH_MD_KEY_AUD_CHANNEL_COUNT` / `OH_MD_KEY_AUDIO_SAMPLE_FORMAT` / `OH_MD_KEY_CODEC_CONFIG` | 9/10 |
| 结束标记 | `BUFFER_FLAGS_EOS` | 9 |

单次 `ReadPcm` 内部的拉取循环（无新增线程）：

```
1. 若 pcmQueue 有数据 → 直接返回（走 swr）
2. 尝试 QueryInputBuffer(0)：
     有 index → 取 Demuxer 下一包 → ES 适配 → GetInputBuffer 写数据 + SetBufferAttr(size,pts,flags)
              → PushInputBuffer → 继续 3
     没有   → 直接 3（输入背压）
3. QueryOutputBuffer(5ms)：
     有 index → GetOutputBuffer → GetBufferAttr → 拷贝 PCM → FreeOutputBuffer → 入队 → 返回
     超时   → 回 2（最多 N 次，N 以内空转，超过阈值算异常）
4. Demuxer EOF → 送 EOS 空输入 → 继续收输出直到 EOS 标记
```

`OH_AVBuffer` 侧只用 `GetAddr` / `GetCapacity` / `SetBufferAttr` / `GetBufferAttr`。
输出 PCM 格式：配置时请求 `SAMPLE_S16LE`；若 `Configure` 不接受则改请求 `SAMPLE_F32LE` 再试；
两者都失败 → 回退。swr 的输入格式按实际请求的那个填。

## 5. 路由判定（三级探测，全部失败才回退）

```
路由(mime, codecpar):
  L0 能力：
     OH_AVCodec_GetCapability(mime, false) != null
     OH_AVCapability_GetAudioSupportedSampleRates(...) 含源采样率或 48k
     channels ∈ {1,2}
  L1 创建/配置/启动：
     CreateByMime → Configure → Prepare → Start，任一步非 0 → 销毁回退
  L2 运行：
     首次输出超时 >2s、Push/Query 返回错误、连续 3 次 EOS 前无输出 → 运行期回退
```

运行期回退流程：关闭 OH codec → 用 `ptsSamples` 换算出毫秒 → `av_seek_frame` 到该点前 0.5s →
重置 swr → 用 FFmpeg 继续。seek 失败就从头重解（宁可重播一点，不能断流）。

mime 映射：

| 容器内 codec | OH mime | 是否进 M3 |
| --- | --- | --- |
| AAC | `OH_AVCODEC_MIMETYPE_AUDIO_AAC` | ✅ |
| MP3 | `OH_AVCODEC_MIMETYPE_AUDIO_MPEG` | ✅ |
| FLAC | `OH_AVCODEC_MIMETYPE_AUDIO_FLAC` | ✅（带 spike 验证） |
| Opus / Vorbis | AUDIO_OPUS / AUDIO_VORBIS | ❌ Ogg 页帧丢失 |
| ALAC/APE/AC3/DTS/WMA | 对应 mime | 后续，按设备能力再开 |

## 6. 码流适配器（最容易翻车的地方）

`OH_AudioCodec` 吃的是**裸码流**，不是容器包，每个格式要一个适配器：

- **AAC**：FFmpeg 从 m4a 拿到的包是裸 AAC（无 ADTS 头），需要现场补 7 字节 ADTS：
  profile=`codecpar->profile+1`、采样率索引表、channel config、帧长=7+packet size。
  若包首已经是 `0xFFF` 同步字（adts 源）则不再包。extradata（ASC）同时走 `OH_MD_KEY_CODEC_CONFIG`。
- **MP3**：包就是 MP3 帧，原样推；首包含 ID3/Xing 也原样推，由系统解码器忽略。
- **FLAC**：`codecpar->extradata`（STREAMINFO）放 `OH_MD_KEY_CODEC_CONFIG`，包作为 FLAC 帧推。
  **风险点**：系统是否要求完整 `fLaC` 头+STREAMINFO 作为码流的一部分，需要在 M3.0 spike 里验证。
- 适配器只做「容器包 → 裸流」的转换，不碰 PCM。

## 7. 可观测性与回退测试

- 新增一行路由日志：
  `I FFM1 : [decoder] route=ohcodec mime=audio/mp4a-latm src=44100/2 out=s16/44100`
  `I FFM1 : [decoder] route=ffmpeg reason=no-capability mime=audio/mpeg`
- 运行期回退日志带原因码（create/configure/start/timeout/push/query）。
- 测试开关：`decoderSetHardware(bool)` NAPI（默认 true），强制 false 用于 A/B CPU 对比，
  也是「软解路径没坏」的回归手段。
- M3.0 先只做 `hardwareProbe()`：对 AAC/MPEG/FLAC 逐个打「能力 + 支持采样率列表」，
  用真机日志决定 L0 白名单，不写死设备能力。

## 8. 里程碑

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| M3.0 | `hardwareProbe` NAPI + 日志；真机确认 mime/采样率/输入格式 | 三种 mime 的能力与支持率有明确日志 |
| M3.1 | `OhCodecBackend` + `PcmChunkQueue` + AAC 同步模式；路由与 L0/L1 回退 | AAC 44.1k 播放出声，`route=ohcodec`，M1 哈希不变 |
| M3.2 | MP3 / FLAC 适配器；L2 运行期回退 | 三种格式都能 `route=ohcodec`；强制软解仍正常 |
| M3.3 | CPU 对比（同一文件 3 分钟，`[player] perf cpu` 均值）+ 文档 | AAC/MP3 CPU 下降 ≥30%，无 underrun 回归 |

## 9. 任务二 / 任务三的落点（确认后一起排期）

- **LAME 依赖警告**：`harmony/entry/src/main/cpp/CMakeLists.txt` 顶部加注释块；
  README「打包时把同名 .so 放进 HAP」那段加粗红字：`libmp3lame.so.0` 必须与 4 个 FFmpeg 库同目录，
  否则 `libavcodec.so` 的 DT_NEEDED 解析失败，整个 NAPI 初始化直接崩。
- **Breaking Change**：新增 `CHANGELOG.md`，v0.1.1 条目里单列「移除 libavfilter 头文件与 .so」，
  并给出迁移方式（不要 include `libavfilter/*.h`；本库不提供滤镜）。
- **M4A 容器**：把 `transcoder.cpp` 的 aac profile 从 `mp4` 改回 `ipod`，保留 ©lyr/iTunes 元数据兼容性；
  上一轮 ipod 失败是 HAP 打了旧 `libavformat`，不是 muxer 不存在，现已修复。
- **LAME 采样率上限**：转码目标固定 48k，本身不会超；但补 `supported_samplerates` 兜底——
  用 `avcodec_get_supported_config(..., AV_CODEC_CONFIG_SAMPLE_RATE, ...)` 取 LAME 支持列表，
  目标率取 `min(48000, max(supported))`，并对 >48k 源打一行降级日志。
- **NAPI 扩展**：`transcodeTo` 支持 `flac` / `wav`；新增 `transcodeByPath(src, dst, format)`，
  native 侧 `open()` 两个路径转 fd 复用现有管线（比 ArkTS 取 fd 少一层）。
