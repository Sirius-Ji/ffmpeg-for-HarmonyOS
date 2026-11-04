#include "transcoder.h"

#include <cstdio>
#include <cstring>
#include <sstream>

#include <hilog/log.h>

#include "fdio.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xFF01
#define LOG_TAG "FFM1"

namespace {

constexpr int kOutRate = 48000;
constexpr int kOutChannels = 2;

std::string AvErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

// 输出档位：编码器 / 封装器 / 码率 / 是否实验性
struct Profile {
    const char *name;
    enum AVCodecID codecId;
    const char *muxer;
    const char *container;
    int bitrate;
    bool experimental;
};

bool PickProfile(const std::string &format, Profile *p) {
    if (format == "aac") {
        *p = {"aac", AV_CODEC_ID_AAC, "mp4", "mp4/m4a", 128000, false};
        return true;
    }
    if (format == "mp3") {
        *p = {"mp3", AV_CODEC_ID_MP3, "mp3", "mp3", 192000, false};
        return true;
    }
    if (format == "opus") {
        *p = {"opus", AV_CODEC_ID_OPUS, "opus", "opus/ogg", 96000, true};
        return true;
    }
    return false;
}

// FFmpeg n9.0 不再暴露 AVCodec::sample_fmts，改成查询 API
enum AVSampleFormat PickSampleFormat(const AVCodecContext *ctx, const AVCodec *codec) {
    const void *cfgs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfgs, &count) >= 0 &&
        cfgs != nullptr && count > 0) {
        return static_cast<const enum AVSampleFormat *>(cfgs)[0];
    }
    return AV_SAMPLE_FMT_FLTP;
}

// 所有资源集中在这里，析构时按依赖顺序释放
struct Ctx {
    int inFd = -1;
    int outFd = -1;
    AVFormatContext *in = nullptr;
    AVFormatContext *out = nullptr;
    AVIOContext *inAvio = nullptr;
    AVIOContext *outAvio = nullptr;
    AVCodecContext *dec = nullptr;
    AVCodecContext *enc = nullptr;
    SwrContext *swr = nullptr;
    AVFrame *frame = nullptr;     // 解码输出
    AVFrame *encFrame = nullptr;  // 编码输入（48k/2ch/FLTP）
    AVFrame *staging = nullptr;   // 按编码器 frame_size 切片的累积帧
    AVPacket *pkt = nullptr;

    ~Ctx() {
        if (pkt) av_packet_free(&pkt);
        if (staging) av_frame_free(&staging);
        if (encFrame) av_frame_free(&encFrame);
        if (frame) av_frame_free(&frame);
        if (swr) swr_free(&swr);
        if (enc) avcodec_free_context(&enc);
        if (dec) avcodec_free_context(&dec);
        if (in) avformat_close_input(&in);
        if (inAvio) {
            av_freep(&inAvio->buffer);
            avio_context_free(&inAvio);
        }
        if (outAvio) {
            av_freep(&outAvio->buffer);
            avio_context_free(&outAvio);
        }
        if (out) avformat_free_context(out);
        if (inFd >= 0) ::close(inFd);
        if (outFd >= 0) ::close(outFd);
    }
};

}  // namespace

