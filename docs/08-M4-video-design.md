# M4.1 视频渲染架构设计（OhVideoDecoder + XComponent 零拷贝上屏）

> 状态：设计稿（M4.0 阶段产出，M4.1 实现前评审）
> 范围：仅视频渲染链路；音频沿用现有 `AudioPlayer`，音画同步在 M4.2 接入。
> 前提：M4.0 的 `videoProbe()` 已确认设备 H.264/H.265 硬解能力与最大分辨率。

## 1. 目标与非目标

**目标**

- 硬解优先：H.264 / H.265 走系统 `OH_VideoDecoder`。
- 零拷贝上屏：解码输出直接进 `OHNativeWindow`（XComponent 的 surface），CPU 不碰 YUV 像素。
- 可回退：硬解不可用或 Configure 失败时，退到 FFmpeg 软解（M4.0 已加 `h264/hevc` 解码器）。
- 可观测：状态、队列深度、丢帧数、解码耗时都能从 NAPI 查。

**非目标（本阶段不做）**

- 视频编码 / 转码（M4.3）。
- HDR、杜比视界、多路视频。
- 软解路径的 GPU 上屏优化（先保证能出画面，零拷贝只承诺硬解路径）。

## 2. 总体结构（文字类图）

```text
┌──────────────────────────── ArkTS / UI ────────────────────────────┐
│ Index.ets                                                          │
│  ├─ XComponent(type=SURFACE, id="videoSurface")                    │
│  │    onLoad / onSurfaceCreated / onSurfaceChanged / onDestroyed    │
│  └─ Buttons: Play / Pause / Stop / Seek / videoProbe               │
└───────────────▲──────────────────────────────┬────────────────────┘
                │ NAPI (attachSurface / play / pause / seek / status)
                │                              │
┌───────────────┴──────────────────────────────▼────────────────────┐
│ C++ VideoPlayer（单实例，状态机）                                   │
│  ├─ FFmpegDemuxer        avformat_open_input / read_frame          │
│  │    ├─ video stream → VideoPacketQueue (AVPacket)                │
│  │    └─ audio stream → AudioPacketQueue（交给现有 AudioPlayer）    │
│  ├─ VideoDecoderRouter   硬解优先 / 软解兜底                        │
│  │    ├─ OhVideoDecoder   OH_VideoDecoder + OHNativeWindow         │
│  │    └─ SwVideoDecoder   FFmpeg h264/hevc → AVFrame               │
│  ├─ AvSyncClock          音频主时钟（M4.2）                         │
│  ├─ RenderSink           surface 模式：解码器直接写 OHNativeWindow  │
│  └─ Stats                队列深度 / 解码耗时 / 丢帧 / 错误码        │
└───────────────▲──────────────────────────────┬────────────────────┘
                │ OHNativeWindow*              │ OH_AVBuffer
┌───────────────┴──────────────────────────────▼────────────────────┐
│ OH_VideoDecoder（surface 模式）                                    │
│  ├─ SetSurface(OHNativeWindow*)                                    │
│  ├─ Configure(format: mime/width/height/pixfmt/csd)                │
│  ├─ Prepare / Start                                                │
│  ├─ OnNeedInputBuffer → 填 csd/packet → PushInputBuffer            │
│  └─ OnOutputBufferAvailable → RenderOutputData/FreeOutputBuffer    │
└────────────────────────────────────────────────────────────────────┘
```

关键点：`VideoPlayer` 不持有像素；它只负责把压缩包喂给解码器，把 surface 指针交给解码器。解码帧的搬运发生在 GPU/显示合成层，CPU 侧零拷贝。

## 3. 零拷贝上屏流程

### 3.1 初始化

1. ArkTS 创建 `XComponent(SURFACE)`，`onSurfaceCreated` 时通过 NAPI 把 `OH_NativeXComponent` 或 `OHNativeWindow*` 交给 C++。
2. C++ 调 `OH_NativeXComponent_GetNativeWindow` 拿到 `OHNativeWindow*`（surface 生命周期由 XComponent 管理，C++ 只借用，不释放）。
3. `videoProbe()` / `OH_AVCodec_GetCapability` 选解码器：优先 `video/avc`、`video/hevc` 的硬件 capability；没有则软解。

### 3.2 解码器配置（surface 模式）

```text
OH_VideoDecoder_CreateByMime(mime)
  → OH_VideoDecoder_SetSurface(codec, nativeWindow)     // 关键：零拷贝开关
  → OH_AVFormat:
       OH_MD_KEY_WIDTH / HEIGHT
       OH_MD_KEY_PIXEL_FORMAT（NV12/NV21，按 capability 选）
       OH_MD_KEY_FRAME_RATE
       csd-0: H.264 的 SPS+PPS / H.265 的 VPS+SPS+PPS（AnnexB，带起始码）
  → Configure → Prepare → Start
```

`csd-0` 由 FFmpeg 的 `extradata` 转换：mp4 里是 avcC/hvcC，需要拆成 AnnexB NAL（SPS/PPS 前补 `00 00 00 01`）。这一步是 M4.1 最容易翻车的地方，单独做单元级校验（比对 SPS/PPS 的 NAL 类型与长度）。

### 3.3 运行时数据流

