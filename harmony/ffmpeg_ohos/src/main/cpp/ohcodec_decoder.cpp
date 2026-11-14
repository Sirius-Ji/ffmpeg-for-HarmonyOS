#include "ohcodec_decoder.h"

#include <cstdio>
#include <cstring>
#include <thread>

#include <hilog/log.h>

extern "C" {
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avbuffer_info.h>
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

const char *ErrName(int code) {
    switch (code) {
        case AV_ERR_OK: return "OK";
        case AV_ERR_NO_MEMORY: return "NO_MEMORY";
        case AV_ERR_OPERATE_NOT_PERMIT: return "NOT_PERMIT";
        case AV_ERR_INVALID_VAL: return "INVALID_VAL";
        case AV_ERR_IO: return "IO";
        case AV_ERR_TIMEOUT: return "TIMEOUT";
        case AV_ERR_UNKNOWN: return "UNKNOWN";
        case AV_ERR_SERVICE_DIED: return "SERVICE_DIED";
        case AV_ERR_INVALID_STATE: return "INVALID_STATE";
        case AV_ERR_UNSUPPORT: return "UNSUPPORT";
        case AV_ERR_INPUT_DATA_ERROR: return "INPUT_DATA_ERROR";
        case AV_ERR_UNSUPPORTED_FORMAT: return "UNSUPPORTED_FORMAT";
        case AV_ERR_STREAM_CHANGED: return "STREAM_CHANGED";
        case AV_ERR_TRY_AGAIN_LATER: return "TRY_AGAIN_LATER";
        default: return "OTHER";
    }
}

bool IsNoDataYet(int code) {
    return code == AV_ERR_TIMEOUT || code == AV_ERR_TRY_AGAIN_LATER;
}

const char *CodeText(int code) {
    static thread_local char buf[64];
    snprintf(buf, sizeof(buf), "%d(%s)", code, ErrName(code));
    return buf;
}

}  // namespace

OhCodecDecoder::~OhCodecDecoder() { Close(); }

bool OhCodecDecoder::CapabilityAllows(const char *mime, int sampleRate, int channels,
                                      std::string *why) {
    if (channels != 1 && channels != 2) {
        if (why) *why = "channels=" + std::to_string(channels) + " unsupported";
        return false;
    }
    OH_AVCapability *cap = OH_AVCodec_GetCapability(mime, false);
    if (cap == nullptr) {
        if (why) *why = std::string("no capability for ") + mime;
        return false;
    }
    const int32_t *rates = nullptr;
    uint32_t count = 0;
    if (OH_AVCapability_GetAudioSupportedSampleRates(cap, &rates, &count) != AV_ERR_OK ||
        rates == nullptr) {
        if (why) *why = "query sample rates failed";
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (rates[i] == sampleRate) return true;
    }
    if (why) *why = "sample rate " + std::to_string(sampleRate) + " not supported by " + mime;
    return false;
}

int OhCodecDecoder::Open(const char *mime, int sampleRate, int channels, const uint8_t *extradata,
                         int extradataSize, std::string *why) {
    Close();
    mime_ = mime;
    sampleRate_ = sampleRate;
    channels_ = channels;

    OH_AVCodec *codec = OH_AudioCodec_CreateByMime(mime, false);
    if (codec == nullptr) {
        if (why) *why = std::string("CreateByMime failed for ") + mime;
        return -1;
    }

    OH_AVFormat *format = OH_AVFormat_Create();
    if (format == nullptr) {
        OH_AudioCodec_Destroy(codec);
        if (why) *why = "AVFormat_Create failed";
        return -2;
    }
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_ENABLE_SYNC_MODE, 1);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, SAMPLE_S16LE);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_CHANNEL_COUNT, channels);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_AUD_SAMPLE_RATE, sampleRate);
    if (extradata != nullptr && extradataSize > 0) {
        OH_AVFormat_SetBuffer(format, OH_MD_KEY_CODEC_CONFIG, extradata, (size_t)extradataSize);
    }

    int stage = 0;
    OH_AVErrCode rc = OH_AudioCodec_Configure(codec, format);
    OH_AVFormat_Destroy(format);
    if (rc != AV_ERR_OK) {
        stage = 1;
    } else if ((rc = OH_AudioCodec_Prepare(codec)) != AV_ERR_OK) {
        stage = 2;
    } else if ((rc = OH_AudioCodec_Start(codec)) != AV_ERR_OK) {
        stage = 3;
    }

    if (stage != 0) {
        if (why) {
            *why = std::string("stage") + std::to_string(stage) + " rc=" + CodeText((int)rc);
        }
        OH_AudioCodec_Destroy(codec);
        return -3;
    }

    codec_ = codec;
    eosSent_ = false;
    started_ = true;
    pushedPackets_ = 0;
    pushedBytes_ = 0;
    outBytes_ = 0;
    outSamples_ = 0;
    OH_LOG_INFO(LOG_APP, "[ohcodec] open ok mime=%{public}s rate=%{public}d ch=%{public}d", mime_,
                sampleRate_, channels_);
    return 0;
}

