#include "decoder.h"

#include "fdio.h"
#include "ohcodec_decoder.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include <unistd.h>

#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xFF01
#define LOG_TAG "FFM1"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/defs.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <multimedia/player_framework/native_avcodec_base.h>
}

// M3.1 硬解总开关（默认开），NAPI 可切换用于 A/B 对比
namespace {
std::atomic<bool> g_hardwareDecode{true};
std::atomic<bool> g_ohcodecFatal{false};
std::mutex g_hwBlacklistMtx;
std::set<std::string> g_hwBlacklist;
}

void SetHardwareDecodeEnabled(bool on) { g_hardwareDecode.store(on); }
bool HardwareDecodeEnabled() { return g_hardwareDecode.load(); }
void MarkOhCodecFatal() { g_ohcodecFatal.store(true); }
bool OhCodecFatal() { return g_ohcodecFatal.load(); }

void BlacklistHwMime(const std::string &mime) {
    if (mime.empty()) return;
    std::lock_guard<std::mutex> lock(g_hwBlacklistMtx);
    if (g_hwBlacklist.insert(mime).second) {
        OH_LOG_WARN(LOG_APP, "[decoder] hw blacklist add mime=%{public}s", mime.c_str());
    }
}

bool IsHwBlacklisted(const std::string &mime) {
    std::lock_guard<std::mutex> lock(g_hwBlacklistMtx);
    return g_hwBlacklist.count(mime) > 0;
}

const char *AudioDecoder::HwStageName() const {
    switch (hwStage_.load()) {
        case 1: return "OH_AudioCodec_QueryOutputBuffer";
        case 2: return "av_read_frame";
        case 3: return "OH_AudioCodec_SendEos/QueryInputBuffer";
        case 4: return "OH_AudioCodec_PushInputBuffer";
        case 5: return "fallback/close";
        default: return "idle";
    }
}

bool AudioDecoder::HwStuckInOhCodec() const {
    const int stage = hwStage_.load();
    return stage == 1 || stage == 3 || stage == 4 || stage == 5;
}

namespace {

struct WavWriter {
    FILE *fp = nullptr;
    int64_t dataBytes = 0;
};

void PutU32(unsigned char *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
void PutU16(unsigned char *p, uint16_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}

void WriteWavHeader(FILE *fp, int sampleRate, int channels, int bits) {
    unsigned char h[44] = {0};
    const uint32_t byteRate = (uint32_t)sampleRate * channels * bits / 8;
    memcpy(h, "RIFF", 4);
    PutU32(h + 4, 36);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    PutU32(h + 16, 16);
    PutU16(h + 20, 1);                    // PCM
    PutU16(h + 22, (uint16_t)channels);
    PutU32(h + 24, (uint32_t)sampleRate);
    PutU32(h + 28, byteRate);
    PutU16(h + 32, (uint16_t)(channels * bits / 8));
    PutU16(h + 34, (uint16_t)bits);
    memcpy(h + 36, "data", 4);
    PutU32(h + 40, 0);
    fwrite(h, 1, sizeof(h), fp);
}

void PatchWavSize(FILE *fp, int64_t dataBytes) {
    unsigned char b[4];
    PutU32(b, (uint32_t)(36 + dataBytes));
    fseek(fp, 4, SEEK_SET);
    fwrite(b, 1, 4, fp);
    PutU32(b, (uint32_t)dataBytes);
    fseek(fp, 40, SEEK_SET);
    fwrite(b, 1, 4, fp);
    fseek(fp, 0, SEEK_END);
}

std::string AvErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

}  // namespace

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder() { Close(); }

void AudioDecoder::SetError(const std::string &what) { err_ = what; }

void AudioDecoder::Close() {
    if (hw_) { hw_->Close(); hw_.reset(); }
    if (swrHw_) { swr_free((SwrContext **)&swrHw_); swrHw_ = nullptr; }
    if (swrStream_) { swr_free((SwrContext **)&swrStream_); swrStream_ = nullptr; }
    if (swr_) { swr_free((SwrContext **)&swr_); swr_ = nullptr; }
    if (frame_) { av_frame_free((AVFrame **)&frame_); frame_ = nullptr; }
    if (packet_) { av_packet_free((AVPacket **)&packet_); packet_ = nullptr; }
    if (dec_) { avcodec_free_context((AVCodecContext **)&dec_); dec_ = nullptr; }
    if (fmt_) { avformat_close_input((AVFormatContext **)&fmt_); fmt_ = nullptr; }
    if (avio_) {
        AVIOContext *pb = (AVIOContext *)avio_;
        av_freep(&pb->buffer);
        avio_context_free(&pb);
        avio_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    pending_.clear();
    pendingPos_ = 0;
    flushSent_ = false;
    eof_ = false;
    streamIndex_ = -1;
    outChannels_ = 0;
    outSampleRate_ = 0;
    route_ = "ffmpeg";
    routeWhy_.clear();
    hwEosSent_ = false;
    hwEosIdle_ = 0;
    hwGotEos_ = false;
    hwDeadlineExt_ = 0;
    hwExpectedSamples_ = 0;
    hwSrcSamplesOut_ = 0;
    hwLastPtsUs_ = 0;
    hwRate_ = 0;
    hwCh_ = 0;
    adts_.clear();
}

int AudioDecoder::Open(const std::string &path, AudioInfo *info) {
    Close();
    err_.clear();

    AVFormatContext *fmt = nullptr;
    int ret = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        SetError("avformat_open_input: " + AvErr(ret));
        return ret;
    }
    fmt_ = fmt;
    return FinishOpen(info);
}

int AudioDecoder::OpenFd(int fd, AudioInfo *info) {
    Close();
    err_.clear();

    fd_ = ::dup(fd);
    if (fd_ < 0) {
        SetError("dup(fd) failed");
        return AVERROR(errno);
    }

    constexpr int kIoBuf = 64 * 1024;
    auto *iobuf = static_cast<unsigned char *>(av_malloc(kIoBuf));
    if (iobuf == nullptr) {
        SetError("av_malloc(io buffer) failed");
        return AVERROR(ENOMEM);
    }
    AVIOContext *avio = avio_alloc_context(iobuf, kIoBuf, 0, (void *)(intptr_t)fd_,
                                          ffio::ReadPacket, nullptr, ffio::Seek);
    if (avio == nullptr) {
        av_free(iobuf);
        SetError("avio_alloc_context failed");
        return AVERROR(ENOMEM);
    }
    avio_ = avio;

    AVFormatContext *fmt = avformat_alloc_context();
    if (fmt == nullptr) {
        SetError("avformat_alloc_context failed");
        return AVERROR(ENOMEM);
    }
    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    fmt_ = fmt;
    const int ret = avformat_open_input(&fmt, nullptr, nullptr, nullptr);
    if (ret < 0) {
        fmt_ = nullptr;
        SetError("avformat_open_input(fd): " + AvErr(ret));
        return ret;
    }
    fmt_ = fmt;
    return FinishOpen(info);
}

int AudioDecoder::FinishOpen(AudioInfo *info) {
    AVFormatContext *fmt = (AVFormatContext *)fmt_;
    int ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        SetError("avformat_find_stream_info: " + AvErr(ret));
        return ret;
    }

