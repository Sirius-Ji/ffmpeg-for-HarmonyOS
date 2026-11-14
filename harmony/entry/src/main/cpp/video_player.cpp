#include "video_player.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
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

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void StartCode(std::vector<uint8_t> *out) {
    out->insert(out->end(), {0, 0, 0, 1});
}

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

struct RawPkt {
    int64_t ptsUs = 0;
    std::vector<uint8_t> data;
};

struct AsyncCtx {
    std::shared_ptr<PlaybackCtx> ctl;
    std::shared_ptr<AudioPlayer> audio;
    bool audioActive = false;

    std::mutex mtx;
    std::deque<RawPkt> queue;
    int64_t firstPtsUs = -1;
    int64_t lastOutMs = NowMs();
    bool eosSent = false;
    bool failed = false;
    int pushed = 0;
    int rendered = 0;
    int dropped = 0;
};

void OnErrorCb(OH_AVCodec *, int32_t errorCode, void *userData) {
    auto *ctx = static_cast<AsyncCtx *>(userData);
    if (ctx) ctx->failed = true;
    OH_LOG_ERROR(LOG_APP, "[vasync] errorCode=%{public}d", errorCode);
}
void OnStreamChangedCb(OH_AVCodec *, OH_AVFormat *, void *) {}

void OnNeedInputCb(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData) {
    auto *ctx = static_cast<AsyncCtx *>(userData);
    if (!ctx || !buffer) return;

    std::lock_guard<std::mutex> lock(ctx->mtx);
    if (ctx->eosSent) return;
    if (ctx->queue.empty()) {
        OH_AVCodecBufferAttr attr{};
        attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
        OH_AVBuffer_SetBufferAttr(buffer, &attr);
        OH_VideoDecoder_PushInputBuffer(codec, index);
        ctx->eosSent = true;
        return;
    }

    RawPkt rp = std::move(ctx->queue.front());
    ctx->queue.pop_front();
    const int cap = OH_AVBuffer_GetCapacity(buffer);
    if ((int)rp.data.size() > cap) {
        OH_LOG_ERROR(LOG_APP, "[vasync] pkt %{public}d > cap %{public}d",
                     (int)rp.data.size(), cap);
        OH_AVCodecBufferAttr attr{};
        attr.size = 0;
        OH_AVBuffer_SetBufferAttr(buffer, &attr);
        OH_VideoDecoder_PushInputBuffer(codec, index);
        return;
    }
    std::memcpy(OH_AVBuffer_GetAddr(buffer), rp.data.data(), rp.data.size());

    OH_AVCodecBufferAttr attr{};
    attr.pts = rp.ptsUs;
    attr.size = (int32_t)rp.data.size();
    attr.flags = 0;
    OH_AVBuffer_SetBufferAttr(buffer, &attr);
    if (OH_VideoDecoder_PushInputBuffer(codec, index) == AV_ERR_OK) {
        if (ctx->firstPtsUs < 0) ctx->firstPtsUs = rp.ptsUs;
        ctx->pushed++;
    }
}

void OnNewOutputCb(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData) {
    auto *ctx = static_cast<AsyncCtx *>(userData);
    if (!ctx || !buffer) return;

    OH_AVCodecBufferAttr attr{};
    int64_t ptsUs = 0;
    if (OH_AVBuffer_GetBufferAttr(buffer, &attr) == AV_ERR_OK) ptsUs = attr.pts;

    const int64_t videoMs =
        (ptsUs - (ctx->firstPtsUs >= 0 ? ctx->firstPtsUs : 0)) / 1000;
    int64_t audioMs = -1;
    if (ctx->audioActive && ctx->audio) audioMs = ctx->audio->PlayedMs();

    if (ctx->audioActive && audioMs >= 0 && videoMs - audioMs < -80) {
        ctx->dropped++;
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }
    int64_t targetNs = NowNs();
    if (ctx->audioActive && audioMs >= 0 && videoMs > audioMs) {
        targetNs += (videoMs - audioMs) * 1000000;
    }
    OH_AVErrCode rc = OH_VideoDecoder_RenderOutputBufferAtTime(codec, index, targetNs);
    if (rc != AV_ERR_OK) OH_VideoDecoder_RenderOutputBuffer(codec, index);
    OH_VideoDecoder_FreeOutputBuffer(codec, index);
    ctx->rendered++;
    ctx->lastOutMs = NowMs();
    if (ctx->rendered == 1) {
        OH_LOG_INFO(LOG_APP, "[vasync] first frame audioMs=%{public}lld", (long long)audioMs);
    }
}

