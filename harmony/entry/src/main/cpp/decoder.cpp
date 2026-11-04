#include "decoder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
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
    if (swrStream_) { swr_free((SwrContext **)&swrStream_); swrStream_ = nullptr; }
    if (swr_) { swr_free((SwrContext **)&swr_); swr_ = nullptr; }
    if (frame_) { av_frame_free((AVFrame **)&frame_); frame_ = nullptr; }
    if (packet_) { av_packet_free((AVPacket **)&packet_); packet_ = nullptr; }
    if (dec_) { avcodec_free_context((AVCodecContext **)&dec_); dec_ = nullptr; }
    if (fmt_) { avformat_close_input((AVFormatContext **)&fmt_); fmt_ = nullptr; }
    pending_.clear();
    pendingPos_ = 0;
    flushSent_ = false;
    eof_ = false;
    streamIndex_ = -1;
    outChannels_ = 0;
    outSampleRate_ = 0;
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

    ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        SetError("avformat_find_stream_info: " + AvErr(ret));
        return ret;
    }

    const AVCodec *codec = nullptr;
    streamIndex_ = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (streamIndex_ < 0) {
        SetError("no audio stream");
        return streamIndex_;
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