    // 自己选音频流：av_find_best_stream 会跳过“没有解码器”的流，
    // 结果把“不支持的音频编码”误报成“没有音频流”。这里分开报错。
    const AVCodec *codec = nullptr;
    streamIndex_ = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream *s = fmt->streams[i];
        if (s->codecpar != nullptr && s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            streamIndex_ = (int)i;
            codec = avcodec_find_decoder(s->codecpar->codec_id);
            break;
        }
    }
    if (streamIndex_ < 0) {
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVStream *s = fmt->streams[i];
            if (s->codecpar == nullptr) continue;
            OH_LOG_WARN(LOG_APP,
                        "[decoder] stream#%{public}u type=%{public}d codec=%{public}s id=%{public}d",
                        i, (int)s->codecpar->codec_type,
                        avcodec_get_name(s->codecpar->codec_id), (int)s->codecpar->codec_id);
        }
        SetError("no audio stream");
        return AVERROR(EINVAL);
    }
    if (codec == nullptr) {
        const AVCodecParameters *p = fmt->streams[streamIndex_]->codecpar;
        const std::string name = avcodec_get_name(p->codec_id);
        OH_LOG_ERROR(LOG_APP,
                     "[decoder] audio codec '%{public}s' not compiled in (id=%{public}d tag=0x%{public}08x)",
                     name.c_str(), (int)p->codec_id, (unsigned)p->codec_tag);
        SetError("unsupported audio codec: " + name + " (not compiled in)");
        return AVERROR_DECODER_NOT_FOUND;
    }

    AVStream *st = fmt->streams[streamIndex_];
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec) {
        SetError("avcodec_alloc_context3 failed");
        return AVERROR(ENOMEM);
    }
    dec_ = dec;
    ret = avcodec_parameters_to_context(dec, st->codecpar);
    if (ret < 0) {
        SetError("avcodec_parameters_to_context: " + AvErr(ret));
        return ret;
    }
    dec->pkt_timebase = st->time_base;
    ret = avcodec_open2(dec, codec, nullptr);
    if (ret < 0) {
        SetError("avcodec_open2: " + AvErr(ret));
        return ret;
    }

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!frame_ || !packet_) {
        SetError("alloc frame/packet failed");
        return AVERROR(ENOMEM);
    }

    // ---- M3 路由：AAC / MP3 / FLAC 优先试系统硬解，失败回退 FFmpeg ----
    route_ = "ffmpeg";
    routeWhy_.clear();
    const char *hwMime = nullptr;
    if (HardwareDecodeEnabled() && !OhCodecFatal()) {
        switch (codec->id) {
            case AV_CODEC_ID_AAC: hwMime = OH_AVCODEC_MIMETYPE_AUDIO_AAC; break;
            case AV_CODEC_ID_MP3: hwMime = OH_AVCODEC_MIMETYPE_AUDIO_MPEG; break;
            case AV_CODEC_ID_FLAC: hwMime = OH_AVCODEC_MIMETYPE_AUDIO_FLAC; break;
            default: break;
        }
        if (hwMime != nullptr && IsHwBlacklisted(hwMime)) {
            routeWhy_ = "hw blacklist";
            hwMime = nullptr;  // 本进程内该 mime 已确认不稳定，直接软解
        }
        if (hwMime != nullptr) {
            const AVCodecParameters *par = st->codecpar;
            const int srcChannels = par->ch_layout.nb_channels;
            std::string why;
            if (OhCodecDecoder::CapabilityAllows(hwMime, par->sample_rate, srcChannels, &why)) {
                auto hw = std::make_unique<OhCodecDecoder>();
                if (hw->Open(hwMime, par->sample_rate, srcChannels, par->extradata,
                             par->extradata_size, &why) == 0) {
                    hw_ = std::move(hw);
                    hwRate_ = par->sample_rate;
                    hwCh_ = srcChannels;
                    hwEosSent_ = false;
                    hwGotEos_ = false;
                    hwEosIdle_ = 0;
                    hwEosTry_ = 0;
                    hwDeadlineExt_ = 0;
                    hwExpectedSamples_ = 0;
                hwSrcSamplesOut_ = 0;
                    if (fmt->duration > 0) {
                        hwExpectedSamples_ =
                            av_rescale(fmt->duration, par->sample_rate, AV_TIME_BASE);
                    }
                    hwLastPtsUs_ = 0;
                    route_ = "ohcodec";
                    routeWhy_.clear();
                } else {
                    routeWhy_ = why;
                }
            } else {
                routeWhy_ = why;
            }
        }
    }
    if (OhCodecFatal()) {
        routeWhy_ = "circuit breaker tripped: ohcodec stuck earlier";
    }
    OH_LOG_INFO(LOG_APP,
                "[decoder] route=%{public}s mime=%{public}s codec=%{public}s src=%{public}d/%{public}d reason=%{public}s",
                route_.c_str(), hwMime != nullptr ? hwMime : "-", codec->name,
                st->codecpar->sample_rate, st->codecpar->ch_layout.nb_channels,
                routeWhy_.c_str());

    if (info) {
        info->format = fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "?";
        info->codec = codec && codec->name ? codec->name : "?";
        info->sampleRate = dec->sample_rate;
        info->channels = dec->ch_layout.nb_channels;
        info->bitRate = (int64_t)st->codecpar->bit_rate;
        if (fmt->duration > 0) {
            info->durationMs = fmt->duration * 1000 / AV_TIME_BASE;
        } else if (st->duration > 0) {
            info->durationMs = av_rescale_q(st->duration, st->time_base, AVRational{1, 1000});
        }
    }
    return 0;
}