void PlaybackThread(std::string path, uint64_t surfaceId, std::shared_ptr<PlaybackCtx> ctl,
                    std::shared_ptr<AudioPlayer> audio, bool audioActive) {
    AVFormatContext *fmt = nullptr;
    OHNativeWindow *window = nullptr;
    OH_AVCodec *codec = nullptr;
    AsyncCtx ctx;
    ctx.ctl = ctl;
    ctx.audio = audio;
    ctx.audioActive = audioActive;
    const std::vector<uint8_t> emptyCsd;

    do {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) break;
        if (avformat_find_stream_info(fmt, nullptr) < 0) break;
        int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vidx < 0) break;
        AVStream *st = fmt->streams[vidx];
        if (st->codecpar->codec_id != AV_CODEC_ID_H264) break;
        const std::vector<uint8_t> csd =
            BuildAnnexBCsd(st->codecpar->extradata, st->codecpar->extradata_size);
        const int nalLen = NalLengthSize(st->codecpar->extradata, st->codecpar->extradata_size);

        AVPacket *pkt = av_packet_alloc();
        if (!pkt) break;
        bool first = true;
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == vidx) {
                RawPkt rp;
                if (pkt->pts != AV_NOPTS_VALUE) {
                    rp.ptsUs = av_rescale_q(pkt->pts, st->time_base, AV_TIME_BASE_Q);
                }
                rp.data = ToAnnexB(pkt->data, pkt->size, nalLen, csd, first);
                first = false;
                if (!rp.data.empty()) ctx.queue.push_back(std::move(rp));
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);

        const int width = st->codecpar->width;
        const int height = st->codecpar->height;
        AVRational fr = av_guess_frame_rate(fmt, st, nullptr);
        int fps = (fr.num > 0 && fr.den > 0) ? (int)(fr.num / fr.den + 0.5) : 30;
        if (fps <= 0 || fps > 120) fps = 30;
        OH_LOG_INFO(LOG_APP, "[vasync] queue=%{public}d %{public}dx%{public}d csd=%{public}d",
                    (int)ctx.queue.size(), width, height, (int)csd.size());

        if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window) != 0 ||
            window == nullptr) {
            break;
        }
        codec = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
        if (!codec) break;

        OH_AVFormat *format = OH_AVFormat_Create();
        if (!format) break;
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_FRAME_RATE, fps);
        OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY, 0);

        OH_AVErrCode rc = OH_VideoDecoder_Configure(codec, format);
        OH_AVFormat_Destroy(format);
        if (rc != AV_ERR_OK) break;
        rc = OH_VideoDecoder_SetSurface(codec, window);
        if (rc != AV_ERR_OK) break;
        OH_AVCodecCallback cb{OnErrorCb, OnStreamChangedCb, OnNeedInputCb, OnNewOutputCb};
        rc = OH_VideoDecoder_RegisterCallback(codec, cb, &ctx);
        if (rc != AV_ERR_OK) break;
        rc = OH_VideoDecoder_Prepare(codec);
        if (rc != AV_ERR_OK) break;
        rc = OH_VideoDecoder_Start(codec);
        if (rc != AV_ERR_OK) break;

        while (!ctl->stop.load() && !ctx.failed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (ctx.eosSent && NowMs() - ctx.lastOutMs > 3000) break;
        }
        OH_LOG_INFO(LOG_APP,
                    "[vasync] done pushed=%{public}d rendered=%{public}d dropped=%{public}d "
                    "eos=%{public}d failed=%{public}d",
                    ctx.pushed, ctx.rendered, ctx.dropped, ctx.eosSent ? 1 : 0,
                    ctx.failed ? 1 : 0);
    } while (false);

    if (codec) {
        OH_VideoDecoder_Stop(codec);
        OH_VideoDecoder_Destroy(codec);
    }
    if (window) OH_NativeWindow_DestroyNativeWindow(window);
    if (fmt) avformat_close_input(&fmt);
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
        if (gCtx) gCtx->stop.store(true);
        if (gThread.joinable()) gThread.detach();

        auto audio = std::make_shared<AudioPlayer>();
        bool audioOk = audio->Open(path) == 0;
        if (audioOk) audioOk = audio->Play() == 0;
        if (!audioOk) OH_LOG_WARN(LOG_APP, "[vasync] no audio track");

        auto ctx = std::make_shared<PlaybackCtx>();
        gCtx = ctx;
        gThread = std::thread(PlaybackThread, path, sid, ctx, audio, audioOk);
        if (result) {
            *result = audioOk ? "{\"state\":\"started\",\"audio\":true,\"mode\":\"async\"}"
                              : "{\"state\":\"started\",\"audio\":false,\"mode\":\"async\"}";
        }
    }
    OH_LOG_INFO(LOG_APP, "[vasync] start path=%{public}s surface=%{public}llu",
                path.c_str(), (unsigned long long)sid);
    return 0;
}

void VideoStop() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gCtx) gCtx->stop.store(true);
    if (gThread.joinable()) gThread.detach();
    gCtx.reset();
}