void OhCodecDecoder::Close() {
    if (codec_ == nullptr) return;
    OH_LOG_INFO(LOG_APP, "[ohcodec] close begin mime=%{public}s started=%{public}d", mime_,
                started_ ? 1 : 0);

    // 1) 把残留输出缓冲收干净：Stop 会等这些 buffer 归还
    int drained = 0;
    for (int i = 0; i < 32; ++i) {
        uint32_t index = 0;
        if (OH_AudioCodec_QueryOutputBuffer(codec_, &index, 0) != AV_ERR_OK) break;
        OH_AudioCodec_FreeOutputBuffer(codec_, index);
        drained++;
    }
    if (drained > 0) OH_LOG_INFO(LOG_APP, "[ohcodec] close drained=%{public}d", drained);

    // 2) Flush：丢弃内部残留，让实例回到可 Stop 的状态
    OH_AVErrCode rc = OH_AudioCodec_Flush(codec_);
    OH_LOG_INFO(LOG_APP, "[ohcodec] close flush rc=%{public}d", (int)rc);

    // 3) Stop（只有真的 Start 过才调）
    if (started_) {
        rc = OH_AudioCodec_Stop(codec_);
        OH_LOG_INFO(LOG_APP, "[ohcodec] close stop rc=%{public}d", (int)rc);
        started_ = false;
    }

    // 4) Destroy，彻底释放系统解码器句柄
    rc = OH_AudioCodec_Destroy(codec_);
    OH_LOG_INFO(LOG_APP, "[ohcodec] close destroy rc=%{public}d", (int)rc);
    codec_ = nullptr;
    eosSent_ = false;
}

void OhCodecDecoder::AbandonAsync() {
    OH_AVCodec *codec = codec_;
    codec_ = nullptr;
    started_ = false;
    if (codec == nullptr) return;
    OH_LOG_WARN(LOG_APP, "[ohcodec] abandon -> async teardown");
    std::thread([codec]() {
        OH_LOG_INFO(LOG_APP, "[ohcodec] async teardown begin");
        OH_AudioCodec_Flush(codec);
        OH_AudioCodec_Stop(codec);
        OH_AudioCodec_Destroy(codec);
        OH_LOG_INFO(LOG_APP, "[ohcodec] async teardown done");
    }).detach();
}