std::string AudioDecoder::InfoJson(const AudioInfo &info) const {
    std::ostringstream os;
    os << "{\"format\":\"" << info.format << "\",\"codec\":\"" << info.codec
       << "\",\"sampleRate\":" << info.sampleRate << ",\"channels\":" << info.channels
       << ",\"durationMs\":" << info.durationMs << ",\"bitRate\":" << info.bitRate << "}";
    return os.str();
}

int AudioDecoder::DecodeAllToWav(const std::string &outPath, int64_t *framesOut, int64_t *bytesOut) {
    if (!fmt_ || !dec_) {
        SetError("decoder not open");
        return AVERROR(EINVAL);
    }
    AVFormatContext *fmt = (AVFormatContext *)fmt_;
    AVCodecContext *dec = (AVCodecContext *)dec_;
    AVFrame *frame = (AVFrame *)frame_;
    AVPacket *pkt = (AVPacket *)packet_;

    FILE *fp = fopen(outPath.c_str(), "wb");
    if (!fp) {
        SetError("cannot open output: " + outPath);
        return AVERROR(EIO);
    }

    outChannels_ = dec->ch_layout.nb_channels > 0 ? dec->ch_layout.nb_channels : 2;
    outSampleRate_ = dec->sample_rate > 0 ? dec->sample_rate : 48000;
    WriteWavHeader(fp, outSampleRate_, outChannels_, 16);

    int64_t totalFrames = 0;
    int64_t totalBytes = 0;
    hash_ = 1469598103934665603ULL;  // FNV-1a 64 offset basis

    auto handle_frame = [&](AVFrame *f) -> int {
        if (!swr_) {
            AVChannelLayout outLayout;
            av_channel_layout_copy(&outLayout, &f->ch_layout);
            SwrContext *swr = nullptr;
            int r = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, outSampleRate_,
                                        &f->ch_layout, (AVSampleFormat)f->format, f->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&outLayout);
            if (r < 0 || !swr) {
                SetError("swr_alloc_set_opts2: " + AvErr(r));
                return r < 0 ? r : AVERROR(ENOMEM);
            }
            if (swr_init(swr) < 0) {
                SetError("swr_init failed");
                swr_free(&swr);
                return AVERROR(EINVAL);
            }
            swr_ = swr;
        }
        const int maxOut = (int)av_rescale_rnd(swr_get_delay((SwrContext *)swr_, f->sample_rate) + f->nb_samples,
                                               outSampleRate_, f->sample_rate, AV_ROUND_UP);
        std::vector<unsigned char> out((size_t)maxOut * outChannels_ * 2 + 4096);
        uint8_t *outPtr[1] = {out.data()};
        int got = swr_convert((SwrContext *)swr_, outPtr, maxOut,
                              (const uint8_t **)f->extended_data, f->nb_samples);
        if (got < 0) {
            SetError("swr_convert: " + AvErr(got));
            return got;
        }
        const int64_t bytes = (int64_t)got * outChannels_ * 2;
        if (bytes > 0 && fwrite(out.data(), 1, (size_t)bytes, fp) != (size_t)bytes) {
            SetError("write output failed");
            return AVERROR(EIO);
        }
        for (int64_t i = 0; i < bytes; ++i) {
            hash_ ^= out[(size_t)i];
            hash_ *= 1099511628211ULL;
        }
        totalFrames += got;
        totalBytes += bytes;
        return 0;
    };

    int ret = 0;
    while ((ret = av_read_frame(fmt, pkt)) >= 0) {
        if (pkt->stream_index != streamIndex_) {
            av_packet_unref(pkt);
            continue;
        }
        ret = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            SetError("avcodec_send_packet: " + AvErr(ret));
            break;
        }
        while ((ret = avcodec_receive_frame(dec, frame)) >= 0) {
            ret = handle_frame(frame);
            av_frame_unref(frame);
            if (ret < 0) break;
        }
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) break;
        ret = 0;
    }
    if (ret == AVERROR_EOF) ret = 0;

    // flush
    avcodec_send_packet(dec, nullptr);
    int r2;
    while ((r2 = avcodec_receive_frame(dec, frame)) >= 0) {
        if (ret == 0) ret = handle_frame(frame);
        av_frame_unref(frame);
    }

    PatchWavSize(fp, totalBytes);
    fclose(fp);

    if (framesOut) *framesOut = totalFrames;
    if (bytesOut) *bytesOut = totalBytes;
    return ret;
}