int TranscodeAudio(int srcFd, int dstFd, const std::string &format, std::string *summary, std::string *err) {
    auto fail = [&](const std::string &what) {
        if (err) *err = what;
        OH_LOG_ERROR(LOG_APP, "[transcode] %{public}s", what.c_str());
        return -1;
    };

    Profile prof{};
    if (!PickProfile(format, &prof)) return fail("unsupported format: " + format);
    OH_LOG_INFO(LOG_APP, "[transcode] format=%{public}s muxer=%{public}s bitrate=%{public}d",
                prof.name, prof.muxer, prof.bitrate);

    Ctx c;
    c.inFd = ::dup(srcFd);
    c.outFd = ::dup(dstFd);
    if (c.inFd < 0 || c.outFd < 0) return fail("dup(fd) failed");

    // ---------- 输入：fd → AVIOContext ----------
    constexpr int kIoBuf = 64 * 1024;
    auto *inBuf = static_cast<unsigned char *>(av_malloc(kIoBuf));
    if (inBuf == nullptr) return fail("av_malloc(in) failed");
    c.inAvio = avio_alloc_context(inBuf, kIoBuf, 0, (void *)(intptr_t)c.inFd,
                                 ffio::ReadPacket, nullptr, ffio::Seek);
    if (c.inAvio == nullptr) return fail("avio_alloc_context(in) failed");

    c.in = avformat_alloc_context();
    if (c.in == nullptr) return fail("avformat_alloc_context(in) failed");
    c.in->pb = c.inAvio;
    c.in->flags |= AVFMT_FLAG_CUSTOM_IO;

    int ret = avformat_open_input(&c.in, nullptr, nullptr, nullptr);
    if (ret < 0) return fail("avformat_open_input: " + AvErr(ret));
    ret = avformat_find_stream_info(c.in, nullptr);
    if (ret < 0) return fail("avformat_find_stream_info: " + AvErr(ret));

    const int streamIndex = av_find_best_stream(c.in, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) return fail("no audio stream");
    AVStream *ist = c.in->streams[streamIndex];

    const AVCodec *decCodec = avcodec_find_decoder(ist->codecpar->codec_id);
    if (decCodec == nullptr) return fail("decoder not compiled in");
    c.dec = avcodec_alloc_context3(decCodec);
    if (c.dec == nullptr) return fail("avcodec_alloc_context3(dec) failed");
    ret = avcodec_parameters_to_context(c.dec, ist->codecpar);
    if (ret < 0) return fail("avcodec_parameters_to_context: " + AvErr(ret));
    ret = avcodec_open2(c.dec, decCodec, nullptr);
    if (ret < 0) return fail("decoder open: " + AvErr(ret));

    // ---------- 输出：编码器 + 封装器 → fd ----------
    ret = avformat_alloc_output_context2(&c.out, nullptr, prof.muxer, nullptr);
    if (ret < 0 || c.out == nullptr) {
        OH_LOG_ERROR(LOG_APP, "[transcode] alloc_output_context2(%{public}s) failed, ret=%{public}d (%{public}s)",
                     prof.muxer, ret, AvErr(ret).c_str());
        // 把已注册的 muxer 打出来，下次一眼看出到底编进去了什么
        char muxers[1024] = {0};
        size_t off = 0;
        void *iter = nullptr;
        const AVOutputFormat *of = nullptr;
        while ((of = av_muxer_iterate(&iter)) != nullptr && off + 40 < sizeof(muxers)) {
            off += (size_t)snprintf(muxers + off, sizeof(muxers) - off, "%s ", of->name);
        }
        OH_LOG_ERROR(LOG_APP, "[transcode] available muxers: %{public}s", muxers);
        return fail("alloc_output_context2(" + std::string(prof.muxer) + ") failed, ret=" + std::to_string(ret));
    }
    auto *outBuf = static_cast<unsigned char *>(av_malloc(kIoBuf));
    if (outBuf == nullptr) return fail("av_malloc(out) failed");
    c.outAvio = avio_alloc_context(outBuf, kIoBuf, 1, (void *)(intptr_t)c.outFd,
                                  nullptr, ffio::WritePacket, ffio::Seek);
    if (c.outAvio == nullptr) return fail("avio_alloc_context(out) failed");
    c.out->pb = c.outAvio;
    c.out->flags |= AVFMT_FLAG_CUSTOM_IO;

    const AVCodec *encCodec = avcodec_find_encoder(prof.codecId);
    if (encCodec == nullptr) return fail(std::string(prof.name) + " encoder not compiled in");
    c.enc = avcodec_alloc_context3(encCodec);
    if (c.enc == nullptr) return fail("avcodec_alloc_context3(enc) failed");
    c.enc->sample_rate = kOutRate;
    av_channel_layout_default(&c.enc->ch_layout, kOutChannels);
    c.enc->sample_fmt = PickSampleFormat(c.enc, encCodec);
    c.enc->bit_rate = prof.bitrate;
    c.enc->time_base = AVRational{1, kOutRate};
    if (prof.experimental) {
        // Opus 原生编码器标了 AV_CODEC_CAP_EXPERIMENTAL，不设这个会被 avcodec_open2 拒绝
        c.enc->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    }
    if (c.out->oformat->flags & AVFMT_GLOBALHEADER) c.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ret = avcodec_open2(c.enc, encCodec, nullptr);
    if (ret < 0) return fail(std::string(prof.name) + " encoder open: " + AvErr(ret));
    OH_LOG_INFO(LOG_APP,
                "[transcode] encoder=%{public}s fmt=%{public}d frame_size=%{public}d rate=%{public}d ch=%{public}d",
                encCodec->name, (int)c.enc->sample_fmt, c.enc->frame_size, c.enc->sample_rate,
                c.enc->ch_layout.nb_channels);

    AVStream *ost = avformat_new_stream(c.out, nullptr);
    if (ost == nullptr) return fail("avformat_new_stream failed");
    ret = avcodec_parameters_from_context(ost->codecpar, c.enc);
    if (ret < 0) return fail("avcodec_parameters_from_context: " + AvErr(ret));
    ost->time_base = c.enc->time_base;

    ret = avformat_write_header(c.out, nullptr);
    if (ret < 0) return fail("avformat_write_header: " + AvErr(ret));

    // ---------- 重采样：源 → 48k/2ch/FLTP ----------
    ret = swr_alloc_set_opts2(&c.swr, &c.enc->ch_layout, c.enc->sample_fmt, c.enc->sample_rate,
                              &c.dec->ch_layout, c.dec->sample_fmt, c.dec->sample_rate, 0, nullptr);
    if (ret < 0 || c.swr == nullptr) return fail("swr_alloc_set_opts2 failed");
    ret = swr_init(c.swr);
    if (ret < 0) return fail("swr_init: " + AvErr(ret));

    c.frame = av_frame_alloc();
    c.encFrame = av_frame_alloc();
    c.staging = av_frame_alloc();
    c.pkt = av_packet_alloc();
    if (!c.frame || !c.encFrame || !c.staging || !c.pkt) return fail("alloc frame/packet failed");

    // AAC 编码器要求每帧恰好 frame_size 个采样（原生实现是 1024）。
    // swr 一次可能吐 1115（44.1k→48k）甚至几千个采样，直接送会 EINVAL，必须切片。
    const int frameSize = c.enc->frame_size > 0 ? c.enc->frame_size : 1024;
    OH_LOG_INFO(LOG_APP,
                "[transcode] encoder frame_size=%{public}d fmt=%{public}d rate=%{public}d ch=%{public}d",
                frameSize, (int)c.enc->sample_fmt, c.enc->sample_rate, c.enc->ch_layout.nb_channels);

    c.staging->format = c.enc->sample_fmt;
    av_channel_layout_copy(&c.staging->ch_layout, &c.enc->ch_layout);
    c.staging->sample_rate = c.enc->sample_rate;
    c.staging->nb_samples = frameSize;
    ret = av_frame_get_buffer(c.staging, 0);
    if (ret < 0) return fail("staging frame alloc: " + AvErr(ret));

    int64_t outPts = 0;
    int64_t totalFrames = 0;
    int64_t totalPackets = 0;
    int64_t sentFrames = 0;
    int pending = 0;

    auto drainEncoder = [&]() -> int {
        for (;;) {
            int r = avcodec_receive_packet(c.enc, c.pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return 0;
            if (r < 0) return r;
            av_packet_rescale_ts(c.pkt, c.enc->time_base, ost->time_base);
            c.pkt->stream_index = ost->index;
            r = av_interleaved_write_frame(c.out, c.pkt);
            av_packet_unref(c.pkt);
            if (r < 0) return r;
            totalPackets++;
        }
    };

    auto encodeFrame = [&](AVFrame *f) -> int {
        const int r = avcodec_send_frame(c.enc, f);
        if (r < 0) {
            OH_LOG_ERROR(LOG_APP,
                         "[transcode] send_frame ret=%{public}d (%{public}s) nb=%{public}d fmt=%{public}d pts=%{public}lld",
                         r, AvErr(r).c_str(), f ? f->nb_samples : -1, f ? f->format : -1,
                         f ? (long long)f->pts : -1);
            return r;
        }
        return drainEncoder();
    };

    // 把累积帧送进编码器；nb_samples 可能小于 frame_size（最后一帧）
    auto sendStaging = [&]() -> int {
        if (pending <= 0) return 0;
        c.staging->nb_samples = pending;
        c.staging->pts = outPts;
        const int64_t pts = outPts;
        outPts += pending;
        const int nb = pending;
        pending = 0;
        const int r = encodeFrame(c.staging);
        if (sentFrames < 3) {
            OH_LOG_INFO(LOG_APP, "[transcode] send frame nb=%{public}d pts=%{public}lld", nb, (long long)pts);
        }
        sentFrames++;
        if (r < 0) return r;
        // 编码器可能持有这一帧的引用，下一轮写入前确保缓冲区独占
        return av_frame_make_writable(c.staging);
    };

    auto pushSamples = [&](const uint8_t *const *data, int n) -> int {
        int off = 0;
        while (off < n) {
            const int room = frameSize - pending;
            const int take = (n - off) < room ? (n - off) : room;
            const int r = av_samples_copy(c.staging->extended_data,
                                          const_cast<uint8_t *const *>(data), pending, off, take,
                                          kOutChannels, (AVSampleFormat)c.enc->sample_fmt);
            if (r < 0) return r;
            pending += take;
            off += take;
            if (pending == frameSize) {
                const int e = sendStaging();
                if (e < 0) return e;
            }
        }
        return 0;
    };

    auto convertAndEncode = [&](AVFrame *f) -> int {
        const int maxOut = (int)av_rescale_rnd(
            swr_get_delay(c.swr, c.dec->sample_rate) + f->nb_samples,
            c.enc->sample_rate, c.dec->sample_rate, AV_ROUND_UP);
        av_frame_unref(c.encFrame);
        c.encFrame->format = c.enc->sample_fmt;
        av_channel_layout_copy(&c.encFrame->ch_layout, &c.enc->ch_layout);
        c.encFrame->sample_rate = c.enc->sample_rate;
        c.encFrame->nb_samples = maxOut;
        int r = av_frame_get_buffer(c.encFrame, 0);
        if (r < 0) return r;
        const int got = swr_convert(c.swr, c.encFrame->extended_data, maxOut,
                                    (const uint8_t **)f->extended_data, f->nb_samples);
        if (got < 0) return got;
        if (got == 0) return 0;
        return pushSamples((const uint8_t *const *)c.encFrame->extended_data, got);
    };

    int64_t decodedFrames = 0;
    while ((ret = av_read_frame(c.in, c.pkt)) >= 0) {
        if (c.pkt->stream_index != streamIndex) {
            av_packet_unref(c.pkt);
            continue;
        }
        ret = avcodec_send_packet(c.dec, c.pkt);
        av_packet_unref(c.pkt);
        if (ret < 0) return fail("decode send_packet: " + AvErr(ret));
        for (;;) {
            ret = avcodec_receive_frame(c.dec, c.frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) return fail("decode receive_frame: " + AvErr(ret));
            decodedFrames++;
            ret = convertAndEncode(c.frame);
            av_frame_unref(c.frame);
            if (ret < 0) return fail("encode frame: " + AvErr(ret));
            totalFrames = outPts;
        }
    }

    // flush 解码器
    if (ret != AVERROR_EOF) return fail("read_frame: " + AvErr(ret));
    avcodec_send_packet(c.dec, nullptr);
    for (;;) {
        ret = avcodec_receive_frame(c.dec, c.frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return fail("decode flush: " + AvErr(ret));
        decodedFrames++;
        ret = convertAndEncode(c.frame);
        av_frame_unref(c.frame);
        if (ret < 0) return fail("encode flush frame: " + AvErr(ret));
        totalFrames = outPts;
    }

    // flush 重采样器
    for (;;) {
        av_frame_unref(c.encFrame);
        c.encFrame->format = c.enc->sample_fmt;
        av_channel_layout_copy(&c.encFrame->ch_layout, &c.enc->ch_layout);
        c.encFrame->sample_rate = c.enc->sample_rate;
        c.encFrame->nb_samples = 4096;
        ret = av_frame_get_buffer(c.encFrame, 0);
        if (ret < 0) return fail("flush frame alloc failed");
        const int got = swr_convert(c.swr, c.encFrame->extended_data, 4096, nullptr, 0);
        if (got < 0) return fail("swr flush: " + AvErr(got));
        if (got == 0) break;
        ret = pushSamples((const uint8_t *const *)c.encFrame->extended_data, got);
        if (ret < 0) return fail("encode swr flush: " + AvErr(ret));
    }

    // 最后一帧可能不满 frame_size；三个编码器都带 SMALL_LAST_FRAME，直接送
    ret = sendStaging();
    if (ret < 0) return fail("encode last frame: " + AvErr(ret));
    totalFrames = outPts;

    // flush 编码器
    ret = encodeFrame(nullptr);
    if (ret < 0) return fail("encoder flush: " + AvErr(ret));

    ret = av_write_trailer(c.out);
    if (ret < 0) return fail("av_write_trailer: " + AvErr(ret));
    avio_flush(c.outAvio);

    const int64_t bytes = c.outAvio->pos;
    const int64_t durationMs = totalFrames * 1000 / kOutRate;

    std::ostringstream os;
    os << "{\"input\":{\"codec\":\"" << decCodec->name << "\",\"sampleRate\":" << c.dec->sample_rate
       << ",\"channels\":" << c.dec->ch_layout.nb_channels << "}"
       << ",\"output\":{\"format\":\"" << prof.name << "\",\"codec\":\"" << encCodec->name
       << "\",\"container\":\"" << prof.container << "\",\"sampleRate\":" << kOutRate
       << ",\"channels\":" << kOutChannels << ",\"bitRate\":" << prof.bitrate << "}"
       << ",\"decodedFrames\":" << decodedFrames << ",\"outFrames\":" << totalFrames
       << ",\"packets\":" << totalPackets << ",\"bytes\":" << bytes
       << ",\"durationMs\":" << durationMs << "}";
    if (summary) *summary = os.str();

    OH_LOG_INFO(LOG_APP,
                "[transcode] done format=%{public}s codec=%{public}s in=%{public}d/%{public}d out=%{public}d/%{public}d bytes=%{public}lld frames=%{public}lld",
                prof.name, encCodec->name, c.dec->sample_rate, c.dec->ch_layout.nb_channels,
                kOutRate, kOutChannels, (long long)bytes, (long long)totalFrames);
    return 0;
}

// 兼容旧调用：默认转 AAC
int TranscodeToAac(int srcFd, int dstFd, std::string *summary, std::string *err) {
    return TranscodeAudio(srcFd, dstFd, "aac", summary, err);
}
