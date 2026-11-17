#include "video_transcode.h"

#include <algorithm>
#include <atomic>
#include <climits>
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
#include <libswscale/swscale.h>
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
static AVFrame *RotateLogo(AVFrame *src, int degrees) {
    if (!src || src->format != AV_PIX_FMT_RGBA) return nullptr;
    const double rad = degrees * 3.141592653589793 / 180.0;
    const double c = std::cos(rad), s = std::sin(rad);
    const int w = src->width, h = src->height;
    const int ow = (int)std::ceil(std::fabs(w * c) + std::fabs(h * s));
    const int oh = (int)std::ceil(std::fabs(w * s) + std::fabs(h * c));
    AVFrame *out = av_frame_alloc();
    out->format = AV_PIX_FMT_RGBA;
    out->width = ow;
    out->height = oh;
    if (av_frame_get_buffer(out, 32) < 0) { av_frame_free(&out); return nullptr; }
    const float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
    const float ncx = (ow - 1) * 0.5f, ncy = (oh - 1) * 0.5f;
    const uint8_t *sp = src->data[0];
    uint8_t *dp = out->data[0];
    const int sl = src->linesize[0], dl = out->linesize[0];
    memset(dp, 0, (size_t)dl * oh);
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            const float dx = x - ncx, dy = y - ncy;
            const float sx = c * dx + s * dy + cx;
            const float sy = -s * dx + c * dy + cy;
            if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
            const int ix = (int)(sx + 0.5f), iy = (int)(sy + 0.5f);
            memcpy(dp + y * dl + x * 4, sp + iy * sl + ix * 4, 4);
        }
    }
    OH_LOG_INFO(LOG_APP, "[overlay] rotated logo %dx%d -> %dx%d angle=%{public}d", w, h, ow, oh, degrees);
    return out;
}

static AVFrame *LoadImageToFrame(const std::string &path, std::string *err) {
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        *err = "open failed"; return nullptr;
    }
    avformat_find_stream_info(fmt, nullptr);
    int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (idx < 0) { avformat_close_input(&fmt); *err = "no video stream"; return nullptr; }
    AVCodecParameters *par = fmt->streams[idx]->codecpar;
    const AVCodec *dec = avcodec_find_decoder(par->codec_id);
    AVCodecContext *ctx = dec ? avcodec_alloc_context3(dec) : nullptr;
    int pr = ctx ? avcodec_parameters_to_context(ctx, par) : -1;
    int cr = (ctx && pr >= 0) ? avcodec_open2(ctx, dec, nullptr) : -1;
    if (!ctx || pr < 0 || cr < 0) {
        char eb[128] = {0}; av_strerror(cr, eb, sizeof(eb));
        OH_LOG_ERROR(LOG_APP, "[overlay] codec=%{public}d dec=%{public}p pr=%{public}d cr=%{public}d err=%{public}s",
                     par->codec_id, dec, pr, cr, eb);
        avcodec_free_context(&ctx); avformat_close_input(&fmt); *err = "decoder open failed"; return nullptr;
    }
    AVPacket *pkt = av_packet_alloc();
    AVFrame *pic = av_frame_alloc();
    AVFrame *rgba = nullptr;
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == idx && avcodec_send_packet(ctx, pkt) == 0 &&
            avcodec_receive_frame(ctx, pic) == 0) {
            rgba = av_frame_alloc();
            rgba->format = AV_PIX_FMT_RGBA; rgba->width = pic->width; rgba->height = pic->height;
            if (av_frame_get_buffer(rgba, 32) < 0) { av_frame_free(&rgba); }
            else {
                SwsContext *sws = sws_getContext(pic->width, pic->height, (AVPixelFormat)pic->format,
                                                 pic->width, pic->height, AV_PIX_FMT_RGBA,
                                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (sws) { sws_scale(sws, pic->data, pic->linesize, 0, pic->height, rgba->data, rgba->linesize); sws_freeContext(sws); }
                else { av_frame_free(&rgba); }
            }
            av_packet_unref(pkt); break;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt); av_frame_free(&pic); avcodec_free_context(&ctx); avformat_close_input(&fmt);
    if (rgba) OH_LOG_INFO(LOG_APP, "[overlay] logo loaded w=%{public}d h=%{public}d fmt=%{public}d",
                          rgba->width, rgba->height, rgba->format);
    else *err = "decode failed";
    return rgba;
}
struct VideoPlan {
    bool trim = false;
    double trimStart = 0.0;
    double trimEnd = -1.0;
    bool crop = false;
    int cropW = 0;
    int cropH = 0;
    int cropX = INT_MIN;
    int cropY = INT_MIN;
    bool color = false;
    double brightness = 0.0;
    double contrast = 1.0;
    double saturation = 1.0;
    int temperature = 0;
    std::string textX = "20";
    std::string textY = "20";
    std::string imageX = "W-w-20";
    std::string imageY = "H-h-20";
    int rotate = 0;
    double speed = 1.0;
    double audioTempo = 1.0;
    int freqShift = 0;
    bool echo = false;
    std::string pipPath;
    double pipEnableStart = 2.0;
    double pipEnableEnd = 6.0;
    std::string subtitlePath;
    std::string fontsDir;
};