int AudioDecoder::ReadPackets(ReadStats *out) {
    if (!fmt_ || streamIndex_ < 0 || !out) {
        SetError("decoder not open");
        return AVERROR(EINVAL);
    }
    AVFormatContext *fmt = (AVFormatContext *)fmt_;
    AVPacket *pkt = (AVPacket *)packet_;
    AVStream *st = fmt->streams[streamIndex_];

    bool first = true;
    int ret = 0;
    while ((ret = av_read_frame(fmt, pkt)) >= 0) {
        if (pkt->stream_index == streamIndex_) {
            out->packets += 1;
            out->bytes += pkt->size;
            int64_t ms = 0;
            if (pkt->pts != AV_NOPTS_VALUE) {
                ms = av_rescale_q(pkt->pts, st->time_base, AVRational{1, 1000});
            }
            if (first) {
                out->firstPtsMs = ms;
                first = false;
            }
            out->lastPtsMs = ms;
        }
        av_packet_unref(pkt);
    }
    // 读干净之后文件在 EOF，这里不还原，调用方读完就不会再读
    return ret == AVERROR_EOF ? 0 : ret;
}

// ---------------- M2：流式解码 ----------------

int AudioDecoder::StreamFillPending(int outRate, int outChannels) {
    if (!fmt_ || !dec_) {
        SetError("decoder not open");
        return AVERROR(EINVAL);
    }
    AVFormatContext *fmt = (AVFormatContext *)fmt_;
    AVCodecContext *dec = (AVCodecContext *)dec_;
    AVFrame *frame = (AVFrame *)frame_;
    AVPacket *pkt = (AVPacket *)packet_;

    if (pendingPos_ < pending_.size()) return 0;  // 上一帧还没消费完
    pending_.clear();
    pendingPos_ = 0;

    // M3.1：AAC 硬解路径
    if (hw_) return HwFillPending(outRate, outChannels);

    for (;;) {
        int ret = avcodec_receive_frame(dec, frame);
        if (ret == 0) {
            if (!swrStream_) {
                AVChannelLayout outLayout;
                if (frame->ch_layout.nb_channels == outChannels) {
                    av_channel_layout_copy(&outLayout, &frame->ch_layout);
                } else {
                    av_channel_layout_default(&outLayout, outChannels);
                }
                SwrContext *swr = nullptr;
                int r = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, outRate,
                                            &frame->ch_layout, (AVSampleFormat)frame->format,
                                            frame->sample_rate, 0, nullptr);
                av_channel_layout_uninit(&outLayout);
                if (r < 0 || !swr) {
                    SetError("swr_alloc_set_opts2(stream): " + AvErr(r));
                    return r < 0 ? r : AVERROR(ENOMEM);
                }
                if (swr_init(swr) < 0) {
                    swr_free(&swr);
                    SetError("swr_init(stream) failed");
                    return AVERROR(EINVAL);
                }
                swrStream_ = swr;
            }

            const int maxOut = (int)av_rescale_rnd(
                swr_get_delay((SwrContext *)swrStream_, frame->sample_rate) + frame->nb_samples,
                outRate, frame->sample_rate, AV_ROUND_UP);
            pending_.resize((size_t)maxOut * outChannels * 2 + 4096);
            uint8_t *dst[1] = {pending_.data()};
            int got = swr_convert((SwrContext *)swrStream_, dst, maxOut,
                                  (const uint8_t **)frame->extended_data, frame->nb_samples);
            av_frame_unref(frame);
            if (got < 0) {
                SetError("swr_convert(stream): " + AvErr(got));
                return got;
            }
            pending_.resize((size_t)got * outChannels * 2);
            if (pending_.empty()) continue;  // 重采样暂时没产出，继续拿帧
            return 0;
        }

        if (ret == AVERROR(EAGAIN)) {
            if (flushSent_) return AVERROR_EOF;
            int r = av_read_frame(fmt, pkt);
            if (r == AVERROR_EOF) {
                avcodec_send_packet(dec, nullptr);  // 进 drain 阶段
                flushSent_ = true;
                continue;
            }
            if (r < 0) {
                SetError("av_read_frame: " + AvErr(r));
                return r;
            }
            if (pkt->stream_index != streamIndex_) {
                av_packet_unref(pkt);
                continue;
            }
            int sr = avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
            if (sr < 0 && sr != AVERROR(EAGAIN)) {
                SetError("avcodec_send_packet: " + AvErr(sr));
                return sr;
            }
            continue;
        }

        if (ret == AVERROR_EOF) {
            // 把 swr 里残留的样本倒干净
            if (swrStream_) {
                const int kMax = 4096;
                pending_.resize((size_t)kMax * outChannels * 2);
                uint8_t *dst[1] = {pending_.data()};
                int got = swr_convert((SwrContext *)swrStream_, dst, kMax, nullptr, 0);
                if (got > 0) {
                    pending_.resize((size_t)got * outChannels * 2);
                    return 0;
                }
                pending_.clear();
            }
            eof_ = true;
            return AVERROR_EOF;
        }

        SetError("avcodec_receive_frame: " + AvErr(ret));
        return ret;
    }
}