```text
demux thread                input thread                  decoder/output callback
     │                           │                                   │
     │ av_read_frame             │                                   │
     ├─ video → VideoQueue ─────►│                                   │
     │                           │ OnNeedInputBuffer                 │
     │                           ├─ av_packet → OH_AVBuffer          │
     │                           ├─ SetBufferAttr(PTS/flags)         │
     │                           └─ PushInputBuffer ────────────────►│
     │                                                               │ 解码
     │                                                               │ surface 直接合成
     │                           OnOutputBufferAvailable ◄───────────┤
     │                           ├─ RenderOutputData（surface 模式）  │
     │                           └─ FreeOutputBuffer                  │
     │                                                               │
     └─ audio → AudioQueue → 现有 AudioPlayer（M4.2 用音频时钟驱视频）
```

**零拷贝成立的条件**：

- 必须 `SetSurface`，且不要走 `GetOutputBuffer` 拿 YUV 再 `memcpy`。
- 解码器输出格式与 surface 支持的格式一致（NV12/NV21；P010 需要 10bit surface，M4.1 不做）。
- 不要在回调里做 `AVFrame` 转换/缩放；缩放交给显示合成。

### 3.4 音画同步（M4.2 预留）

- 以音频播放位置为主时钟；视频帧 PTS 落后超过阈值（如 40ms）直接丢帧，超前则等待。
- 丢帧在 **FreeOutputBuffer 之前**判断，避免已经上屏又重复渲染。
- 暂停时停止喂输入、保留最后一帧；seek 时 `Flush` + 清队列 + 重置时钟。

## 4. 软解兜底路径

```text
videoProbe 无 capability / CreateByMime 失败 / Configure 失败
  → SwVideoDecoder（FFmpeg h264 / hevc）
  → AVFrame (YUV420P)
  → M4.1 先做 CPU 转换 + OH_NativeWindow 写缓冲（能出画面，非零拷贝）
  → M4.2 再评估 EGL/GLES 纹理上传，减少拷贝
```

路由规则沿用音频硬解的三级策略：

- **L0**：capability 不支持 → 直接软解，不尝试创建。
- **L1**：Create/Configure/Prepare 失败 → 记录错误码，软解。
- **L2**：运行期 `OnError` → 当前会话内回退一次；再失败则永久熔断该 MIME，避免死循环。

## 5. 生命周期与线程模型

| 对象 | 线程 | 生命周期 |
|---|---|---|
| `FFmpegDemuxer` | demux thread | open → read loop → close |
| `OhVideoDecoder` | input thread + callback | create → configure → start → stop → destroy |
| `OHNativeWindow` | UI thread 创建，C++ 借用 | XComponent surface 存活期 |
| `AudioPlayer` | 现有实现 | 不变 |
| `AvSyncClock` | 渲染/回调线程读 | 播放会话级 |

Surface 重建（折叠屏/旋转/后台恢复）时：

1. `onSurfaceDestroyed`：停喂输入，`OH_VideoDecoder_Stop`，但**不要 Destroy**（保留解码器配置）。
2. `onSurfaceCreated`：`SetSurface` 新窗口，重新 `Prepare/Start`。
3. 若尺寸变化，重建解码器（`Destroy` → `Create` → `Configure`）。

## 6. 错误处理与可观测性

- 所有 `OH_AVErrCode` 转成可读字符串进日志（`[vdec] err=AV_ERR_...`）。
- NAPI 暴露 `videoStatus()`：`{state, width, height, decoded, dropped, queue, lastError}`。
- 永久熔断标记按 MIME 记录，进程内有效；重启后重新探测。

## 7. M4.1 验收标准

1. 720p / 1080p H.264 测试素材都能硬解上屏，CPU 占用明显低于软解（对比数据）。
2. `videoProbe()` 在目标平板上返回 `hardware=true`、`supports1080p=true`，并记录最大分辨率。
3. 连续播放 10 分钟无泄漏（decoder 实例数稳定、surface 不重复创建）。
4. 硬解失败时自动切软解，画面能出但允许更高 CPU。
5. 旋转/后台恢复后能继续播放，不黑屏、不崩溃。

## 8. 风险清单

| 风险 | 影响 | 缓解 |
|---|---|---|
| avcC → AnnexB 转换错误 | 解码器报错/花屏 | 单独校验 csd，先用 720p 素材打通 |
| surface 尺寸与视频尺寸不一致 | 拉伸/黑边 | 按视频宽高比设置 XComponent，或渲染侧 letterbox |
| 回调里做重活 | 掉帧/ANR | 回调只做入队/释放，渲染决策放独立线程 |
| 解码器实例泄漏 | 第二次播放失败 | RAII + 状态机，Stop/Destroy 成对 |
| 设备不支持 HEVC | 黑屏 | videoProbe 预判 + 软解兜底 |
| 音频时钟未接入 | 音画不同步 | M4.2 前先做无音频视频验证，再接主时钟 |

## 9. 与 M4.0 探针的衔接

`videoProbe()` 返回的字段直接决定路由：

```json
{"codec":"h264","hardware":true,"maxWidth":4096,"maxHeight":4096,
 "maxFps":120,"creatable":true,"supports1080p":true,"supports4K":true}
```

- `capability=false` 或 `creatable=false` → 该 MIME 直接软解。
- `maxWidth/maxHeight` 小于素材尺寸 → 软解或降分辨率。
- `hardware=false` → 仍可尝试，但记录为“软件解码器”，不承诺零拷贝。