static bool HasJsonKey(const std::string &j, const char *key) {
    return j.find(std::string("\"") + key + "\"") != std::string::npos;
}
static double JsonNum(const std::string &j, const char *key, double def) {
    size_t p = j.find(std::string("\"") + key + "\"");
    if (p == std::string::npos) return def;
    p = j.find(':', p);
    if (p == std::string::npos) return def;
    return strtod(j.c_str() + p + 1, nullptr);
}
static std::string JsonStr(const std::string &j, const char *key, const std::string &def) {
    size_t p = j.find(std::string("\"") + key + "\"");
    if (p == std::string::npos) return def;
    p = j.find(':', p);
    if (p == std::string::npos) return def;
    p = j.find('"', p);
    if (p == std::string::npos) return def;
    size_t e = j.find('"', p + 1);
    if (e == std::string::npos) return def;
    return j.substr(p + 1, e - p - 1);
}
static VideoPlan ParseVideoPlan(const std::string &j) {
    VideoPlan p;
    if (j.empty()) return p;
    p.trim = HasJsonKey(j, "trimStart") || HasJsonKey(j, "trimEnd");
    p.trimStart = JsonNum(j, "trimStart", 0.0);
    p.trimEnd = JsonNum(j, "trimEnd", -1.0);
    p.crop = JsonNum(j, "cropW", 0) > 0 && JsonNum(j, "cropH", 0) > 0;
    p.cropW = (int)JsonNum(j, "cropW", 0);
    p.cropH = (int)JsonNum(j, "cropH", 0);
    p.cropX = HasJsonKey(j, "cropX") ? (int)JsonNum(j, "cropX", 0) : INT_MIN;
    p.cropY = HasJsonKey(j, "cropY") ? (int)JsonNum(j, "cropY", 0) : INT_MIN;
    p.color = HasJsonKey(j, "brightness") || HasJsonKey(j, "contrast") ||
              HasJsonKey(j, "saturation") || HasJsonKey(j, "temperature");
    p.brightness = JsonNum(j, "brightness", 0.0);
    p.contrast = JsonNum(j, "contrast", 1.0);
    p.saturation = JsonNum(j, "saturation", 1.0);
    p.temperature = (int)JsonNum(j, "temperature", 0);
    p.textX = JsonStr(j, "textX", JsonStr(j, "x", p.textX));
    p.textY = JsonStr(j, "textY", JsonStr(j, "y", p.textY));
    p.imageX = JsonStr(j, "imageX", JsonStr(j, "x", p.imageX));
    p.imageY = JsonStr(j, "imageY", JsonStr(j, "y", p.imageY));
    p.rotate = (int)JsonNum(j, "rotate", 0);
    if (p.rotate < 0) p.rotate = 0;
    if (p.rotate > 360) p.rotate = 360;
    p.speed = JsonNum(j, "speed", 1.0);
    if (p.speed < 0.25) p.speed = 0.25;
    if (p.speed > 4.0) p.speed = 4.0;
    p.audioTempo = JsonNum(j, "atempo", p.speed);
    if (p.audioTempo < 0.25) p.audioTempo = 0.25;
    if (p.audioTempo > 4.0) p.audioTempo = 4.0;
    p.freqShift = (int)JsonNum(j, "freqShift", 0);
    p.echo = HasJsonKey(j, "echo") && JsonNum(j, "echo", 0) != 0;
    p.pipPath = JsonStr(j, "pipPath", "");
    p.pipEnableStart = JsonNum(j, "pipEnableStart", 2.0);
    p.pipEnableEnd = JsonNum(j, "pipEnableEnd", 6.0);
    p.subtitlePath = JsonStr(j, "subtitlePath", "");
    p.fontsDir = JsonStr(j, "fontsDir", "");
    return p;
}