int AudioDecoder::ReadPcm(std::vector<uint8_t> &out, int maxFrames, int outRate, int outChannels) {
    out.clear();
    if (maxFrames <= 0 || outRate <= 0 || outChannels <= 0) return AVERROR(EINVAL);

    const size_t want = (size_t)maxFrames * outChannels * 2;
    while (out.size() < want) {
        if (stopRequested_.load()) return AVERROR_EXIT;
        int ret = StreamFillPending(outRate, outChannels);
        if (ret < 0) {
            // 已经有数据就先给出去，EOF 留到下次再报，避免丢尾巴
            if (!out.empty()) return 0;
            return ret;
        }
        const size_t avail = pending_.size() - pendingPos_;
        const size_t take = std::min(avail, want - out.size());
        out.insert(out.end(), pending_.begin() + pendingPos_, pending_.begin() + pendingPos_ + take);
        pendingPos_ += take;
    }
    return 0;
}

// ---------------- M3.1：AAC 硬解后端 ----------------

namespace {
// FFmpeg profile -> AAC AOT（音频对象类型）；ADTS 的 2-bit profile = AOT-1
int AacAotFromAvProfile(int profile) {
    switch (profile) {
        case AV_PROFILE_AAC_MAIN: return 1;
        case AV_PROFILE_AAC_SSR: return 3;
        case AV_PROFILE_AAC_LTP: return 4;
        case AV_PROFILE_AAC_HE: return 5;
        case AV_PROFILE_AAC_HE_V2: return 29;
        default: return 2;  // AAC-LC
    }
}
}  // namespace

