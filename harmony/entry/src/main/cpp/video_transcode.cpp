#include "video_transcode.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <vector>

#include <hilog/log.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xFF01
#define LOG_TAG "FFM1"

namespace {
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
static bool IsStart3(const uint8_t *d, int n, int i) {
    return i + 3 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1;
}
static bool IsStart4(const uint8_t *d, int n, int i) {
    return i + 4 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1;
}
static int NextStart(const uint8_t *d, int n, int from) {
    for (int i = from; i + 3 <= n; ++i)
        if (IsStart3(d, n, i) || IsStart4(d, n, i)) return i;
    return n;
}
static bool ParseAnnexBParams(const uint8_t *d, int n, std::vector<uint8_t> &sps,
                              std::vector<uint8_t> &pps) {
    int i = 0;
    while (i + 3 <= n) {
        int sc = NextStart(d, n, i);
        if (sc >= n) break;
        const int sl = IsStart4(d, n, sc) ? 4 : 3;
        const int ns = sc + sl;
        const int ne = NextStart(d, n, ns);
        if (ne <= ns) break;
        if ((d[ns] & 0x1f) == 7) sps.assign(d + ns, d + ne);
        else if ((d[ns] & 0x1f) == 8) pps.assign(d + ns, d + ne);
        i = ne;
    }
    return !sps.empty() && !pps.empty();
}
static bool AnnexBToAvcc(AVPacket *&pkt, std::vector<uint8_t> &sps, std::vector<uint8_t> &pps) {
    const uint8_t *d = pkt->data;
    const int n = pkt->size;
    std::vector<uint8_t> out;
    int i = 0;
    while (i + 3 <= n) {
        int sc = NextStart(d, n, i);
        if (sc >= n) break;
        const int sl = IsStart4(d, n, sc) ? 4 : 3;
        const int ns = sc + sl;
        const int ne = NextStart(d, n, ns);
        if (ne <= ns) break;
        const int type = d[ns] & 0x1f;
        if (type == 7) sps.assign(d + ns, d + ne);
        else if (type == 8) pps.assign(d + ns, d + ne);
        else {
            const uint32_t len = (uint32_t)(ne - ns);
            out.push_back((uint8_t)(len >> 24));
            out.push_back((uint8_t)(len >> 16));
            out.push_back((uint8_t)(len >> 8));
            out.push_back((uint8_t)len);
            out.insert(out.end(), d + ns, d + ne);
        }
        i = ne;
    }
    if (out.empty()) return false;
    AVPacket *np = av_packet_alloc();
    av_new_packet(np, (int)out.size());
    memcpy(np->data, out.data(), out.size());
    np->pts = pkt->pts;
    np->dts = pkt->dts;
    np->flags = pkt->flags;
    av_packet_free(&pkt);
    pkt = np;
    return true;
}
static std::vector<uint8_t> BuildAvcC(const std::vector<uint8_t> &sps,
                                      const std::vector<uint8_t> &pps) {
    std::vector<uint8_t> ed;
    if (sps.size() < 4 || pps.empty()) return ed;
    ed.push_back(1);
    ed.push_back(sps[1]);
    ed.push_back(sps[2]);
    ed.push_back(sps[3]);
    ed.push_back(0xFF);
    ed.push_back(0xE1);
    ed.push_back((uint8_t)(sps.size() >> 8));
    ed.push_back((uint8_t)sps.size());
    ed.insert(ed.end(), sps.begin(), sps.end());
    ed.push_back(1);
    ed.push_back((uint8_t)(pps.size() >> 8));
    ed.push_back((uint8_t)pps.size());
    ed.insert(ed.end(), pps.begin(), pps.end());
    return ed;
}
}  // namespace

std::string VideoTranscodeStatusJson() {
    std::lock_guard<std::mutex> lock(gStatusMutex);
    return gStatusJson;
}

