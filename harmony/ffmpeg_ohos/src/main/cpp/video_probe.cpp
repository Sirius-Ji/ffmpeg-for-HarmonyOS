#include "video_probe.h"

#include <sstream>
#include <vector>

#include <hilog/log.h>

extern "C" {
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_averrors.h>
}

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xFF01
#define LOG_TAG "FFM1"

namespace {

struct VideoResult {
    const char *codec = "";
    const char *mime = "";
    bool hasCapability = false;
    std::string capabilityName;
    bool hardware = false;
    int32_t minWidth = 0;
    int32_t maxWidth = 0;
    int32_t minHeight = 0;
    int32_t maxHeight = 0;
    int32_t maxFps = 0;
    bool creatable = false;
    bool surfaceBFrameRisk = false;
};

VideoResult ProbeVideo(const char *codec, const char *mime) {
    VideoResult out;
    out.codec = codec;
    out.mime = mime;

    OH_AVCapability *cap = OH_AVCodec_GetCapability(mime, false);
    if (cap != nullptr) {
        out.hasCapability = true;
        const char *name = OH_AVCapability_GetName(cap);
        out.capabilityName = name != nullptr ? name : "";
        out.hardware = OH_AVCapability_IsHardware(cap);
        // M4.2.3 实测：海思 OMX 的 surface+sync/async 路径会固定少吐约 3 个 B 帧。
        // 仅作风险标记，不影响是否能创建解码器。
        out.surfaceBFrameRisk =
            out.hardware && out.capabilityName.find("hisi") != std::string::npos;

        OH_AVRange range{};
        if (OH_AVCapability_GetVideoWidthRange(cap, &range) == AV_ERR_OK) {
            out.minWidth = range.minVal;
            out.maxWidth = range.maxVal;
        }
        if (OH_AVCapability_GetVideoHeightRange(cap, &range) == AV_ERR_OK) {
            out.minHeight = range.minVal;
            out.maxHeight = range.maxVal;
        }
        if (OH_AVCapability_GetVideoFrameRateRange(cap, &range) == AV_ERR_OK) {
            out.maxFps = range.maxVal;
        }
    }

    // 只创建/销毁，不 Configure：验证系统确实能给出解码器实例
    OH_AVCodec *decoder = OH_VideoDecoder_CreateByMime(mime);
    out.creatable = decoder != nullptr;
    if (decoder != nullptr) OH_VideoDecoder_Destroy(decoder);
    return out;
}

}  // namespace

int VideoProbe(std::string *json) {
    std::vector<VideoResult> all;
    all.push_back(ProbeVideo("h264", OH_AVCODEC_MIMETYPE_VIDEO_AVC));
    all.push_back(ProbeVideo("hevc", OH_AVCODEC_MIMETYPE_VIDEO_HEVC));

    std::ostringstream os;
    os << "{\"decoders\":[";
    bool first = true;
    for (const auto &v : all) {
        if (!first) os << ",";
        first = false;
        const bool p720 = v.maxWidth >= 1280 && v.maxHeight >= 720;
        const bool p1080 = v.maxWidth >= 1920 && v.maxHeight >= 1080;
        const bool p4k = v.maxWidth >= 3840 && v.maxHeight >= 2160;
        os << "{\"codec\":\"" << v.codec << "\",\"mime\":\"" << v.mime << "\""
           << ",\"capability\":" << (v.hasCapability ? "true" : "false")
           << ",\"name\":\"" << v.capabilityName << "\""
           << ",\"hardware\":" << (v.hardware ? "true" : "false")
           << ",\"minWidth\":" << v.minWidth << ",\"maxWidth\":" << v.maxWidth
           << ",\"minHeight\":" << v.minHeight << ",\"maxHeight\":" << v.maxHeight
           << ",\"maxFps\":" << v.maxFps
           << ",\"creatable\":" << (v.creatable ? "true" : "false")
           << ",\"supports720p\":" << (p720 ? "true" : "false")
           << ",\"supports1080p\":" << (p1080 ? "true" : "false")
           << ",\"supports4K\":" << (p4k ? "true" : "false")
           << ",\"surfaceBFrameRisk\":" << (v.surfaceBFrameRisk ? "true" : "false")
           << ",\"knownIssue\":\""
           << (v.surfaceBFrameRisk ? "hisi-bframe-drop" : "") << "\"}";

        OH_LOG_INFO(LOG_APP,
                    "[vprobe] codec=%{public}s mime=%{public}s cap=%{public}d hw=%{public}d "
                    "range=%{public}dx%{public}d@%{public}d create=%{public}d",
                    v.codec, v.mime, v.hasCapability ? 1 : 0, v.hardware ? 1 : 0,
                    v.maxWidth, v.maxHeight, v.maxFps, v.creatable ? 1 : 0);
    }
    os << "]}";

    if (json != nullptr) *json = os.str();
    return 0;
}
