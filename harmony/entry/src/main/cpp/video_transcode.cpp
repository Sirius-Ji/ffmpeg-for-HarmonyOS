#include "video_transcode.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <vector>

#include <hilog/log.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <libavcodec/bsf.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libswresample/swresample.h>
}

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xFF01
#define LOG_TAG "FFM1"

namespace {
static std::string EscapeText(const std::string &in) {
    std::string out;
    for (char c : in) {
        if (c == '\\' || c == '\'' || c == ':') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}
class WatermarkFilter {
public:
    ~WatermarkFilter() { if (graph_) avfilter_graph_free(&graph_); }
    bool Init(const std::string &text, const std::string &position, int fontSize,
              const std::string &colorIn, const std::string &fontPath, int alpha, bool bold,
              int width, int height) {
        graph_ = avfilter_graph_alloc();
        if (!graph_) return false;
        char args[256];
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=nv12:time_base=1/24:pixel_aspect=1/1",
                 width, height);
        if (avfilter_graph_create_filter(&src_, avfilter_get_by_name("buffer"), "in",
                                         args, nullptr, graph_) < 0) return false;
        if (avfilter_graph_create_filter(&sink_, avfilter_get_by_name("buffersink"), "out",
                                         nullptr, nullptr, graph_) < 0) return false;
        std::string x = "24", y = "24";
        if (position == "top-right") { x = "w-tw-24"; }
        else if (position == "bottom-left") { y = "h-th-24"; }
        else if (position == "bottom-right") { x = "w-tw-24"; y = "h-th-24"; }
        else if (position == "center") { x = "(w-tw)/2"; y = "(h-th)/2"; }
        char color[128];
        if (alpha < 100) {
            snprintf(color, sizeof(color), "%s@%.2f", colorIn.c_str(), alpha / 100.0);
        } else {
            snprintf(color, sizeof(color), "%s", colorIn.c_str());
        }
        const char *border = bold ? ":borderw=2:bordercolor=black" : "";
        char dt[2048];
        snprintf(dt, sizeof(dt),
                 "fontfile=%s:text='%s':x=%s:y=%s:fontsize=%d:fontcolor=%s%s",
                 fontPath.c_str(), EscapeText(text).c_str(), x.c_str(), y.c_str(),
                 fontSize, color, border);
        if (avfilter_graph_create_filter(&draw_, avfilter_get_by_name("drawtext"), "draw",
                                         dt, nullptr, graph_) < 0) return false;
        if (avfilter_graph_create_filter(&fmt_, avfilter_get_by_name("format"), "fmt",
                                         "pix_fmts=nv12", nullptr, graph_) < 0) return false;
        if (avfilter_link(src_, 0, draw_, 0) < 0 || avfilter_link(draw_, 0, fmt_, 0) < 0 ||
            avfilter_link(fmt_, 0, sink_, 0) < 0) return false;
        if (avfilter_graph_config(graph_, nullptr) < 0) return false;
        ready_ = true;
        return true;
    }
    int Run(AVFrame *in, AVFrame *out) {
        if (!ready_) return -1;
        int ret = av_buffersrc_add_frame_flags(src_, in, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret < 0) return ret;
        return av_buffersink_get_frame(sink_, out);
    }
private:
    AVFilterGraph *graph_ = nullptr;
    AVFilterContext *src_ = nullptr, *sink_ = nullptr, *draw_ = nullptr, *fmt_ = nullptr;
    bool ready_ = false;
};
struct OutPkt {
    AVPacket *pkt = nullptr;
    int stream = 0;
    double when = 0;
    AVRational tb{1, 1};
};
std::mutex gStatusMutex;
std::string gStatusJson = "{\"state\":0}";

void SetStatus(int state, const std::string &json) {
    std::lock_guard<std::mutex> lock(gStatusMutex);
    gStatusJson = json;
    (void)state;
}

void ScaleToNv12(const AVFrame *src, AVFrame *dst) {
    const int sw = src->width, sh = src->height;
    const int dw = dst->width, dh = dst->height;
    const int ys = dst->linesize[0], us = dst->linesize[1];
    uint8_t *dy = dst->data[0];
    uint8_t *duv = dst->data[1];
    for (int y = 0; y < dh; ++y) {
        const int sy = y * sh / dh;
        const uint8_t *s = src->data[0] + (size_t)sy * src->linesize[0];
        uint8_t *d = dy + (size_t)y * ys;
        for (int x = 0; x < dw; ++x) d[x] = s[x * sw / dw];
    }
    for (int y = 0; y < dh / 2; ++y) {
        const int sy = y * (sh / 2) / (dh / 2);
        const uint8_t *su = src->data[1] + (size_t)sy * src->linesize[1];
        const uint8_t *sv = src->data[2] + (size_t)sy * src->linesize[2];
        uint8_t *d = duv + (size_t)y * us;
        for (int x = 0; x < dw / 2; ++x) {
            const int sx = x * (sw / 2) / (dw / 2);
            d[x * 2] = su[sx];
            d[x * 2 + 1] = sv[sx];
        }
    }
}
static int MuxTsThenMp4(std::vector<OutPkt> &packets, AVCodecContext *venc,
                        AVCodecContext *aenc, const std::string &dstPath,
                        const std::string &container) {
    const std::string tsPath = dstPath + ".ts";
    AVFormatContext *ts = nullptr;
    if (avformat_alloc_output_context2(&ts, nullptr, "mpegts", tsPath.c_str()) < 0) return -1;
    AVStream *vst = nullptr, *ast = nullptr;
    if (venc) {
        vst = avformat_new_stream(ts, nullptr);
        avcodec_parameters_from_context(vst->codecpar, venc);
        vst->time_base = AVRational{1, 90000};  // TS 标准时间基
        vst->codecpar->codec_tag = 0;
        OH_LOG_INFO(LOG_APP, "[vtrans] ts video codec=%{public}d extradata=%{public}d",
                    (int)vst->codecpar->codec_id, vst->codecpar->extradata_size);
        if (venc->extradata && venc->extradata_size > 0 &&
            vst->codecpar->extradata == nullptr) {
            vst->codecpar->extradata = (uint8_t *)av_mallocz(
                venc->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (vst->codecpar->extradata) {
                memcpy(vst->codecpar->extradata, venc->extradata, venc->extradata_size);
                vst->codecpar->extradata_size = venc->extradata_size;
            }
        }
    }
    if (aenc) {
        ast = avformat_new_stream(ts, nullptr);
        avcodec_parameters_from_context(ast->codecpar, aenc);
        ast->time_base = aenc->time_base;
    }
    if (avio_open(&ts->pb, tsPath.c_str(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(ts, nullptr) < 0) {
        avformat_free_context(ts);
        return -2;
    }
    std::sort(packets.begin(), packets.end(),
              [](const OutPkt &a, const OutPkt &b) { return a.when < b.when; });
    int64_t lastVideoDts = -1;
    int videoLog = 0;
    for (auto &p : packets) {
        AVStream *st = p.stream == 0 ? vst : ast;
        if (!st) { av_packet_free(&p.pkt); continue; }
        av_packet_rescale_ts(p.pkt, p.tb, st->time_base);
        p.pkt->stream_index = st->index;
        p.pkt->pos = -1;
        if (p.stream == 0) {
            if (p.pkt->dts <= lastVideoDts)
                OH_LOG_WARN(LOG_APP, "[vtrans] DTS regress last=%{public}lld now=%{public}lld",
                            (long long)lastVideoDts, (long long)p.pkt->dts);
            lastVideoDts = p.pkt->dts;
        }
        int ret = av_interleaved_write_frame(ts, p.pkt);
        if (p.stream == 0 && videoLog < 6) {
            char e[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, e, sizeof(e));
            OH_LOG_INFO(LOG_APP, "[vtrans] ts pkt pts=%{public}lld dts=%{public}lld size=%{public}d key=%{public}d ret=%{public}d %{public}s",
                        (long long)p.pkt->pts, (long long)p.pkt->dts, p.pkt->size,
                        (p.pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0, ret, e);
            videoLog++;
        } else if (ret < 0) {
            char e[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, e, sizeof(e));
            OH_LOG_ERROR(LOG_APP, "[vtrans] ts write ret=%{public}d %{public}s", ret, e);
        }
        av_packet_free(&p.pkt);
    }
    packets.clear();
    av_write_trailer(ts);
    avio_closep(&ts->pb);
    avformat_free_context(ts);

    // remux: TS -> MP4，-c copy 等价，由 FFmpeg 提取 extradata/转 AVCC
    AVFormatContext *in = nullptr, *out = nullptr;
    if (avformat_open_input(&in, tsPath.c_str(), nullptr, nullptr) < 0) return -3;
    if (avformat_find_stream_info(in, nullptr) < 0) { avformat_close_input(&in); return -4; }
    if (avformat_alloc_output_context2(&out, nullptr, container.c_str(), dstPath.c_str()) < 0) {
        avformat_close_input(&in); return -5;
    }
    int vmap[8], amap[8];
    for (int i = 0; i < 8; ++i) { vmap[i] = -1; amap[i] = -1; }
    for (unsigned i = 0; i < in->nb_streams && i < 8; ++i) {
        AVStream *src = in->streams[i];
        if (src->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            src->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        AVStream *dst = avformat_new_stream(out, nullptr);
        avcodec_parameters_copy(dst->codecpar, src->codecpar);
        dst->time_base = src->time_base;
        dst->codecpar->codec_tag = 0;
        if (src->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) vmap[i] = dst->index;
        else amap[i] = dst->index;
    }
    if (avio_open(&out->pb, dstPath.c_str(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(out, nullptr) < 0) {
        avio_closep(&out->pb); avformat_free_context(out); avformat_close_input(&in); return -6;
    }
    AVPacket *pkt = av_packet_alloc();
    int written = 0;
    while (av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index >= 0 && pkt->stream_index < 8 &&
            (vmap[pkt->stream_index] >= 0 || amap[pkt->stream_index] >= 0)) {
            AVStream *src = in->streams[pkt->stream_index];
            AVStream *dst = out->streams[vmap[pkt->stream_index] >= 0
                                             ? vmap[pkt->stream_index] : amap[pkt->stream_index]];
            av_packet_rescale_ts(pkt, src->time_base, dst->time_base);
            pkt->stream_index = dst->index;
            pkt->pos = -1;
            av_interleaved_write_frame(out, pkt);
            written++;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    av_write_trailer(out);
    avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);
    OH_LOG_INFO(LOG_APP, "[vtrans] remux packets=%{public}d", written);
    return written > 0 ? 0 : -7;
}
}  // namespace

std::string VideoTranscodeStatusJson() {
    std::lock_guard<std::mutex> lock(gStatusMutex);
    return gStatusJson;
}

int VideoTranscode(const std::string &srcPath, const std::string &dstPath,
                   const std::string &format, std::string *result) {
    SetStatus(1, "{\"state\":1}");
    const bool watermark = format.rfind("watermark|", 0) == 0;
    std::string wmText = "FFmpeg-ohos", wmPosition = "top-left", wmColor = "white";
    std::string fontPath;
    int wmFontSize = 48;
    int wmAlpha = 100;
    bool wmBold = false;
    if (watermark) {
        std::string rest = format.substr(10);
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            size_t pos = rest.find('|', start);
            parts.push_back(rest.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
            if (pos == std::string::npos) break;
            start = pos + 1;
        }
        if (parts.size() >= 7) {
            wmText = parts[0];
            wmPosition = parts[1];
            wmFontSize = atoi(parts[2].c_str());
            if (wmFontSize < 8) wmFontSize = 8;
            wmColor = parts[3];
            fontPath = parts[4];
            wmAlpha = atoi(parts[5].c_str());
            if (wmAlpha < 0) wmAlpha = 0;
            if (wmAlpha > 100) wmAlpha = 100;
            wmBold = (parts[6] == "1");
        } else if (parts.size() >= 5) {
            wmText = parts[0];
            wmPosition = parts[1];
            wmFontSize = atoi(parts[2].c_str());
            if (wmFontSize < 8) wmFontSize = 8;
            wmColor = parts[3];
            fontPath = parts[4];
        } else {
            fontPath = rest;
        }
    }
    const std::string realFormat = watermark ? "mp4" : format;
    const bool hevc = (realFormat == "hevc" || realFormat == "h265");
    const bool flv = (realFormat == "flv");
    const std::string container = (realFormat == "mkv") ? "matroska" : (flv ? "flv" : "mp4");
    OH_LOG_INFO(LOG_APP, "[vtrans] stage=0 start");
    AVFormatContext *in = nullptr, *out = nullptr;
    AVCodecContext *vdec = nullptr, *venc = nullptr, *adec = nullptr, *aenc = nullptr;
    SwrContext *swr = nullptr;
    AVFrame *frame = nullptr, *nv = nullptr;
    AVPacket *pkt = nullptr;
    std::vector<OutPkt> packets;
    std::vector<float> pcm;
    int frames = 0, vpackets = 0, apackets = 0;
    int failed = 0;
    if (hevc) {
        OH_AVCapability *hw = OH_AVCodec_GetCapabilityByCategory(
            OH_AVCODEC_MIMETYPE_VIDEO_HEVC, true, HARDWARE);
        OH_AVCapability *sw = OH_AVCodec_GetCapabilityByCategory(
            OH_AVCODEC_MIMETYPE_VIDEO_HEVC, true, SOFTWARE);
        OH_LOG_INFO(LOG_APP, "[vtrans] hevc encoder hw=%{public}p sw=%{public}p hwName=%{public}s swName=%{public}s",
                    hw, sw, hw ? OH_AVCapability_GetName(hw) : "null",
                    sw ? OH_AVCapability_GetName(sw) : "null");
        if (!hw && !sw) {
            SetStatus(3, "{\"state\":3,\"code\":\"E_NOT_SUPPORTED\"}");
            if (result) *result = "{\"state\":3,\"code\":\"E_NOT_SUPPORTED\",\"message\":\"HEVC encoder not supported\"}";
            return -1;
        }
    }

    try {
        if (avformat_open_input(&in, srcPath.c_str(), nullptr, nullptr) != 0) failed = 1;
        if (!failed && avformat_find_stream_info(in, nullptr) < 0) failed = 2;
        int vidx = failed ? -1 : av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        int aidx = failed ? -1 : av_find_best_stream(in, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

        // ---- audio ----
        if (!failed && aidx >= 0) {
            OH_LOG_INFO(LOG_APP, "[vtrans] stage=8 audio decode");
            AVCodecParameters *par = in->streams[aidx]->codecpar;
            const AVCodec *d = avcodec_find_decoder(par->codec_id);
            adec = avcodec_alloc_context3(d);
            if (!d || !adec || avcodec_parameters_to_context(adec, par) < 0 ||
                avcodec_open2(adec, d, nullptr) < 0) failed = 8;
            AVChannelLayout inLayout{}, outLayout{};
            if (!failed) {
                av_channel_layout_copy(&inLayout, &adec->ch_layout);
                av_channel_layout_default(&outLayout, 2);
                if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, 48000,
                                        &inLayout, adec->sample_fmt, adec->sample_rate,
                                        0, nullptr) < 0 || swr_init(swr) < 0) failed = 9;
                av_channel_layout_uninit(&inLayout);
                av_channel_layout_uninit(&outLayout);
            }
            if (!failed) {
                frame = av_frame_alloc();
                pkt = av_packet_alloc();
                while (av_read_frame(in, pkt) >= 0) {
                    if (pkt->stream_index != aidx) { av_packet_unref(pkt); continue; }
                    if (avcodec_send_packet(adec, pkt) == 0) {
                        while (avcodec_receive_frame(adec, frame) == 0) {
                            const int maxOut = swr_get_out_samples(swr, frame->nb_samples);
                            const int base = (int)pcm.size();
                            pcm.resize(base + (size_t)maxOut * 2);
                            uint8_t *o[1] = {(uint8_t *)(pcm.data() + base)};
                            const int got = swr_convert(swr, o, maxOut,
                                                        (const uint8_t **)frame->extended_data,
                                                        frame->nb_samples);
                            pcm.resize(base + (got > 0 ? (size_t)got * 2 : 0));
                        }
                    }
                    av_packet_unref(pkt);
                }
                av_frame_free(&frame);
                av_packet_free(&pkt);
            }
            if (!failed) {
                const AVCodec *ae = avcodec_find_encoder(AV_CODEC_ID_AAC);
                aenc = avcodec_alloc_context3(ae);
                aenc->sample_rate = 48000;
                av_channel_layout_default(&aenc->ch_layout, 2);
                aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
                aenc->bit_rate = 128000;
                aenc->time_base = AVRational{1, 48000};
                if (!ae || avcodec_open2(aenc, ae, nullptr) < 0) failed = 10;
            }
            if (!failed) {
                OH_LOG_INFO(LOG_APP, "[vtrans] stage=9 aac encode samples=%{public}d",
                            (int)(pcm.size() / 2));
                const int fs = aenc->frame_size > 0 ? aenc->frame_size : 1024;
                AVFrame *af = av_frame_alloc();
                af->format = aenc->sample_fmt;
                af->sample_rate = 48000;
                av_channel_layout_copy(&af->ch_layout, &aenc->ch_layout);
                af->nb_samples = fs;
                av_frame_get_buffer(af, 0);
                pkt = av_packet_alloc();
                int64_t pos = 0;
                const int total = (int)(pcm.size() / 2);
                while (pos < total) {
                    av_frame_make_writable(af);
                    const int n = std::min<int64_t>(fs, total - pos);
                    float *l = (float *)af->extended_data[0];
                    float *r = (float *)af->extended_data[1];
                    for (int i = 0; i < n; ++i) {
                        l[i] = pcm[(size_t)(pos + i) * 2];
                        r[i] = pcm[(size_t)(pos + i) * 2 + 1];
                    }
                    if (n < fs)
                        for (int ch = 0; ch < 2; ++ch)
                            memset(af->extended_data[ch] + n, 0, (fs - n) * sizeof(float));
                    af->pts = pos;
                    af->nb_samples = fs;
                    if (avcodec_send_frame(aenc, af) == 0) {
                        while (avcodec_receive_packet(aenc, pkt) == 0) {
                            OutPkt op;
                            op.pkt = av_packet_clone(pkt);
                            op.stream = 1;
                            op.when = pkt->pts * av_q2d(aenc->time_base);
                            op.tb = aenc->time_base;
                            packets.push_back(op);
                            apackets++;
                            av_packet_unref(pkt);
                        }
                    }
                    pos += fs;
                }
                av_frame_free(&af);
                av_packet_free(&pkt);
            }
        }

        // ---- video ----
        if (!failed && vidx >= 0) {
            OH_LOG_INFO(LOG_APP, "[vtrans] stage=3 video decode/encode");
            if (av_seek_frame(in, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
                OH_LOG_WARN(LOG_APP, "[vtrans] seek back failed");
            }
            avformat_flush(in);
            AVCodecParameters *par = in->streams[vidx]->codecpar;
            const int outW = par->width & ~1;
            const int outH = par->height & ~1;
            OH_LOG_INFO(LOG_APP, "[vtrans] output %{public}dx%{public}d", outW, outH);
            const AVCodec *d = avcodec_find_decoder(par->codec_id);
            vdec = avcodec_alloc_context3(d);
            if (!d || !vdec || avcodec_parameters_to_context(vdec, par) < 0 ||
                avcodec_open2(vdec, d, nullptr) < 0) failed = 3;
            const AVCodec *e = avcodec_find_encoder_by_name(hevc ? "hevc_ohcodec" : "h264_ohcodec");
            if (!failed && !e) failed = 4;
            if (!failed) {
                venc = avcodec_alloc_context3(e);
                venc->width = outW;
                venc->height = outH;
                venc->time_base = AVRational{1, 24};
                venc->framerate = AVRational{24, 1};
                venc->pix_fmt = AV_PIX_FMT_NV12;
                venc->bit_rate = (outW >= 1920) ? 8000000 : 4000000;
                venc->gop_size = 48;
                venc->max_b_frames = 0;
                AVDictionary *opts = nullptr;
                av_dict_set(&opts, "allow_sw", "1", 0);
                int er = avcodec_open2(venc, e, &opts);
                av_dict_free(&opts);
                if (er < 0) failed = 5;
            }
            if (!failed) {
                frame = av_frame_alloc();
                nv = av_frame_alloc();
                nv->format = AV_PIX_FMT_NV12;
                nv->width = outW;
                nv->height = outH;
                if (av_frame_get_buffer(nv, 32) < 0) failed = 6;
            }
            if (!failed) {
                pkt = av_packet_alloc();
                WatermarkFilter wm;
                const bool wmOk = watermark && wm.Init(wmText, wmPosition, wmFontSize, wmColor, fontPath, wmAlpha, wmBold, outW, outH);
                if (watermark)
                    OH_LOG_INFO(LOG_APP, "[vtrans] watermark=%{public}d font=%{public}s",
                                wmOk ? 1 : 0, fontPath.c_str());
                while (av_read_frame(in, pkt) >= 0) {
                    if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }
                    if (avcodec_send_packet(vdec, pkt) == 0) {
                        while (avcodec_receive_frame(vdec, frame) == 0) {
                            av_frame_make_writable(nv);
                            ScaleToNv12(frame, nv);
                            nv->pts = frames++;
                            AVFrame *filtered = nullptr;
                            AVFrame *sendFrame = nv;
                            if (wmOk) {
                                filtered = av_frame_alloc();
                                if (filtered && wm.Run(nv, filtered) == 0) {
                                    filtered->pts = nv->pts;
                                    sendFrame = filtered;
                                } else {
                                    if (filtered) av_frame_free(&filtered);
                                    filtered = nullptr;
                                }
                            }
                            if (avcodec_send_frame(venc, sendFrame) == 0) {
                                while (avcodec_receive_packet(venc, pkt) == 0) {
                                    OutPkt op;
                                    op.pkt = av_packet_clone(pkt);
                                    op.stream = 0;
                                    const bool bad_ts = (pkt->dts == AV_NOPTS_VALUE || pkt->pts == 0);
                                    int64_t stamp = bad_ts ? vpackets : (pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts);
                                    if (bad_ts) {
                                        op.pkt->pts = stamp;
                                        op.pkt->dts = stamp;
                                    }
                                    op.when = stamp / 24.0;
                                    op.tb = AVRational{1, 24};
                                    if ((vpackets % 24) == 0) {
                                        OH_LOG_INFO(LOG_APP, "[vtrans] pkt=%{public}d pts=%{public}lld dts=%{public}lld size=%{public}d",
                                                    vpackets, (long long)pkt->pts, (long long)pkt->dts,
                                                    pkt->size);
                                    }
                                    packets.push_back(op);
                                    vpackets++;
                                    av_packet_unref(pkt);
                                }
                            }
                            if (filtered) av_frame_free(&filtered);
                        }
                    }
                    av_packet_unref(pkt);
                }
                avcodec_send_frame(venc, nullptr);
                while (avcodec_receive_packet(venc, pkt) == 0) {
                    OutPkt op;
                    op.pkt = av_packet_clone(pkt);
                    op.stream = 0;
                    const bool bad_ts = (pkt->dts == AV_NOPTS_VALUE || pkt->pts == 0);
                    int64_t stamp = bad_ts ? vpackets : (pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts);
                    if (bad_ts) {
                        op.pkt->pts = stamp;
                        op.pkt->dts = stamp;
                    }
                    op.when = stamp / 24.0;
                    op.tb = AVRational{1, 24};
                    packets.push_back(op);
                    vpackets++;
                    av_packet_unref(pkt);
                }
                OH_LOG_INFO(LOG_APP, "[vtrans] stage=6 video frames=%{public}d packets=%{public}d",
                            frames, vpackets);
            }
        }

        // 手写 AVCC 极易在海思芯片上引发随机花屏，故采用 TS 中转 + FFmpeg 原生 remux。
        // ---- two-stage mux: AnnexB -> MPEG-TS -> MP4 remux ----
        if (!failed) {
            if (MuxTsThenMp4(packets, venc, aenc, dstPath, container) != 0) failed = 11;
        }
        if (out) {
            if (out->pb) avio_closep(&out->pb);
            avformat_free_context(out);
            out = nullptr;
        }
        struct stat st{};
        if (!failed && (stat(dstPath.c_str(), &st) != 0 || st.st_size <= 0)) failed = 13;
        if (!failed) {
            std::ostringstream os;
            os << "{\"state\":2,\"size\":" << (long long)st.st_size
               << ",\"frames\":" << frames << ",\"vpackets\":" << vpackets
               << ",\"apackets\":" << apackets << "}";
            SetStatus(2, os.str());
            if (result) *result = os.str();
            OH_LOG_INFO(LOG_APP, "[vtrans] stage=12 done size=%{public}lld",
                        (long long)st.st_size);
        }
    } catch (...) {
        failed = 99;
        OH_LOG_ERROR(LOG_APP, "[vtrans] stage=99 exception");
    }

    if (frame) av_frame_free(&frame);
    if (nv) av_frame_free(&nv);
    if (pkt) av_packet_free(&pkt);
    if (swr) swr_free(&swr);
    if (vdec) avcodec_free_context(&vdec);
    if (venc) avcodec_free_context(&venc);
    if (adec) avcodec_free_context(&adec);
    if (aenc) avcodec_free_context(&aenc);
    if (in) avformat_close_input(&in);
    for (auto &p : packets) av_packet_free(&p.pkt);

    if (failed) {
        std::ostringstream os;
        os << "{\"state\":3,\"stage\":" << failed << "}";
        SetStatus(3, os.str());
        if (result) *result = os.str();
        OH_LOG_ERROR(LOG_APP, "[vtrans] failed=%{public}d", failed);
        return -1;
    }
    return 0;
}