void AudioDecoder::BuildElementary(void *pktPtr) {
    AVPacket *pkt = (AVPacket *)pktPtr;
    adts_.clear();
    if (pkt == nullptr || pkt->size <= 0 || pkt->data == nullptr) return;

    AVFormatContext *fmt = (AVFormatContext *)fmt_;
    if (fmt == nullptr || streamIndex_ < 0) return;
    const AVCodecParameters *par = fmt->streams[streamIndex_]->codecpar;

    // MP3 / FLAC：demuxer 出来的就是裸码流，原样推给系统解码器
    if (par->codec_id == AV_CODEC_ID_MP3 || par->codec_id == AV_CODEC_ID_FLAC) {
        adts_.assign(pkt->data, pkt->data + pkt->size);
        return;
    }

    // 已经是 ADTS（裸 aac / adts 容器）就不再包一层
    if (pkt->size >= 2 && pkt->data[0] == 0xFF && (pkt->data[1] & 0xF6) == 0xF0) {
        adts_.assign(pkt->data, pkt->data + pkt->size);
        return;
    }

    // AAC（m4a 里是裸帧）：补 7 字节 ADTS 头
    static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000, 7350};
    int rateIdx = 4;  // 44100 兜底
    for (int i = 0; i < 13; ++i) {
        if (kRates[i] == par->sample_rate) {
            rateIdx = i;
            break;
        }
    }
    const int aot = AacAotFromAvProfile(par->profile >= 0 ? par->profile : AV_PROFILE_AAC_LOW);
    // HE-AAC / HEv2 的 SBR/PS 是隐式扩展，ADTS 头只能声明核心 LC（AOT 2），
    // 否则 AOT 5/29 放不进 2 bit 的 profile 字段，会出现“创建成功但无输出”。
    const int adtsProfile = (aot == 5 || aot == 29) ? 1 : ((aot - 1) & 0x3);
    const int channels = par->ch_layout.nb_channels > 0 ? par->ch_layout.nb_channels : hwCh_;
    const int frameLen = 7 + pkt->size;

    adts_.resize((size_t)frameLen);
    uint8_t *a = adts_.data();
    a[0] = 0xFF;
    a[1] = 0xF1;  // MPEG-4, layer 0, 无 CRC
    a[2] = (uint8_t)((adtsProfile << 6) | (rateIdx << 2) | (channels >> 2));
    a[3] = (uint8_t)(((channels & 3) << 6) | ((frameLen >> 11) & 0x3));
    a[4] = (uint8_t)((frameLen >> 3) & 0xFF);
    a[5] = (uint8_t)(((frameLen & 7) << 5) | 0x1F);
    a[6] = 0xFC;
    memcpy(a + 7, pkt->data, (size_t)pkt->size);
}

int AudioDecoder::HwSwrToPending(const uint8_t *data, int samples, int srcRate, int outRate,
                                 int outChannels) {
    // 按容器时长截断：硬解可能多出 priming/padding，超过期望采样数的部分直接丢弃
    if (hwExpectedSamples_ > 0) {
        const int64_t remain = hwExpectedSamples_ - hwSrcSamplesOut_;
        if (remain <= 0) {
            eof_ = true;
            OH_LOG_INFO(LOG_APP,
                        "[decoder] route=%{public}s eof(reached expected %{public}lld samples)",
                        route_.c_str(), hwExpectedSamples_);
            return AVERROR_EOF;
        }
        if ((int64_t)samples > remain) samples = (int)remain;
    }
    hwSrcSamplesOut_ += samples;
    if (swrHw_ == nullptr) {
        AVChannelLayout outLayout;
        AVChannelLayout inLayout;
        av_channel_layout_default(&outLayout, outChannels);
        av_channel_layout_default(&inLayout, hwCh_ > 0 ? hwCh_ : outChannels);

        SwrContext *swr = nullptr;
        int r = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, outRate,
                                    &inLayout, AV_SAMPLE_FMT_S16, srcRate, 0, nullptr);
        av_channel_layout_uninit(&outLayout);
        av_channel_layout_uninit(&inLayout);
        if (r < 0 || swr == nullptr) {
            SetError("swr_alloc_set_opts2(hw): " + AvErr(r));
            return r < 0 ? r : AVERROR(ENOMEM);
        }
        if (swr_init(swr) < 0) {
            swr_free(&swr);
            SetError("swr_init(hw) failed");
            return AVERROR(EINVAL);
        }
        swrHw_ = swr;
    }

    const int est = (int)av_rescale_rnd(samples, outRate, srcRate, AV_ROUND_UP) + 256;
    pending_.resize((size_t)est * outChannels * 2);
    uint8_t *dst[1] = {pending_.data()};
    const uint8_t *in[1] = {data};
    const int got = swr_convert((SwrContext *)swrHw_, dst, est, in, samples);
    if (got < 0) {
        SetError("swr_convert(hw): " + AvErr(got));
        return got;
    }
    pending_.resize((size_t)got * outChannels * 2);
    return 0;
}