int OhCodecDecoder::Push(const uint8_t *data, int size, int64_t ptsUs, std::string *why) {
    if (codec_ == nullptr) {
        if (why) *why = "decoder not open";
        return -1;
    }
    uint32_t index = 0;
    OH_AVErrCode rc = OH_AudioCodec_QueryInputBuffer(codec_, &index, 0);
    if (IsNoDataYet((int)rc)) return 1;
    if (rc != AV_ERR_OK) {
        if (why) *why = std::string("QueryInputBuffer rc=") + CodeText((int)rc);
        return -2;
    }
    OH_AVBuffer *buf = OH_AudioCodec_GetInputBuffer(codec_, index);
    if (buf == nullptr) {
        if (why) *why = "GetInputBuffer null";
        return -3;
    }
    const int32_t capacity = OH_AVBuffer_GetCapacity(buf);
    if (size > capacity) {
        if (why) *why = "packet larger than input buffer";
        return -4;
    }
    uint8_t *addr = OH_AVBuffer_GetAddr(buf);
    if (addr == nullptr || data == nullptr) {
        if (why) *why = "GetAddr null";
        return -5;
    }
    memcpy(addr, data, (size_t)size);

    OH_AVCodecBufferAttr attr{};
    attr.pts = ptsUs;
    attr.size = size;
    attr.offset = 0;
    attr.flags = 0;
    rc = OH_AVBuffer_SetBufferAttr(buf, &attr);
    if (rc != AV_ERR_OK) {
        if (why) *why = std::string("SetBufferAttr rc=") + CodeText((int)rc);
        return -6;
    }
    rc = OH_AudioCodec_PushInputBuffer(codec_, index);
    if (rc != AV_ERR_OK) {
        if (why) *why = std::string("PushInputBuffer rc=") + CodeText((int)rc);
        return -7;
    }
    pushedPackets_++;
    pushedBytes_ += size;
    return 0;
}

int OhCodecDecoder::SendEos(std::string *why) {
    if (codec_ == nullptr) return -1;
    uint32_t index = 0;
    OH_AVErrCode rc = OH_AudioCodec_QueryInputBuffer(codec_, &index, 0);  // 非阻塞
    if (IsNoDataYet((int)rc)) return 1;
    if (rc != AV_ERR_OK) {
        if (why) *why = std::string("EOS QueryInputBuffer rc=") + CodeText((int)rc);
        return -2;
    }
    OH_AVBuffer *buf = OH_AudioCodec_GetInputBuffer(codec_, index);
    if (buf == nullptr) return -3;
    OH_AVCodecBufferAttr attr{};
    attr.pts = 0;
    attr.size = 0;
    attr.offset = 0;
    attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
    OH_AVBuffer_SetBufferAttr(buf, &attr);
    rc = OH_AudioCodec_PushInputBuffer(codec_, index);
    if (rc != AV_ERR_OK) {
        if (why) *why = std::string("EOS push rc=") + CodeText((int)rc);
        return -4;
    }
    eosSent_ = true;
    return 0;
}

int OhCodecDecoder::Pull(std::vector<uint8_t> *pcm, int *samples, std::string *why) {
    if (codec_ == nullptr) return -1;
    if (pcm) pcm->clear();
    if (samples) *samples = 0;

    uint32_t index = 0;
    // 非阻塞：让解码线程每一轮都能检查取消标志，Stop 时不会被系统调用拖住
    OH_AVErrCode rc = OH_AudioCodec_QueryOutputBuffer(codec_, &index, 0);
    if (IsNoDataYet((int)rc)) return 1;  // 还没数据，不是错误
    if (rc != AV_ERR_OK) {
        if (why) *why = std::string("QueryOutputBuffer rc=") + CodeText((int)rc);
        return -2;
    }
    OH_AVBuffer *buf = OH_AudioCodec_GetOutputBuffer(codec_, index);
    if (buf == nullptr) {
        if (why) *why = "GetOutputBuffer null";
        return -3;
    }
    OH_AVCodecBufferAttr attr{};
    rc = OH_AVBuffer_GetBufferAttr(buf, &attr);
    if (rc != AV_ERR_OK) {
        OH_AudioCodec_FreeOutputBuffer(codec_, index);
        if (why) *why = std::string("GetBufferAttr rc=") + CodeText((int)rc);
        return -4;
    }
    const bool eos = (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0;
    if (attr.size > 0 && pcm != nullptr) {
        const int32_t capacity = OH_AVBuffer_GetCapacity(buf);
        const int32_t copy = attr.size <= capacity ? attr.size : capacity;
        const uint8_t *addr = OH_AVBuffer_GetAddr(buf);
        if (addr != nullptr && copy > 0) {
            pcm->assign(addr, addr + copy);
            if (samples) *samples = copy / (2 * channels_);
            outBytes_ += copy;
            outSamples_ += copy / (2 * channels_);
        }
    }
    OH_AudioCodec_FreeOutputBuffer(codec_, index);
    if (eos) return 2;
    return 0;
}
