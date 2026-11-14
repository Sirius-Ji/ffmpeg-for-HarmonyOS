#include "hardware_probe.h"

#include <sstream>
#include <vector>

#include <hilog/log.h>

extern "C" {
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_audiocodec.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_averrors.h>
#include <multimedia/player_framework/native_avformat.h>
}

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xFF01
#define LOG_TAG "FFM1"

namespace {

struct RateResult {
    int rate = 0;
    int configure = -999;
    int prepare = -999;
    int start = -999;
};

struct MimeResult {
    const char *name = "";
    const char *mime = "";
    bool hasCapability = false;
    std::string capabilityName;
    std::vector<int> rates;
    std::vector<RateResult> results;
};

// 在指定采样率上走一遍 Configure → Prepare → Start，全部成功才算真能解
RateResult ProbeRate(const char *mime, int rate) {
    RateResult r;
    r.rate = rate;
    OH_AVCodec *codec = OH_AudioCodec_CreateByMime(mime, false);
    if (codec == nullptr) {
        r.configure = -1;
        return r;
    }

    OH_AVFormat *format = OH_AVFormat_Create();
    if (format == nullptr) {
        OH_AudioCodec_Destroy(codec);
        r.configure = -2;
        return r;
    }
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_ENABLE_SYNC_MODE, 1);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, rate);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, 2);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, SAMPLE_S16LE);

    r.configure = (int)OH_AudioCodec_Configure(codec, format);
    if (r.configure == (int)AV_ERR_OK) {
        r.prepare = (int)OH_AudioCodec_Prepare(codec);
    }
    if (r.prepare == (int)AV_ERR_OK) {
        r.start = (int)OH_AudioCodec_Start(codec);
    }
    if (r.start == (int)AV_ERR_OK) {
        OH_AudioCodec_Stop(codec);
    }

    OH_AVFormat_Destroy(format);
    OH_AudioCodec_Destroy(codec);
    return r;
}

MimeResult ProbeMime(const char *name, const char *mime) {
    MimeResult out;
    out.name = name;
    out.mime = mime;

    OH_AVCapability *cap = OH_AVCodec_GetCapability(mime, false);
    if (cap != nullptr) {
        out.hasCapability = true;
        const char *capName = OH_AVCapability_GetName(cap);
        out.capabilityName = capName ? capName : "";

        const int32_t *rates = nullptr;
        uint32_t rateCount = 0;
        if (OH_AVCapability_GetAudioSupportedSampleRates(cap, &rates, &rateCount) == AV_ERR_OK &&
            rates != nullptr) {
            for (uint32_t i = 0; i < rateCount; ++i) out.rates.push_back(rates[i]);
        }
    }

    // 能力列表归能力列表，实际能不能 Configure/Start 才是硬证据
    for (int rate : {44100, 48000, 96000}) {
        out.results.push_back(ProbeRate(mime, rate));
    }
    return out;
}

}  // namespace

int HardwareProbe(std::string *json) {
    std::vector<MimeResult> all;
    all.push_back(ProbeMime("aac", OH_AVCODEC_MIMETYPE_AUDIO_AAC));
    all.push_back(ProbeMime("mp3", OH_AVCODEC_MIMETYPE_AUDIO_MPEG));
    all.push_back(ProbeMime("flac", OH_AVCODEC_MIMETYPE_AUDIO_FLAC));

    std::ostringstream os;
    os << "{\"probes\":[";
    bool first = true;
    for (const auto &m : all) {
        if (!first) os << ",";
        first = false;
        os << "{\"name\":\"" << m.name << "\",\"mime\":\"" << m.mime << "\""
           << ",\"capability\":" << (m.hasCapability ? "true" : "false")
           << ",\"capabilityName\":\"" << m.capabilityName << "\""
           << ",\"supportedRates\":[";
        for (size_t i = 0; i < m.rates.size(); ++i) {
            if (i) os << ",";
            os << m.rates[i];
        }
        os << "],\"tries\":[";
        bool firstTry = true;
        for (const auto &r : m.results) {
            if (!firstTry) os << ",";
            firstTry = false;
            os << "{\"rate\":" << r.rate << ",\"configure\":" << r.configure
               << ",\"prepare\":" << r.prepare << ",\"start\":" << r.start << "}";
        }
        os << "]}";

        std::ostringstream line;
        line << "mime=" << m.mime << " name=" << m.name
             << " cap=" << (m.hasCapability ? 1 : 0) << " capName=" << m.capabilityName
             << " rates=";
        for (size_t i = 0; i < m.rates.size(); ++i) {
            if (i) line << ",";
            line << m.rates[i];
        }
        for (const auto &r : m.results) {
            line << " | " << r.rate << ":cfg=" << r.configure << ",prep=" << r.prepare
                 << ",start=" << r.start;
        }
        OH_LOG_INFO(LOG_APP, "[probe] %{public}s", line.str().c_str());
    }
    os << "]}";

    if (json) *json = os.str();
    return 0;
}