class AudioFilterGraph {
public:
    ~AudioFilterGraph() { if (graph_) avfilter_graph_free(&graph_); }
    bool Init(const VideoPlan &plan, float volume) {
        graph_ = avfilter_graph_alloc();
        if (!graph_) return false;
        if (avfilter_graph_create_filter(&src_, avfilter_get_by_name("abuffer"), "ain",
            "sample_rate=48000:sample_fmt=fltp:channel_layout=stereo:time_base=1/48000",
            nullptr, graph_) < 0) return false;
        AVFilterContext *prev = src_;
        auto add = [&](AVFilterContext **ctx, const char *name, const char *args) -> bool {
            if (avfilter_graph_create_filter(ctx, avfilter_get_by_name(name), name, args, nullptr, graph_) < 0) return false;
            if (avfilter_link(prev, 0, *ctx, 0) < 0) return false;
            prev = *ctx;
            return true;
        };
        if (plan.trim) {
            char a[96];
            if (plan.trimEnd >= 0.0)
                snprintf(a, sizeof(a), "start=%.3f:end=%.3f", plan.trimStart, plan.trimEnd);
            else
                snprintf(a, sizeof(a), "start=%.3f", plan.trimStart);
            if (!add(&trim_, "atrim", a)) return false;
            if (!add(&setpts_, "asetpts", "PTS-STARTPTS")) return false;
        }
        if (plan.audioTempo != 1.0) {
            char a[64];
            snprintf(a, sizeof(a), "tempo=%.5f", plan.audioTempo);
            if (!add(&tempo_, "atempo", a)) return false;
        }
        if (plan.freqShift != 0) {
            char a[64];
            snprintf(a, sizeof(a), "shift=%d:order=8:level=1", plan.freqShift);
            if (!add(&freq_, "afreqshift", a)) return false;
        }
        if (plan.echo) {
            if (!add(&echo_, "aecho", "0.8:0.9:40|60:0.4|0.3")) return false;
        }
        if (volume != 1.0f) {
            char a[32];
            snprintf(a, sizeof(a), "volume=%.3f", volume);
            if (!add(&vol_, "volume", a)) return false;
        }
        if (!add(&limit_, "alimiter", "limit=0.95")) return false;
        if (!add(&fmt_, "aformat", "sample_fmts=flt:sample_rates=48000:channel_layouts=stereo")) return false;
        if (avfilter_graph_create_filter(&sink_, avfilter_get_by_name("abuffersink"), "aout",
                                         nullptr, nullptr, graph_) < 0) return false;
        if (avfilter_link(prev, 0, sink_, 0) < 0) return false;
        if (avfilter_graph_config(graph_, nullptr) < 0) return false;
        ready_ = true;
        return true;
    }
    int Feed(const float *pcm, int frames, std::vector<float> *out) {
        if (!ready_) return -1;
        int off = 0;
        while (off < frames) {
            const int n = std::min(1024, frames - off);
            AVFrame *f = av_frame_alloc();
            f->format = AV_SAMPLE_FMT_FLTP;
            f->sample_rate = 48000;
            av_channel_layout_default(&f->ch_layout, 2);
            f->nb_samples = n;
            if (av_frame_get_buffer(f, 0) < 0) { av_frame_free(&f); return -1; }
            float *l = (float *)f->extended_data[0];
            float *r = (float *)f->extended_data[1];
            for (int i = 0; i < n; ++i) {
                l[i] = pcm[(size_t)(off + i) * 2];
                r[i] = pcm[(size_t)(off + i) * 2 + 1];
            }
            f->pts = off;
            int ret = av_buffersrc_add_frame_flags(src_, f, AV_BUFFERSRC_FLAG_KEEP_REF);
            av_frame_free(&f);
            if (ret < 0) return ret;
            Drain(out);
            off += n;
        }
        return 0;
    }
    int Flush(std::vector<float> *out) {
        if (!ready_) return -1;
        int ret = av_buffersrc_add_frame_flags(src_, nullptr, 0);
        if (ret < 0) return ret;
        return Drain(out, true);
    }
private:
    int Drain(std::vector<float> *out, bool eos = false) {
        for (;;) {
            AVFrame *f = av_frame_alloc();
            int r = av_buffersink_get_frame(sink_, f);
            if (r == AVERROR(EAGAIN)) { av_frame_free(&f); break; }
            if (r == AVERROR_EOF) { av_frame_free(&f); break; }
            if (r < 0) { av_frame_free(&f); return r; }
            const int n = f->nb_samples;
            if (f->format == AV_SAMPLE_FMT_FLT) {
                const float *p = (const float *)f->data[0];
                out->insert(out->end(), p, p + (size_t)n * 2);
            }
            av_frame_free(&f);
            if (eos) continue;
        }
        return 0;
    }
    AVFilterGraph *graph_ = nullptr;
    AVFilterContext *src_ = nullptr, *sink_ = nullptr, *trim_ = nullptr, *setpts_ = nullptr;
    AVFilterContext *tempo_ = nullptr, *freq_ = nullptr, *echo_ = nullptr, *vol_ = nullptr, *limit_ = nullptr, *fmt_ = nullptr;
    bool ready_ = false;
};

