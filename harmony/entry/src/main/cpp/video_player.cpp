#include "video_player.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <hilog/log.h>

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
std::atomic<bool> gStop{false};

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t NowMs() { return NowNs() / 1000000; }

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
        // 有些容器直接给 AnnexB
        if (size >= 4 && ed[0] == 0 && ed[1] == 0 && (ed[2] == 1 || (ed[2] == 0 && ed[3] == 1))) {
            out.assign(ed, ed + size);
        }
        return out;
    }

    int pos = 5;  // 跳过 version/profile/compat/level/lengthSize
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

// AVCC（4/3/2/1 字节长度前缀）→ AnnexB 起始码；首帧前面补 SPS/PPS。
std::vector<uint8_t> ToAnnexB(const uint8_t *data, int size, int nalLen,
                              const std::vector<uint8_t> &csd, bool prependCsd) {
    std::vector<uint8_t> out;
    if (prependCsd && !csd.empty()) out = csd;
    if (data == nullptr || size <= 0) return out;

    // 已经是 AnnexB
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

void PlaybackThread(std::string path, uint64_t surfaceId) {
    AVFormatContext *fmt = nullptr;
    OHNativeWindow *window = nullptr;
    OH_AVCodec *codec = nullptr;
    AVFormatContext *fmtOwned = nullptr;
    AVPacket *pkt = nullptr;

    do {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) {
            OH_LOG_ERROR(LOG_APP, "[vplay] open failed: %{public}s", path.c_str());
            break;
        }
        fmtOwned = fmt;
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
                    "[vplay] stream %{public}dx%{public}d fps=%{public}d csd=%{public}zu nalLen=%{public}d",
                    width, height, fps, csd.size(), nalLen);

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
        int64_t firstPtsUs = -1;
        int64_t lastOutMs = NowMs();
        const int64_t startMs = lastOutMs;
        bool havePkt = false;
        bool eosRead = false;
        bool eosSent = false;
        int pushed = 0;
        int rendered = 0;
        bool firstFrame = true;

        while (!gStop.load()) {
            if (NowMs() - startMs > 30000) {
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
                            OH_LOG_ERROR(LOG_APP, "[vplay] packet %{public}zu > cap %{public}d",
                                         annex.size(), cap);
                        }
                    }
                    av_packet_unref(pkt);
                    havePkt = false;
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
            OH_AVErrCode orc = OH_VideoDecoder_QueryOutputBuffer(codec, &outIdx, 5);
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
                OH_VideoDecoder_FreeOutputBuffer(codec, outIdx);
                rendered++;
                lastOutMs = NowMs();
                if (rendered == 1) {
                    OH_LOG_INFO(LOG_APP, "[vplay] first frame rendered at %{public}lld",
                                (long long)lastOutMs);
                }
            }

            if (eosSent && !havePkt && (NowMs() - lastOutMs) > 400) break;
        }

        OH_LOG_INFO(LOG_APP, "[vplay] done pushed=%{public}d rendered=%{public}d stop=%{public}d",
                    pushed, rendered, gStop.load() ? 1 : 0);
    } while (false);

    if (pkt != nullptr) av_packet_free(&pkt);
    if (codec != nullptr) {
        OH_VideoDecoder_Stop(codec);
        OH_VideoDecoder_Destroy(codec);
    }
    if (window != nullptr) OH_NativeWindow_DestroyNativeWindow(window);
    if (fmtOwned != nullptr) avformat_close_input(&fmtOwned);
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
        gStop.store(true);
        if (gThread.joinable()) gThread.join();
        gStop.store(false);
        gThread = std::thread(PlaybackThread, path, sid);
    }

    if (result) *result = "{\"state\":\"started\"}";
    OH_LOG_INFO(LOG_APP, "[vplay] start path=%{public}s surface=%{public}llu",
                path.c_str(), (unsigned long long)sid);
    return 0;
}

void VideoStop() {
    std::lock_guard<std::mutex> lock(gMutex);
    gStop.store(true);
    if (gThread.joinable()) gThread.join();
    gStop.store(false);
}
