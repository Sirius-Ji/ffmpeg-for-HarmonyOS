# 海思 OMX H.264 B 帧丢帧 Bug Report（M4.2.3/M4.2.4）

> 面向 OpenHarmony / 华为官方提交的复现报告。
> 结论先行：同一段合法 H.264 Main B 帧码流，桌面 FFmpeg 输出 721 帧，
> 设备 `OH_VideoDecoder` 在 **Surface 同步、Surface 异步、Buffer 输出** 三种模式下都只输出 719 帧。
> 丢失发生在解码器核心，不是 Surface 绑定或渲染路径。

## 1. 环境

| 项 | 值 |
|---|---|
| 设备 | 华为平板，海思 SoC（`OMX.hisi.video.decoder.avc`） |
| 系统 | HarmonyOS NEXT（API 26） |
| 解码器 | `OH_VideoDecoder`（native_media_vdec） |
| 应用 | `com.example.ffmpegforharmony`（debug 签名） |

## 2. 测试素材

- 来源：Big Buck Bunny 原始 H.264 码流，35–65 秒 **stream copy**（不重编码）
- 容器：mp4；视频 853×480、24fps、H.264 **Main**、`has_b_frames=1`
- 音频：AAC 5.1（播放侧下混立体声）
- `ffprobe -count_frames`：**nb_read_frames = 721**，包数 721，PTS 唯一、无缺口
- 桌面 FFmpeg 解码：721 帧，全部正常；缺失帧的 framemd5 与相邻帧不同（非重复/填充）

## 3. 现象

应用侧对输入包与输出帧做 PTS 对账：

```
[vplay] audit packets=721 outputs=719 missing=3 nonDisplay=0 discarded=0
[vplay] done pushed=721 rendered=719 dropped=0
```

三个“缺失”PTS 中，`29972737/29972738` 存在 1µs 舍入，实际真实缺失 2 帧：

| PTS (µs) | 时间 | 帧类型 | flags | packet size |
|---|---|---|---|---|
| 27181071 | 27.181s | B | 0x0 | 1558 |
| 28597738 | 28.598s | B | 0x0 | 420 |
| 29347738 | 29.348s | B | 0x0 | 383 |

最近输出 PTS 与它们都相差 **41667µs（一整帧）**，不是时间戳舍入。

## 4. 对比实验

| 模式 | 输入 | 输出 | 结论 |
|---|---|---|---|
| Surface + 同步查询 | 721 | 719 | 丢 2 帧 |
| Surface + 异步回调（`RegisterCallback`） | 721 | 719（718+1 主动丢） | 丢 2 帧 |
| **Buffer 输出 + CPU NV12 上屏** | 721 | 719（718+1 主动丢，`renderFail=0`） | **同样丢 2 帧** |
| 桌面 FFmpeg 软解 | 721 | 721 | 正常 |

### 已排除的变量

- 输入时间戳：PTS / DTS 两种赋值都试过
- 低延迟：`OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY` = 0 / 1 / 不设置
- 帧率：`OH_MD_KEY_FRAME_RATE` 保留 / 删除
- csd：塞首帧 / `OH_MD_KEY_CODEC_CONFIG`
- EOS：主动送 EOS / 不送 EOS（不送更差，718）
- 渲染：`RenderOutputBufferAtTime` / 立即 `RenderOutputBuffer`
- 输入节奏：允许领先 50ms / 300ms
- 回调并发：预解封装到内存队列，输入回调串行取包
- **Surface 绑定与否**：Buffer 模式不 `SetSurface`，仍丢同样帧

## 5. 复现步骤

1. 用原始 B 帧 mp4（例如 BBB 35–65s stream copy）打开 `OH_VideoDecoder`；
2. `Configure`（NV12、24fps、关闭低延迟）→ `SetSurface` 或 Buffer 模式；
3. 异步 `RegisterCallback`，`OnNeedInputBuffer` 按解码序喂包，`OnNewOutputBuffer` 取 PTS；
4. 统计输入包数、输出帧数、缺失 PTS。

## 6. 期望 vs 实际

- 期望：721 个输入包对应 721 个显示帧（与桌面 FFmpeg 一致）
- 实际：719 个输出帧，固定缺失 27.181s / 28.598s 的 B 帧（可能还有一帧受舍入影响）

## 7. 请求

请检查 `OMX.hisi.video.decoder.avc` 在 H.264 Main + B 帧（`has_b_frames=1`）下的 DPB / 输出队列实现，
特别是低延迟关闭、Surface 与 Buffer 两种输出模式共用路径时是否错误丢弃非参考 B 帧。
附件：原始 mp4、`ffprobe -show_packets/-show_frames` 输出、应用侧 `[vplay] missing#` 日志、三种模式计数。

## 8. 项目侧临时结论

- `videoProbe()` 对海思硬件返回 `surfaceBFrameRisk=true` / `knownIssue=hisi-bframe-drop`。
- `videoPlay(..., bufferMode=true)` 已实现（CPU NV12 上屏），但**不能修复**该丢帧，只用于对比/诊断。
- 严格帧对齐需等待厂商修复，或改用软件解码（FFmpeg）路径。