class PipVideo {
public:
    ~PipVideo() {
        if (pkt) av_packet_free(&pkt);
        if (rgba) av_frame_free(&rgba);
        if (frame) av_frame_free(&frame);
        if (sws) sws_freeContext(sws);
        if (dec) avcodec_free_context(&dec);
        if (fmt) avformat_close_input(&fmt);
    }
    bool Open(const std::string &path) {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
        vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vidx < 0) return false;
        const AVCodec *c = avcodec_find_decoder(fmt->streams[vidx]->codecpar->codec_id);
        if (!c) return false;
        dec = avcodec_alloc_context3(c);
        if (!dec || avcodec_parameters_to_context(dec, fmt->streams[vidx]->codecpar) < 0 ||
            avcodec_open2(dec, c, nullptr) < 0) return false;
        pipW = dec->width;
        pipH = dec->height;
        frame = av_frame_alloc();
        rgba = av_frame_alloc();
        pkt = av_packet_alloc();
        return frame && rgba && pkt;
    }
    int Next(AVFrame **out) {
        while (!eof) {
            int rr = av_read_frame(fmt, pkt);
            if (rr < 0) { eof = true; break; }
            if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }
            int sr = avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
            if (sr < 0 && sr != AVERROR(EAGAIN)) continue;
            while (avcodec_receive_frame(dec, frame) == 0) {
                if (!rgba->data[0]) {
                    rgba->format = AV_PIX_FMT_RGBA;
                    rgba->width = frame->width;
                    rgba->height = frame->height;
                    if (av_frame_get_buffer(rgba, 32) < 0) return -1;
                    sws = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                         frame->width, frame->height, AV_PIX_FMT_RGBA,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (!sws) return -1;
                }
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                          rgba->data, rgba->linesize);
                rgba->pts = frame->pts;
                *out = rgba;
                return 1;
            }
        }
        if (eof && !flushed) {
            flushed = true;
            avcodec_send_packet(dec, nullptr);
            if (avcodec_receive_frame(dec, frame) == 0) {
                if (!rgba->data[0]) return 0;
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                          rgba->data, rgba->linesize);
                rgba->pts = frame->pts;
                *out = rgba;
                return 1;
            }
        }
        return 0;
    }
    int Width() const { return pipW; }
    int Height() const { return pipH; }
private:
    int pipW = 0, pipH = 0;
    AVFormatContext *fmt = nullptr;
    AVCodecContext *dec = nullptr;
    SwsContext *sws = nullptr;
    AVFrame *frame = nullptr, *rgba = nullptr;
    AVPacket *pkt = nullptr;
    int vidx = -1;
    bool eof = false, flushed = false;
};

