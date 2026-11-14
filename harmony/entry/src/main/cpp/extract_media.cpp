#include "extract_media.h"

#include <cstdio>
#include <sstream>
#include <sys/stat.h>

#include <hilog/log.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
}

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xFF01
#define LOG_TAG "FFM1"

namespace {
std::string ContainerFor(const std::string &dst) {
    if (dst.size() >= 4 && dst.substr(dst.size() - 4) == ".mkv") return "matroska";
    if (dst.size() >= 4 && dst.substr(dst.size() - 4) == ".aac") return "adts";
    if (dst.size() >= 4 && dst.substr(dst.size() - 4) == ".mp3") return "adts";  // AAC copy uses ADTS, keep .aac recommended
    return "mp4";
}
}  // namespace

int ExtractMedia(const std::string &srcPath, const std::string &dstPath,
                 const std::string &type, std::string *result) {
    AVFormatContext *in = nullptr, *out = nullptr;
    AVPacket *pkt = nullptr;
    int failed = 0;
    do {
        if (avformat_open_input(&in, srcPath.c_str(), nullptr, nullptr) != 0) { failed = 1; break; }
        if (avformat_find_stream_info(in, nullptr) < 0) { failed = 2; break; }
        const AVMediaType mt = (type == "audio") ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;
        int idx = av_find_best_stream(in, mt, -1, -1, nullptr, 0);
        if (idx < 0) { failed = 3; break; }

        if (type == "video" && dstPath.size() >= 5 &&
            dstPath.substr(dstPath.size() - 5) == ".h264") {
            const AVBitStreamFilter *f = av_bsf_get_by_name("h264_mp4toannexb");
            AVBSFContext *bsf = nullptr;
            if (!f || av_bsf_alloc(f, &bsf) < 0) { failed = 7; break; }
            avcodec_parameters_copy(bsf->par_in, in->streams[idx]->codecpar);
            if (av_bsf_init(bsf) < 0) { av_bsf_free(&bsf); failed = 8; break; }
            FILE *fout = fopen(dstPath.c_str(), "wb");
            if (!fout) { av_bsf_free(&bsf); failed = 9; break; }
            pkt = av_packet_alloc();
            int written = 0;
            while (av_read_frame(in, pkt) >= 0) {
                if (pkt->stream_index == idx) {
                    if (av_bsf_send_packet(bsf, pkt) == 0) {
                        AVPacket *op = av_packet_alloc();
                        while (av_bsf_receive_packet(bsf, op) == 0) {
                            fwrite(op->data, 1, op->size, fout);
                            written++;
                            av_packet_unref(op);
                        }
                        av_packet_free(&op);
                    }
                }
                av_packet_unref(pkt);
            }
            av_bsf_send_packet(bsf, nullptr);
            AVPacket *op = av_packet_alloc();
            while (av_bsf_receive_packet(bsf, op) == 0) {
                fwrite(op->data, 1, op->size, fout);
                written++;
                av_packet_unref(op);
            }
            av_packet_free(&op);
            fclose(fout);
            av_bsf_free(&bsf);
            struct stat sb{};
            if (stat(dstPath.c_str(), &sb) != 0 || sb.st_size <= 0) { failed = 10; break; }
            std::ostringstream os;
            os << "{\"state\":2,\"size\":" << (long long)sb.st_size << ",\"packets\":" << written
               << ",\"container\":\"h264\"}";
            if (result) *result = os.str();
            OH_LOG_INFO(LOG_APP, "[extract] video->h264 packets=%{public}d size=%{public}lld",
                        written, (long long)sb.st_size);
            break;
        }
        const std::string container = ContainerFor(dstPath);
        if (avformat_alloc_output_context2(&out, nullptr, container.c_str(), dstPath.c_str()) < 0) {
            failed = 4; break;
        }
        AVStream *st = avformat_new_stream(out, nullptr);
        avcodec_parameters_copy(st->codecpar, in->streams[idx]->codecpar);
        st->codecpar->codec_tag = 0;
        st->time_base = in->streams[idx]->time_base;
        if (avio_open(&out->pb, dstPath.c_str(), AVIO_FLAG_WRITE) < 0 ||
            avformat_write_header(out, nullptr) < 0) { failed = 5; break; }

        pkt = av_packet_alloc();
        int written = 0;
        while (av_read_frame(in, pkt) >= 0) {
            if (pkt->stream_index == idx) {
                av_packet_rescale_ts(pkt, in->streams[idx]->time_base, st->time_base);
                pkt->stream_index = st->index;
                pkt->pos = -1;
                if (av_interleaved_write_frame(out, pkt) >= 0) written++;
            }
            av_packet_unref(pkt);
        }
        av_write_trailer(out);
        struct stat sb{};
        if (stat(dstPath.c_str(), &sb) != 0 || sb.st_size <= 0) { failed = 6; break; }
        std::ostringstream os;
        os << "{\"state\":2,\"size\":" << (long long)sb.st_size << ",\"packets\":" << written
           << ",\"container\":\"" << container << "\"}";
        if (result) *result = os.str();
        OH_LOG_INFO(LOG_APP, "[extract] %{public}s -> %{public}s packets=%{public}d size=%{public}lld",
                    type.c_str(), container.c_str(), written, (long long)sb.st_size);
    } while (false);

    if (out) {
        if (out->pb) avio_closep(&out->pb);
        avformat_free_context(out);
    }
    if (in) avformat_close_input(&in);
    if (pkt) av_packet_free(&pkt);
    if (failed) {
        OH_LOG_ERROR(LOG_APP, "[extract] failed stage=%{public}d", failed);
        if (result) *result = "{\"state\":3,\"stage\":" + std::to_string(failed) + "}";
        return -1;
    }
    return 0;
}
