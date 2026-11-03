/* M0 烟测：像 HAP 里那样 include + link 这套 FFmpeg 产物。 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

const char *ff_smoke_version(void)
{
    return av_version_info();
}

int ff_smoke_probe(const char *path)
{
    AVFormatContext *ctx = NULL;
    int ret = avformat_open_input(&ctx, path, NULL, NULL);
    if (ret == 0)
        avformat_close_input(&ctx);
    return ret;
}

int ff_smoke_oh_video_decoders(void)
{
    const AVCodec *h264 = avcodec_find_decoder_by_name("h264_oh");
    const AVCodec *hevc = avcodec_find_decoder_by_name("hevc_oh");
    return (h264 != NULL) + (hevc != NULL);
}