class ImageOverlayFilter {
public:
    ~ImageOverlayFilter() {
        if (logoFrame_) av_frame_free(&logoFrame_);
        if (graph_) avfilter_graph_free(&graph_);
    }
    bool Init(int inW, int inH, int outW, int outH, int logoW, int logoH,
              const std::string &fontPath, const std::string &text, AVRational tb,
              const VideoPlan &plan) {
        graph_ = avfilter_graph_alloc();
        if (!graph_) return false;
        char mainArgs[192], logoArgs[192];
        snprintf(mainArgs, sizeof(mainArgs), "video_size=%dx%d:pix_fmt=nv12:time_base=%d/%d:pixel_aspect=1/1", inW, inH, tb.num, tb.den);
        snprintf(logoArgs, sizeof(logoArgs), "video_size=%dx%d:pix_fmt=rgba:time_base=%d/%d:pixel_aspect=1/1", logoW, logoH, tb.num, tb.den);
            const bool hasLogo = logoW > 0 && logoH > 0;
        OH_LOG_INFO(LOG_APP, "[plan] init in=%{public}dx%{public}d out=%{public}dx%{public}d logo=%{public}d trim=%{public}d crop=%{public}d color=%{public}d rotate=%{public}d speed=%.3f textX=%{public}s textY=%{public}s",
                    inW, inH, outW, outH, hasLogo ? 1 : 0, plan.trim ? 1 : 0,
                    plan.crop ? 1 : 0, plan.color ? 1 : 0, plan.rotate, plan.speed,
                    plan.textX.c_str(), plan.textY.c_str());
        if (avfilter_graph_create_filter(&main_, avfilter_get_by_name("buffer"), "main", mainArgs, nullptr, graph_) < 0) {
            OH_LOG_ERROR(LOG_APP, "[plan] create main buffer failed");
            return false;
        }
        if (hasLogo) {
            snprintf(logoArgs, sizeof(logoArgs), "video_size=%dx%d:pix_fmt=rgba:time_base=%d/%d:pixel_aspect=1/1", logoW, logoH, tb.num, tb.den);
            int lr = avfilter_graph_create_filter(&logo_, avfilter_get_by_name("buffer"), "logo", logoArgs, nullptr, graph_);
            if (lr < 0) {
                char eb[128] = {0}; av_strerror(lr, eb, sizeof(eb));
                OH_LOG_ERROR(LOG_APP, "[plan] create logo failed rc=%{public}d err=%{public}s args=%{public}s",
                             lr, eb, logoArgs);
                return false;
            }
        }

        AVFilterContext *prev = main_;
        auto add = [&](AVFilterContext **ctx, const char *name, const char *args) -> bool {
            int cr = avfilter_graph_create_filter(ctx, avfilter_get_by_name(name), name, args, nullptr, graph_);
            if (cr < 0) {
                char eb[128] = {0}; av_strerror(cr, eb, sizeof(eb));
                OH_LOG_ERROR(LOG_APP, "[plan] create %{public}s failed rc=%{public}d err=%{public}s args=%{public}s",
                             name, cr, eb, args);
                return false;
            }
            int lr = avfilter_link(prev, 0, *ctx, 0);
            if (lr < 0) {
                char eb[128] = {0}; av_strerror(lr, eb, sizeof(eb));
                OH_LOG_ERROR(LOG_APP, "[plan] link %{public}s failed rc=%{public}d err=%{public}s", name, lr, eb);
                return false;
            }
            prev = *ctx;
            return true;
        };

        if (plan.trim) {
            OH_LOG_INFO(LOG_APP, "[plan] trim handled in C++ start=%.3f end=%.3f", plan.trimStart, plan.trimEnd);
        }
        if (plan.crop && plan.cropW > 0 && plan.cropH > 0) {
            char ca[160];
            const char *x = plan.cropX == INT_MIN ? "(in_w-out_w)/2" : "cropX";
            const char *y = plan.cropY == INT_MIN ? "(in_h-out_h)/2" : "cropY";
            if (plan.cropX == INT_MIN && plan.cropY == INT_MIN)
                snprintf(ca, sizeof(ca), "w=%d:h=%d:x=%s:y=%s", plan.cropW, plan.cropH, x, y);
            else
                snprintf(ca, sizeof(ca), "w=%d:h=%d:x=%d:y=%d",
                         plan.cropW, plan.cropH,
                         plan.cropX == INT_MIN ? (inW - plan.cropW) / 2 : plan.cropX,
                         plan.cropY == INT_MIN ? (inH - plan.cropH) / 2 : plan.cropY);
            if (!add(&crop_, "crop", ca)) return false;
        }
        char sc[64];
        snprintf(sc, sizeof(sc), "w=%d:h=%d", outW, outH);
        if (!add(&scale_, "scale", sc)) return false;

        if (plan.color) {
            if (plan.brightness != 0.0 || plan.saturation != 1.0) {
                char ha[96];
                snprintf(ha, sizeof(ha), "b=%.4f:s=%.4f", plan.brightness, plan.saturation);
                if (!add(&hue_, "hue", ha)) return false;
            }
            if (plan.contrast != 1.0) {
                const double c = plan.contrast < 0.01 ? 0.01 : plan.contrast;
                double lo = 0.5 - 0.5 / c;
                double hi = 0.5 + 0.5 / c;
                if (lo < 0.0) lo = 0.0;
                if (hi > 1.0) hi = 1.0;
                char la[256];
                snprintf(la, sizeof(la),
                         "rimin=%.4f:gimin=%.4f:bimin=%.4f:rimax=%.4f:gimax=%.4f:bimax=%.4f",
                         lo, lo, lo, hi, hi, hi);
                if (!add(&levels_, "colorlevels", la)) return false;
            }
            if (plan.temperature > 0) {
                char ta[64];
                snprintf(ta, sizeof(ta), "temperature=%d:mix=1", plan.temperature);
                if (!add(&temp_, "colortemperature", ta)) return false;
            }
        }

        if (hasLogo) {
            char oa[256];
            if (!plan.pipPath.empty()) {
                snprintf(oa, sizeof(oa), "x=W-w-24:y=H-h-24:enable=between(t,%.3f,%.3f)",
                         plan.pipEnableStart, plan.pipEnableEnd);
            } else {
                snprintf(oa, sizeof(oa), "x=%s:y=%s", plan.imageX.c_str(), plan.imageY.c_str());
            }
            if (!add(&overlay_, "overlay", oa)) return false;
            AVFilterContext *logoOut = logo_;
            if (!plan.pipPath.empty()) {
                if (avfilter_graph_create_filter(&logoScale_, avfilter_get_by_name("scale"), "pip_scale",
                                                 "w=320:h=180", nullptr, graph_) < 0) return false;
                if (avfilter_link(logo_, 0, logoScale_, 0) < 0) return false;
                logoOut = logoScale_;
            }
            if (avfilter_link(logoOut, 0, overlay_, 1) < 0) return false;
        }

        char sa[768];
        if (!plan.subtitlePath.empty()) {
            if (!plan.fontsDir.empty())
                snprintf(sa, sizeof(sa), "filename=%s:fontsdir=%s:force_style=FontName=Source Han Sans CN",
                         plan.subtitlePath.c_str(), plan.fontsDir.c_str());
            else
                snprintf(sa, sizeof(sa), "filename=%s:force_style=FontName=Source Han Sans CN",
                         plan.subtitlePath.c_str());
            if (!add(&subs_, "subtitles", sa)) return false;
        }

        char dt[512];
        snprintf(dt, sizeof(dt), "fontfile=%s:text='%s':x=%s:y=%s:fontsize=48:fontcolor=white",
                 fontPath.c_str(), EscapeText(text).c_str(), plan.textX.c_str(), plan.textY.c_str());
        if (!add(&draw_, "drawtext", dt)) return false;
        if (!add(&fmt_, "format", "pix_fmts=nv12")) return false;
        if (avfilter_graph_create_filter(&sink_, avfilter_get_by_name("buffersink"), "out",
                                         nullptr, nullptr, graph_) < 0) return false;
        if (avfilter_link(prev, 0, sink_, 0) < 0) return false;
        int gr = avfilter_graph_config(graph_, nullptr);
        if (gr < 0) {
            char eb[128] = {0}; av_strerror(gr, eb, sizeof(eb));
            OH_LOG_ERROR(LOG_APP, "[plan] graph_config failed rc=%{public}d err=%{public}s", gr, eb);
            return false;
        }
        ready_ = true; return true;
    }
    int SendLogo(AVFrame *f) {
        if (!logo_) return -1;
        if (logoFrame_) av_frame_free(&logoFrame_);
        logoFrame_ = av_frame_clone(f);
        logoPts_ = 0;
        return logoFrame_ ? 0 : AVERROR(ENOMEM);
    }
    int Run(AVFrame *in, AVFrame *out) {
        if (!ready_) return -1;
        if (logoFrame_) {
            AVFrame *lf = av_frame_clone(logoFrame_);
            if (!lf) return AVERROR(ENOMEM);
            lf->pts = in->pts;
            int lr = av_buffersrc_add_frame_flags(logo_, lf, AV_BUFFERSRC_FLAG_KEEP_REF);
            av_frame_free(&lf);
            if (lr < 0) return lr;
        }
        int r = av_buffersrc_add_frame_flags(main_, in, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (r < 0) return r;
        int sr = av_buffersink_get_frame(sink_, out);
        return sr;
    }
    int SendEOS() { return av_buffersrc_add_frame_flags(main_, nullptr, 0); }
    int Receive(AVFrame *out) { return av_buffersink_get_frame(sink_, out); }
private:
    AVFilterGraph *graph_ = nullptr;
    AVFilterContext *main_ = nullptr, *logo_ = nullptr, *overlay_ = nullptr, *scale_ = nullptr, *draw_ = nullptr, *fmt_ = nullptr, *sink_ = nullptr;
    AVFilterContext *trim_ = nullptr, *setpts_ = nullptr, *crop_ = nullptr, *hue_ = nullptr, *levels_ = nullptr, *temp_ = nullptr, *rotate_ = nullptr, *logoFmt_ = nullptr, *logoScale_ = nullptr, *subs_ = nullptr;
    AVFrame *logoFrame_ = nullptr;
    int64_t logoPts_ = 0;
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
                        const std::string &container, double speed) {
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
        if (p.stream == 0 && speed > 0.0 && speed != 1.0) {
            const AVRational sp = av_d2q(speed, 1000);
            p.pkt->pts = av_rescale_q(p.pkt->pts, AVRational{sp.den, sp.num}, AVRational{1, 1});
            p.pkt->dts = av_rescale_q(p.pkt->dts, AVRational{sp.den, sp.num}, AVRational{1, 1});
            p.pkt->duration = av_rescale_q(p.pkt->duration, AVRational{sp.den, sp.num}, AVRational{1, 1});
        }
        p.pkt->stream_index = st->index;
        p.pkt->pos = -1;
        if (p.stream == 0) {
            if (p.pkt->dts <= lastVideoDts) p.pkt->dts = lastVideoDts + 1;
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

int ImageProbe(const std::string &path, std::string *result) {
    std::string err;
    AVFrame *f = LoadImageToFrame(path, &err);
    if (!f) { *result = err; return -1; }
    *result = "{\"w\":" + std::to_string(f->width) + ",\"h\":" + std::to_string(f->height) + "}";
    av_frame_free(&f);
    return 0;
}

int VideoTranscode(const std::string &srcPath, const std::string &dstPath,
                   const std::string &format, const std::string &resolution,
                   float volume, const std::string &imagePath, const std::string &optionsJson,
                   std::string *result) {
    SetStatus(1, "{\"state\":1}");
    OH_LOG_INFO(LOG_APP, "[M4.6_BUILD_VER] Native Res=%{public}s scale_enabled=1", resolution.c_str());
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
    VideoPlan plan = ParseVideoPlan(optionsJson);
    OH_LOG_INFO(LOG_APP, "[plan] json=%{public}s trim=%{public}d [%.3f,%.3f] crop=%{public}d %{public}dx%{public}d color=%{public}d b=%.2f c=%.2f s=%.2f temp=%{public}d rotate=%{public}d speed=%.2f atempo=%.2f",
                optionsJson.c_str(), plan.trim ? 1 : 0, plan.trimStart, plan.trimEnd,
                plan.crop ? 1 : 0, plan.cropW, plan.cropH, plan.color ? 1 : 0,
                plan.brightness, plan.contrast, plan.saturation, plan.temperature,
                plan.rotate, plan.speed, plan.audioTempo);
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
            float pcmGain = volume;
            if (!failed) {
                const bool audioPlanActive = plan.trim || plan.audioTempo != 1.0 ||
                                             plan.freqShift != 0 || plan.echo || volume != 1.0f;
                if (audioPlanActive) {
                    AudioFilterGraph ag;
                    if (ag.Init(plan, volume)) {
                        std::vector<float> filtered;
                        ag.Feed(pcm.data(), (int)(pcm.size() / 2), &filtered);
                        ag.Flush(&filtered);
                        pcm.swap(filtered);
                        pcmGain = 1.0f;
                    } else {
                        OH_LOG_ERROR(LOG_APP, "[audio] filter graph init failed, fallback");
                    }
                }
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
                        float lv = pcm[(size_t)(pos + i) * 2] * pcmGain;
                        float rv = pcm[(size_t)(pos + i) * 2 + 1] * pcmGain;
                        l[i] = lv > 1.0f ? 1.0f : (lv < -1.0f ? -1.0f : lv);
                        r[i] = rv > 1.0f ? 1.0f : (rv < -1.0f ? -1.0f : rv);
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
            const int inW = par->width & ~1;
            const int inH = par->height & ~1;
            int outW = inW, outH = inH;
            if (resolution == "720p") { outW = 1280; outH = 720; }
            else if (resolution == "1080p") { outW = 1920; outH = 1080; }
            OH_LOG_INFO(LOG_APP, "[vtrans] output %{public}dx%{public}d res=%{public}s",
                        outW, outH, resolution.c_str());
            const AVCodec *d = avcodec_find_decoder(par->codec_id);
            vdec = avcodec_alloc_context3(d);
            if (!d || !vdec || avcodec_parameters_to_context(vdec, par) < 0 ||
                avcodec_open2(vdec, d, nullptr) < 0) failed = 3;
            const AVCodec *e = avcodec_find_encoder_by_name(hevc ? "hevc_ohcodec" : "h264_ohcodec");
            if (!failed && !e) failed = 4;
            AVRational srcRate = in->streams[vidx]->avg_frame_rate;
            if (srcRate.num <= 0 || srcRate.den <= 0) srcRate = AVRational{24, 1};
            AVRational encRate = srcRate;
            if (plan.speed != 1.0) {
                encRate.num = (int)(srcRate.num * plan.speed + 0.5);
                if (encRate.num < 1) encRate.num = 1;
            }
            if (!failed) {
                venc = avcodec_alloc_context3(e);
                venc->width = outW;
                venc->height = outH;
                venc->time_base = AVRational{srcRate.den, srcRate.num};
                venc->framerate = srcRate;
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
                nv->width = (watermark || !imagePath.empty()) ? inW : outW;
                nv->height = (watermark || !imagePath.empty()) ? inH : outH;
                if (av_frame_get_buffer(nv, 32) < 0) failed = 6;
            }
            if (!failed) {
                pkt = av_packet_alloc();
                const AVRational outTb = AVRational{srcRate.den, srcRate.num};
                const int64_t trimStartPts = plan.trim ? (int64_t)(plan.trimStart * srcRate.num / srcRate.den) : 0;
                const int64_t trimEndPts = (plan.trim && plan.trimEnd >= 0.0)
                    ? (int64_t)(plan.trimEnd * srcRate.num / srcRate.den) : INT64_MAX;
                if (!HasJsonKey(optionsJson, "textX") && !HasJsonKey(optionsJson, "x")) {
                    if (wmPosition == "top-right") { plan.textX = "w-tw-24"; plan.textY = "24"; }
                    else if (wmPosition == "bottom-left") { plan.textX = "24"; plan.textY = "h-th-24"; }
                    else if (wmPosition == "bottom-right") { plan.textX = "w-tw-24"; plan.textY = "h-th-24"; }
                    else if (wmPosition == "center") { plan.textX = "(w-tw)/2"; plan.textY = "(h-th)/2"; }
                    else { plan.textX = "24"; plan.textY = "24"; }
                }
                ImageOverlayFilter img;
                PipVideo pip;
                bool pipOk = false;
                if (!plan.pipPath.empty()) {
                    pipOk = pip.Open(plan.pipPath);
                    OH_LOG_INFO(LOG_APP, "[vtrans] pip open=%{public}d path=%{public}s size=%{public}dx%{public}d",
                                pipOk ? 1 : 0, plan.pipPath.c_str(), pip.Width(), pip.Height());
                }
                AVFrame *logoFrame = nullptr;
                if (!pipOk && !imagePath.empty()) {
                    std::string imgErr;
                    logoFrame = LoadImageToFrame(imagePath, &imgErr);
                    if (logoFrame && plan.rotate != 0) {
                        AVFrame *rot = RotateLogo(logoFrame, plan.rotate);
                        if (rot) { av_frame_free(&logoFrame); logoFrame = rot; }
                    }
                    if (!logoFrame) OH_LOG_ERROR(LOG_APP, "[overlay] image load failed: %{public}s", imgErr.c_str());
                }
                const bool useGraph = watermark || logoFrame != nullptr || pipOk;
                bool wmOk = false;
                if (useGraph) {
                    wmOk = img.Init(inW, inH, outW, outH,
                                    pipOk ? pip.Width() : (logoFrame ? logoFrame->width : 0),
                                    pipOk ? pip.Height() : (logoFrame ? logoFrame->height : 0),
                                    fontPath, wmText, AVRational{1, 1000}, plan);
                    if (wmOk && logoFrame && !pipOk) wmOk = img.SendLogo(logoFrame) >= 0;
                }
                if (logoFrame) av_frame_free(&logoFrame);
                OH_LOG_INFO(LOG_APP, "[vtrans] image=%{public}d text=%{public}d usePlan=%{public}d rotate=%{public}d",
                            imagePath.empty() ? 0 : 1, watermark ? 1 : 0, wmOk ? 1 : 0, plan.rotate);
                if (useGraph && !wmOk) {
                    failed = 20;
                    SetStatus(3, "{\"state\":3,\"code\":\"E_FILTER_INIT_FAILED\"}");
                }
                double speedAcc = 0.0;
                int64_t sendPts = 0;
                auto enqueueVideoPacket = [&]() {
                    OutPkt op;
                    op.pkt = av_packet_clone(pkt);
                    op.stream = 0;
                    const bool bad_ts = (pkt->dts == AV_NOPTS_VALUE || pkt->pts == 0);
                    int64_t stamp = bad_ts ? vpackets : (pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts);
                    if (bad_ts) {
                        op.pkt->pts = stamp;
                        op.pkt->dts = stamp;
                    }
                    op.when = stamp * av_q2d(outTb);
                    op.tb = outTb;
                    packets.push_back(op);
                    vpackets++;
                    av_packet_unref(pkt);
                };
                while (!failed && av_read_frame(in, pkt) >= 0) {
                    if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }
                    if (avcodec_send_packet(vdec, pkt) == 0) {
                        while (avcodec_receive_frame(vdec, frame) == 0) {
                            av_frame_make_writable(nv);
                            ScaleToNv12(frame, nv);
                            nv->pts = frames++;
                            if (plan.trim) {
                                if (nv->pts < trimStartPts || nv->pts >= trimEndPts) continue;
                                nv->pts -= trimStartPts;
                            }
                            nv->pts = (int64_t)(nv->pts * 1000.0 * srcRate.den / srcRate.num + 0.5);
                            AVFrame *filtered = nullptr;
                            AVFrame *sendFrame = nv;
                            if (wmOk) {
                                if (pipOk) {
                                    AVFrame *pf = nullptr;
                                    if (pip.Next(&pf) > 0) {
                                        pf->pts = nv->pts;
                                        img.SendLogo(pf);
                                    }
                                }
                                filtered = av_frame_alloc();
                                int fr = filtered ? img.Run(nv, filtered) : -1;
                                if (fr == 0) {
                                    if (filtered->pts == AV_NOPTS_VALUE) filtered->pts = nv->pts;
                                    sendFrame = filtered;
                                } else if (fr == AVERROR(EAGAIN) || fr == AVERROR_EOF) {
                                    if (filtered) av_frame_free(&filtered);
                                    continue;
                                } else {
                                    if (filtered) av_frame_free(&filtered);
                                    filtered = nullptr;
                                }
                            }
                            if (sendFrame) sendFrame->pts = sendPts;
                            sendPts++;
                            int sfr;
                            for (;;) {
                                sfr = avcodec_send_frame(venc, sendFrame);
                                if (sfr != AVERROR(EAGAIN)) break;
                                while (avcodec_receive_packet(venc, pkt) == 0) {
                                    enqueueVideoPacket();
                                }
                            }
                            if (sfr < 0) {
                                char eb[128] = {0}; av_strerror(sfr, eb, sizeof(eb));
                                OH_LOG_ERROR(LOG_APP, "[plan] send_frame rc=%{public}d err=%{public}s pts=%{public}lld",
                                             sfr, eb, (long long)sendFrame->pts);
                                failed = 21;
                            }
                            if (sfr == 0) {
                                while (avcodec_receive_packet(venc, pkt) == 0) {
                                    enqueueVideoPacket();
                                }
                            }
                            if (filtered) av_frame_free(&filtered);
                        }
                    }
                    av_packet_unref(pkt);
                }
                if (wmOk) {
                    int eosRet = img.SendEOS();
                    OH_LOG_INFO(LOG_APP, "[wm] EOS sent ret=%{public}d", eosRet);
                    int drained = 0;
                    while (true) {
                        AVFrame *of = av_frame_alloc();
                        int r = img.Receive(of);
                        if (r < 0) {
                            OH_LOG_INFO(LOG_APP, "[wm] drain end r=%{public}d frames=%{public}d", r, drained);
                            av_frame_free(&of);
                            break;
                        }
                        of->pts = frames++;
                        if (avcodec_send_frame(venc, of) == 0) {
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
                                op.when = stamp * av_q2d(outTb);
                                op.tb = outTb;
                                packets.push_back(op);
                                vpackets++;
                                av_packet_unref(pkt);
                            }
                        }
                        av_frame_free(&of);
                        drained++;
                    }
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
                    op.when = stamp * av_q2d(outTb);
                    op.tb = outTb;
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
            if (MuxTsThenMp4(packets, venc, aenc, dstPath, container, plan.speed) != 0) failed = 11;
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
