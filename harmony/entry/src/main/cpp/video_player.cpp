#include "video_player.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <hilog/log.h>

#include "audio_player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_averrors.h>
#include <native_window/external_window.h>
}

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xFF01
#define LOG_TAG "FFM1"

namespace {

std::mutex gMutex;
std::thread gThread;

struct PlaybackCtx {
    std::atomic<bool> stop{false};
};

std::shared_ptr<PlaybackCtx> gCtx;

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t NowMs() { return NowNs() / 1000000; }

bool SleepCheck(const std::shared_ptr<PlaybackCtx> &ctx, int64_t ms) {
    const int64_t end = NowMs() + ms;
    while (NowMs() < end) {
        if (ctx->stop.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return !ctx->stop.load();
}

void StartCode(std::vector<uint8_t> *out) {
    out->push_back(0);
    out->push_back(0);
    out->push_back(0);
    out->push_back(1);
}

// avcC（mp4 的 extradata）拆成 AnnexB 的 SPS/PPS；已经是 AnnexB 就原样返回。
std::vector<uint8_t> BuildAnnexBCsd(const uint8_t *ed, int size) {
    std::vector<uint8_t> out;
    if (ed == nullptr || size <= 0) return out;

    if (!(size >= 7 && ed[0] == 1)) {
        if (size >= 4 && ed[0] == 0 && ed[1] == 0 && (ed[2] == 1 || (ed[2] == 0 && ed[3] == 1))) {
            out.assign(ed, ed + size);
        }
        return out;
    }

    int pos = 5;
    if (pos >= size) return out;
    int numSps = ed[pos++] & 0x1f;
    for (int i = 0; i < numSps && pos + 2 <= size; ++i) {
        int len = (ed[pos] << 8) | ed[pos + 1];
        pos += 2;
        if (len <= 0 || pos + len > size) break;
        StartCode(&out);
        out.insert(out.end(), ed + pos, ed + pos + len);
        pos += len;
    }
    if (pos < size) {
        int numPps = ed[pos++];
        for (int i = 0; i < numPps && pos + 2 <= size; ++i) {
            int len = (ed[pos] << 8) | ed[pos + 1];
            pos += 2;
            if (len <= 0 || pos + len > size) break;
            StartCode(&out);
            out.insert(out.end(), ed + pos, ed + pos + len);
            pos += len;
        }
    }
    return out;
}

int NalLengthSize(const uint8_t *ed, int size) {
    if (ed != nullptr && size >= 5 && ed[0] == 1) return (ed[4] & 0x03) + 1;
    return 4;
}

std::vector<uint8_t> ToAnnexB(const uint8_t *data, int size, int nalLen,
                              const std::vector<uint8_t> &csd, bool prependCsd) {
    std::vector<uint8_t> out;
    if (prependCsd && !csd.empty()) out = csd;
    if (data == nullptr || size <= 0) return out;

    if (size >= 4 && data[0] == 0 && data[1] == 0 &&
        (data[2] == 1 || (data[2] == 0 && data[3] == 1))) {
        out.insert(out.end(), data, data + size);
        return out;
    }

    int pos = 0;
    bool any = false;
    while (pos + nalLen <= size) {
        uint32_t len = 0;
        for (int i = 0; i < nalLen; ++i) len = (len << 8) | data[pos + i];
        pos += nalLen;
        if (len == 0 || pos + (int)len > size) break;
        StartCode(&out);
        out.insert(out.end(), data + pos, data + pos + len);
        pos += (int)len;
        any = true;
    }
    if (!any) out.insert(out.end(), data, data + size);
    return out;
}

void PlaybackThread(std::string path, uint64_t surfaceId, std::shared_ptr<PlaybackCtx> ctx,
                    std::shared_ptr<AudioPlayer> audio, bool audioActive) {
    AVFormatContext *fmt = nullptr;
    OHNativeWindow *window = nullptr;
    OH_AVCodec *codec = nullptr;
    AVPacket *pkt = nullptr;
    int dropped = 0;

    do {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) {
            OH_LOG_ERROR(LOG_APP, "[vplay] open failed: %{public}s", path.c_str());
            break;
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            OH_LOG_ERROR(LOG_APP, "[vplay] find_stream_info failed");
            break;
        }
        int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vidx < 0) {
            OH_LOG_ERROR(LOG_APP, "[vplay] no video stream");
            break;
        }
        AVStream *st = fmt->streams[vidx];
        if (st->codecpar->codec_id != AV_CODEC_ID_H264) {
            OH_LOG_ERROR(LOG_APP, "[vplay] only h264 supported, got %{public}d",
                         (int)st->codecpar->codec_id);
            break;
        }

        const uint8_t *ed = st->codecpar->extradata;
        const int edSize = st->codecpar->extradata_size;
        const std::vector<uint8_t> csd = BuildAnnexBCsd(ed, edSize);
        const int nalLen = NalLengthSize(ed, edSize);
        const int width = st->codecpar->width;
        const int height = st->codecpar->height;
        AVRational fr = av_guess_frame_rate(fmt, st, nullptr);
        int fps = (fr.num > 0 && fr.den > 0) ? (int)(fr.num / fr.den + 0.5) : 30;
        if (fps <= 0 || fps > 120) fps = 30;

        OH_LOG_INFO(LOG_APP,
                    "[vplay] stream %{public}dx%{public}d fps=%{public}d csd=%{public}d nalLen=%{public}d audio=%{public}d",
                    width, height, fps, (int)csd.size(), nalLen, audioActive ? 1 : 0);

        int wr = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window);
        if (wr != 0 || window == nullptr) {
            OH_LOG_ERROR(LOG_APP, "[vplay] CreateNativeWindow failed rc=%{public}d", wr);
            break;
        }

        codec = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
        if (codec == nullptr) {
            OH_LOG_ERROR(LOG_APP, "[vplay] CreateByMime failed");
            break;
        }

        OH_AVFormat *format = OH_AVFormat_Create();
        if (format == nullptr) {
            OH_LOG_ERROR(LOG_APP, "[vplay] AVFormat_Create failed");
            break;
        }
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_FRAME_RATE, fps);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_ENABLE_SYNC_MODE, 1);
        // B 帧重排必须关低延迟：低延迟模式按解码序输出且可能丢弃不完整帧
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY, 0);

        OH_AVErrCode rc = OH_VideoDecoder_Configure(codec, format);
        OH_AVFormat_Destroy(format);
        if (rc != AV_ERR_OK) {
            OH_LOG_ERROR(LOG_APP, "[vplay] Configure rc=%{public}d", (int)rc);
            break;
        }
        rc = OH_VideoDecoder_SetSurface(codec, window);
        if (rc != AV_ERR_OK) {
            OH_LOG_ERROR(LOG_APP, "[vplay] SetSurface rc=%{public}d", (int)rc);
            break;
        }
        rc = OH_VideoDecoder_Prepare(codec);
        if (rc != AV_ERR_OK) {
            OH_LOG_ERROR(LOG_APP, "[vplay] Prepare rc=%{public}d", (int)rc);
            break;
        }
        rc = OH_VideoDecoder_Start(codec);
        if (rc != AV_ERR_OK) {
            OH_LOG_ERROR(LOG_APP, "[vplay] Start rc=%{public}d", (int)rc);
            break;
        }

        pkt = av_packet_alloc();
        if (pkt == nullptr) break;

        const int64_t baseNs = NowNs();
        const int64_t fallbackStartMs = NowMs();
        int64_t firstPtsUs = -1;
        int64_t lastOutMs = NowMs();
        const int64_t startMs = lastOutMs;
        bool havePkt = false;
        bool eosRead = false;
        bool eosSent = false;
        int pushed = 0;
        int rendered = 0;
        int outSeen = 0;
        int audioMsAtStart = audioActive ? (int)audio->PlayedMs() : -1;
        bool firstFrame = true;

        // 音频时间戳没就绪时用墙钟兜底，绝不能返回 -1 让输入无限放行
        auto masterMs = [&]() -> int64_t {
            if (audioActive && audio) {
                const int64_t a = audio->PlayedMs();
                if (a >= 0) return a;
            }
            return NowMs() - fallbackStartMs;
        };

        while (!ctx->stop.load()) {
            if (NowMs() - startMs > 60000) {
                OH_LOG_WARN(LOG_APP, "[vplay] timeout guard hit");
                break;
            }

            if (!havePkt && !eosRead) {
                int r = av_read_frame(fmt, pkt);
                if (r >= 0) {
                    if (pkt->stream_index == vidx) {
                        havePkt = true;
                    } else {
                        av_packet_unref(pkt);
                    }
                } else {
                    eosRead = true;
                }
            }

            if (havePkt) {
                // 用音频主时钟限制输入领先量；不 sleep，避免堵住输出回收。
                bool inputReady = true;
                if (audioActive && audio && firstPtsUs >= 0 &&
                    pkt->pts != AV_NOPTS_VALUE) {
                    const int64_t pktMs =
                        av_rescale_q(pkt->pts, st->time_base, AV_TIME_BASE_Q) / 1000;
                    if (pktMs > masterMs() + 300) inputReady = false;
                }
                if (!inputReady) {
                    // 这一轮只收输出，下一轮再看时钟
                } else {
                    uint32_t inIdx = 0;
                if (OH_VideoDecoder_QueryInputBuffer(codec, &inIdx, 0) == AV_ERR_OK) {
                    OH_AVBuffer *buf = OH_VideoDecoder_GetInputBuffer(codec, inIdx);
                    if (buf != nullptr) {
                        int64_t ptsUs = 0;
                        if (pkt->pts != AV_NOPTS_VALUE) {
                            ptsUs = av_rescale_q(pkt->pts, st->time_base, AV_TIME_BASE_Q);
                        }
                        const std::vector<uint8_t> annex =
                            ToAnnexB(pkt->data, pkt->size, nalLen, csd, firstFrame);
                        const int cap = OH_AVBuffer_GetCapacity(buf);
                        if ((int)annex.size() <= cap) {
                            std::memcpy(OH_AVBuffer_GetAddr(buf), annex.data(), annex.size());
                            OH_AVCodecBufferAttr attr{};
                            attr.pts = ptsUs;
                            attr.size = (int32_t)annex.size();
                            attr.offset = 0;
                            attr.flags = 0;
                            OH_AVBuffer_SetBufferAttr(buf, &attr);
                            if (OH_VideoDecoder_PushInputBuffer(codec, inIdx) == AV_ERR_OK) {
                                if (firstPtsUs < 0) firstPtsUs = ptsUs;
                                pushed++;
                                firstFrame = false;
                            }
                        } else {
                            OH_LOG_ERROR(LOG_APP, "[vplay] packet %{public}d > cap %{public}d",
                                         (int)annex.size(), cap);
                        }
                    }
                    av_packet_unref(pkt);
                    havePkt = false;
                }
                }
            } else if (eosRead && !eosSent) {
                uint32_t inIdx = 0;
                if (OH_VideoDecoder_QueryInputBuffer(codec, &inIdx, 0) == AV_ERR_OK) {
                    OH_AVBuffer *buf = OH_VideoDecoder_GetInputBuffer(codec, inIdx);
                    if (buf != nullptr) {
                        OH_AVCodecBufferAttr attr{};
                        attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
                        OH_AVBuffer_SetBufferAttr(buf, &attr);
                        OH_VideoDecoder_PushInputBuffer(codec, inIdx);
                        eosSent = true;
                    }
                }
            }

            uint32_t outIdx = 0;
            OH_AVErrCode orc = OH_VideoDecoder_QueryOutputBuffer(codec, &outIdx, 5000);
            if (orc == AV_ERR_OK) {
                int64_t ptsUs = 0;
                OH_AVBuffer *ob = OH_VideoDecoder_GetOutputBuffer(codec, outIdx);
                if (ob != nullptr) {
                    OH_AVCodecBufferAttr oa{};
                    if (OH_AVBuffer_GetBufferAttr(ob, &oa) == AV_ERR_OK) ptsUs = oa.pts;
                }
                if (ptsUs <= 0) {
                    ptsUs = (firstPtsUs >= 0 ? firstPtsUs : 0) +
                            (int64_t)rendered * 1000000 / fps;
                }

                if (audioActive && audio) {
                    const int64_t videoMs =
                        (ptsUs - (firstPtsUs >= 0 ? firstPtsUs : 0)) / 1000;
                    const int64_t audioMs = masterMs();
                    if (outSeen % 24 == 0) {
                        OH_LOG_INFO(LOG_APP,
                                    "[vsync] out=%{public}d videoMs=%{public}lld "
                                    "audioMs=%{public}lld diff=%{public}lld",
                                    outSeen, (long long)videoMs, (long long)audioMs,
                                    (long long)(videoMs - audioMs));
                    }
                    outSeen++;
                    const int64_t diff = videoMs - audioMs;
                    if (diff < -80) {
                        // 视频落后音频超过 80ms：丢掉这一帧去追
                        OH_VideoDecoder_FreeOutputBuffer(codec, outIdx);
                        dropped++;
                        lastOutMs = NowMs();
                        continue;
                    }
                    // 按音频时钟预约渲染时刻，不持有输出 buffer 睡眠
                    const int64_t targetNs = NowNs() + (diff > 0 ? diff * 1000000 : 0);
                    OH_AVErrCode rr =
                        OH_VideoDecoder_RenderOutputBufferAtTime(codec, outIdx, targetNs);
                    if (rr != AV_ERR_OK) {
                        rr = OH_VideoDecoder_RenderOutputBuffer(codec, outIdx);
                    }
                } else {
                    int64_t deltaNs = 0;
                    if (firstPtsUs >= 0 && ptsUs > firstPtsUs) {
                        deltaNs = (ptsUs - firstPtsUs) * 1000;
                    }
                    int64_t targetNs = baseNs + deltaNs;
                    if (targetNs < NowNs()) targetNs = NowNs();
                    OH_AVErrCode rr =
                        OH_VideoDecoder_RenderOutputBufferAtTime(codec, outIdx, targetNs);
                    if (rr != AV_ERR_OK) {
                        rr = OH_VideoDecoder_RenderOutputBuffer(codec, outIdx);
                    }
                }
                OH_VideoDecoder_FreeOutputBuffer(codec, outIdx);
                rendered++;
                lastOutMs = NowMs();
                if (rendered == 1) {
                    OH_LOG_INFO(LOG_APP,
                                "[vplay] first frame rendered at %{public}lld audioMs=%{public}d",
                                (long long)lastOutMs, audioActive ? (int)audio->PlayedMs() : -1);
                }
            }

            if (eosSent && !havePkt && (NowMs() - lastOutMs) > 2000) break;
        }

        OH_LOG_INFO(LOG_APP,
                    "[vplay] done pushed=%{public}d rendered=%{public}d dropped=%{public}d "
                    "audioStart=%{public}d stop=%{public}d",
                    pushed, rendered, dropped, audioMsAtStart, ctx->stop.load() ? 1 : 0);
    } while (false);

    if (pkt != nullptr) av_packet_free(&pkt);
    if (codec != nullptr) {
        OH_VideoDecoder_Stop(codec);
        OH_VideoDecoder_Destroy(codec);
    }
    if (window != nullptr) OH_NativeWindow_DestroyNativeWindow(window);
    if (fmt != nullptr) avformat_close_input(&fmt);
}

}  // namespace