void AudioDecoder::FallbackToSoftware(const std::string &why) {
    OH_LOG_WARN(LOG_APP, "[decoder] fallback to ffmpeg: %{public}s (stage=%{public}s)", why.c_str(),
                HwStageName());
    hwStage_.store(5);
    if (hw_) {
        if (!hwEosSent_) {
            // 只有数据路径上的回退才拉黑；EOS 阶段的瞬时错误不算硬件不可用
            BlacklistHwMime(hw_->mime());
        }
        const bool nothingDecoded = hw_->outSamples() == 0;
        // 实例可能已经卡死：把 Flush/Stop/Destroy 丢到后台线程，解码线程立刻继续
        hw_->AbandonAsync();
        hw_.reset();
        if (nothingDecoded) {
            // 硬解还没产出过任何数据：回到文件开头用软解重解，保证一个字节不差
            AVFormatContext *fmt = (AVFormatContext *)fmt_;
            AVCodecContext *dec = (AVCodecContext *)dec_;
            if (fmt != nullptr && streamIndex_ >= 0) {
                av_seek_frame(fmt, streamIndex_, 0, AVSEEK_FLAG_BACKWARD);
            }
            if (dec != nullptr) avcodec_flush_buffers(dec);
            pending_.clear();
            pendingPos_ = 0;
            flushSent_ = false;
            OH_LOG_INFO(LOG_APP, "[decoder] fallback before first output -> rewind to 0");
        }
    }
    if (swrHw_) {
        swr_free((SwrContext **)&swrHw_);
        swrHw_ = nullptr;
    }
    // 不 seek、不清 pending：后续包直接交给 FFmpeg 从当前位置续解，避免丢帧/重复
    route_ = "ffmpeg(fallback)";
    routeWhy_ = why;
    flushSent_ = false;
    hwStage_.store(0);
}