int VideoTranscodeToMp4(const std::string &srcPath, const std::string &dstPath,
                        std::string *result) {
    SetStatus(1, "{\"state\":1}");
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
            const AVCodec *d = avcodec_find_decoder(par->codec_id);
            vdec = avcodec_alloc_context3(d);
            if (!d || !vdec || avcodec_parameters_to_context(vdec, par) < 0 ||
                avcodec_open2(vdec, d, nullptr) < 0) failed = 3;
            const AVCodec *e = avcodec_find_encoder_by_name("h264_ohcodec");
            if (!failed && !e) failed = 4;
            if (!failed) {
                venc = avcodec_alloc_context3(e);
                venc->width = 1280;
                venc->height = 720;
                venc->time_base = AVRational{1, 24};
                venc->framerate = AVRational{24, 1};
                venc->pix_fmt = AV_PIX_FMT_NV12;
                venc->bit_rate = 4000000;
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
                nv->width = 1280;
                nv->height = 720;
                if (av_frame_get_buffer(nv, 32) < 0) failed = 6;
            }
            if (!failed) {
                pkt = av_packet_alloc();
                while (av_read_frame(in, pkt) >= 0) {
                    if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }
                    if (avcodec_send_packet(vdec, pkt) == 0) {
                        while (avcodec_receive_frame(vdec, frame) == 0) {
                            av_frame_make_writable(nv);
                            ScaleToNv12(frame, nv);
                            nv->pts = frames++;
                            if (avcodec_send_frame(venc, nv) == 0) {
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

        // ---- mux ----
        if (!failed) {
            OH_LOG_INFO(LOG_APP, "[vtrans] stage=10 mux packets=%{public}d", (int)packets.size());
            // OH 编码器输出 AnnexB；mp4 需要 AVCC + avcC extradata
            std::vector<uint8_t> sps, pps;
            for (auto &p : packets) {
                if (p.stream == 0) AnnexBToAvcc(p.pkt, sps, pps);
            }
            if ((sps.empty() || pps.empty()) && venc && venc->extradata)
                ParseAnnexBParams(venc->extradata, venc->extradata_size, sps, pps);
            std::vector<uint8_t> avcc = BuildAvcC(sps, pps);
            OH_LOG_INFO(LOG_APP, "[vtrans] stage=11 avcc extradata=%{public}d",
                        (int)avcc.size());
            if (avformat_alloc_output_context2(&out, nullptr, "mp4", dstPath.c_str()) < 0) failed = 11;
            AVStream *vst = nullptr, *ast = nullptr;
            if (!failed && venc) {
                vst = avformat_new_stream(out, nullptr);
                avcodec_parameters_from_context(vst->codecpar, venc);
                vst->time_base = AVRational{1, 90000};
                vst->avg_frame_rate = AVRational{24, 1};
                vst->r_frame_rate = AVRational{24, 1};
                if (!avcc.empty()) {
                    vst->codecpar->extradata = (uint8_t *)av_mallocz(avcc.size() + AV_INPUT_BUFFER_PADDING_SIZE);
                    if (vst->codecpar->extradata) {
                        memcpy(vst->codecpar->extradata, avcc.data(), avcc.size());
                        vst->codecpar->extradata_size = (int)avcc.size();
                    }
                }
            }
            if (!failed && aenc) {
                ast = avformat_new_stream(out, nullptr);
                avcodec_parameters_from_context(ast->codecpar, aenc);
                ast->time_base = aenc->time_base;
            }
            if (!failed && (avio_open(&out->pb, dstPath.c_str(), AVIO_FLAG_WRITE) < 0 ||
                            avformat_write_header(out, nullptr) < 0)) failed = 12;
            if (!failed) {
                std::sort(packets.begin(), packets.end(),
                          [](const OutPkt &a, const OutPkt &b) { return a.when < b.when; });
                for (auto &p : packets) {
                    AVStream *st = p.stream == 0 ? vst : ast;
                    if (!st) { av_packet_free(&p.pkt); continue; }
                    av_packet_rescale_ts(p.pkt, p.tb, st->time_base);
                    p.pkt->stream_index = st->index;
                    p.pkt->pos = -1;
                    av_interleaved_write_frame(out, p.pkt);
                    av_packet_free(&p.pkt);
                }
                packets.clear();
                av_write_trailer(out);
            }
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
}static bool IsStart3(const uint8_t *d, int n, int i) {
    return i + 3 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1;
}
static bool IsStart4(const uint8_t *d, int n, int i) {
    return i + 4 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1;
}