int VideoPlay(const std::string &path, const std::string &surfaceId, std::string *result) {
    uint64_t sid = 0;
    try {
        sid = std::stoull(surfaceId);
    } catch (...) {
        if (result) *result = "{\"error\":\"bad surfaceId\"}";
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(gMutex);

        // 播放中重播：只发停止信号并 detach，旧线程自己收尾；UI 不再等 join
        if (gCtx) gCtx->stop.store(true);
        if (gThread.joinable()) gThread.detach();

        auto audio = std::make_shared<AudioPlayer>();
        bool audioOk = audio->Open(path) == 0;
        if (audioOk) audioOk = audio->Play() == 0;
        if (!audioOk) {
            OH_LOG_WARN(LOG_APP, "[vplay] no audio track / audio open failed");
        } else {
            OH_LOG_INFO(LOG_APP, "[vplay] audio started route=%{public}s",
                        audio->StatusJson().c_str());
        }

        auto ctx = std::make_shared<PlaybackCtx>();
        gCtx = ctx;
        gThread = std::thread(PlaybackThread, path, sid, ctx, audio, audioOk);

        if (result) {
            *result = audioOk ? "{\"state\":\"started\",\"audio\":true}"
                              : "{\"state\":\"started\",\"audio\":false}";
        }
    }

    OH_LOG_INFO(LOG_APP, "[vplay] start path=%{public}s surface=%{public}llu",
                path.c_str(), (unsigned long long)sid);
    return 0;
}

void VideoStop() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gCtx) gCtx->stop.store(true);
    if (gThread.joinable()) gThread.detach();
    gCtx.reset();
}