int AudioDecoder::HwFillPending(int outRate, int outChannels) {
    AVFormatContext *fmt = (AVFormatContext *)fmt_;
    AVPacket *pkt = (AVPacket *)packet_;
    const int srcRate = hwRate_ > 0 ? hwRate_ : outRate;
    // 非阻塞轮询 + 1ms 让出；期间每轮都检查取消标志，Stop 能及时返回
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    for (;;) {
        if (stopRequested_.load()) {
            OH_LOG_INFO(LOG_APP, "[decoder] hw cancelled by stop");
            return AVERROR_EXIT;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            if (hwEosSent_) {
                const bool shortTail = hwExpectedSamples_ > 0 &&
                                       hw_->outSamples() + 1024 < hwExpectedSamples_;
                if (shortTail && hwDeadlineExt_ < 2) {
                    // 尾部明显少了：再给 2 秒，避免截断
                    hwDeadlineExt_++;
                    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    OH_LOG_WARN(LOG_APP,
                                "[decoder] short tail (got=%{public}lld expect~%{public}lld), extend wait #%{public}d",
                                hw_->outSamples(), hwExpectedSamples_, hwDeadlineExt_);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (swrHw_ != nullptr) {
                    const int kMax = 4096;
                    pending_.resize((size_t)kMax * outChannels * 2);
                    uint8_t *dst[1] = {pending_.data()};
                    const int got = swr_convert((SwrContext *)swrHw_, dst, kMax, nullptr, 0);
                    if (got > 0) {
                        pending_.resize((size_t)got * outChannels * 2);
                        return 0;
                    }
                    pending_.clear();
                }
                eof_ = true;
                OH_LOG_INFO(LOG_APP,
                            "[decoder] route=%{public}s eof(timeout after EOS) pushed=%{public}lld inBytes=%{public}lld outBytes=%{public}lld outSamples=%{public}lld",
                            route_.c_str(), hw_->pushedPackets(), hw_->pushedBytes(), hw_->outBytes(),
                            hw_->outSamples());
                return AVERROR_EOF;
            }
            FallbackToSoftware("no progress (>2s)");
            return StreamFillPending(outRate, outChannels);
        }

        std::string why;
        std::vector<uint8_t> pcm;
        int samples = 0;
        hwStage_.store(1);
        const int pr = hw_->Pull(&pcm, &samples, &why);
        hwStage_.store(0);

        if (pr == 0) {
            if (samples > 0) {
                hwEosIdle_ = 0;
                return HwSwrToPending(pcm.data(), samples, srcRate, outRate, outChannels);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (pr == 2) {
            // 收到系统 EOS 标记：不立刻结束，后面再等 100ms 确认没有尾部数据
            hwGotEos_ = true;
            hwEosIdle_ = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (pr < 0) {
            FallbackToSoftware(why);
            return StreamFillPending(outRate, outChannels);
        }

        // pr == 1：暂时没有输出
        if (hwEosSent_) {
            ++hwEosIdle_;
            // 收到 EOS 标记：再等 100ms（20 次）确认真的没数据；
            // 没收到标记：维持 500ms 空闲判定，兼容不回显 EOS 的设备（如 192k FLAC）
            const int limit = hwGotEos_ ? 20 : 500;
            if (hwEosIdle_ >= limit) {
                if (swrHw_ != nullptr) {
                    const int kMax = 4096;
                    pending_.resize((size_t)kMax * outChannels * 2);
                    uint8_t *dst[1] = {pending_.data()};
                    const int got = swr_convert((SwrContext *)swrHw_, dst, kMax, nullptr, 0);
                    if (got > 0) {
                        pending_.resize((size_t)got * outChannels * 2);
                        hwEosIdle_ = 0;
                        return 0;
                    }
                    pending_.clear();
                }
                const bool shortTail = hwExpectedSamples_ > 0 &&
                                       hw_->outSamples() + 1024 < hwExpectedSamples_;
                if (shortTail && hwEosIdle_ < limit + 400) {
                    // 尾部还没吐完：继续等最多 400ms，别急着截断
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                eof_ = true;
                OH_LOG_INFO(LOG_APP,
                            "[decoder] route=%{public}s eof(idle %{public}dms eos=%{public}d) pushed=%{public}lld inBytes=%{public}lld outSamples=%{public}lld expect~%{public}lld%{public}s",
                            route_.c_str(), hwEosIdle_, hwGotEos_ ? 1 : 0, hw_->pushedPackets(),
                            hw_->pushedBytes(), hw_->outSamples(), hwExpectedSamples_,
                            shortTail ? " SHORT" : "");
                return AVERROR_EOF;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        hwStage_.store(2);
        int r = av_read_frame(fmt, pkt);
        hwStage_.store(0);
        if (r == AVERROR_EOF) {
            hwStage_.store(3);
            int er = -1;
            // EOS 推送偶尔也会瞬时返回 INVALID_VAL，重试几次再决定怎么收尾
            for (int attempt = 0; attempt < 10; ++attempt) {
                why.clear();
                er = hw_->SendEos(&why);
                if (er >= 0) break;
                if (why.find("INVALID_VAL") == std::string::npos) break;
                if (stopRequested_.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            hwStage_.store(0);
            if (er == 1) {
                if (++hwEosTry_ >= 100) {
                    // 送不进 EOS 就不要死磕：跳过 EOS，靠空闲超时正常收尾
                    OH_LOG_WARN(LOG_APP, "[decoder] skip EOS after %{public}d retries", hwEosTry_);
                    hwEosSent_ = true;
                    hwEosIdle_ = 0;
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;  // 输入缓冲满，先拉输出，下一轮重试
            }
            if (er < 0) {
                if (why.find("INVALID_VAL") != std::string::npos) {
                    // EOS 被拒不是解码故障：跳过 EOS，继续把尾部输出拉干净
                    OH_LOG_WARN(LOG_APP,
                                "[decoder] EOS rejected (INVALID_VAL), drain without EOS");
                    hwEosSent_ = true;
                    hwEosIdle_ = 0;
                    hwEosTry_ = 0;
                    continue;
                }
                FallbackToSoftware(why);
                return StreamFillPending(outRate, outChannels);
            }
            hwEosSent_ = true;
            hwEosIdle_ = 0;
            hwEosTry_ = 0;
            continue;
        }
        if (r < 0) {
            SetError("av_read_frame(hw): " + AvErr(r));
            return r;
        }
        if (pkt->stream_index != streamIndex_) {
            av_packet_unref(pkt);
            continue;
        }

        BuildElementary(pkt);
        AVStream *st = fmt->streams[streamIndex_];
        const int64_t ptsUs = (pkt->pts != AV_NOPTS_VALUE)
                                  ? av_rescale_q(pkt->pts, st->time_base, AVRational{1, 1000000})
                                  : hwLastPtsUs_;
        hwStage_.store(4);
        // 刚 Start 的 codec 偶尔会瞬时返回 INVALID_VAL，重试一小会儿再判失败
        int push = -1;
        for (int attempt = 0; attempt < 200; ++attempt) {
            why.clear();
            push = hw_->Push(adts_.data(), (int)adts_.size(), ptsUs, &why);
            if (push >= 0) break;
            if (why.find("INVALID_VAL") == std::string::npos) break;
            if (stopRequested_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        hwStage_.store(0);
        if (push == 0) {
            hwLastPtsUs_ = ptsUs;
            av_packet_unref(pkt);
            continue;
        }
        if (push == 1) {
            av_packet_unref(pkt);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        // push < 0：回退前先把当前包交给 FFmpeg 软解，避免丢这一帧
        {
            AVCodecContext *dec = (AVCodecContext *)dec_;
            if (dec != nullptr) {
                const int sr = avcodec_send_packet(dec, pkt);
                OH_LOG_WARN(LOG_APP,
                            "[decoder] fallback recover packet bytes=%{public}d send_packet=%{public}d",
                            pkt->size, sr);
            }
        }
        av_packet_unref(pkt);
        FallbackToSoftware(why);
        return StreamFillPending(outRate, outChannels);
    }
}
