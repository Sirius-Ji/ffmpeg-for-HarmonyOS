#include "audio_player.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <hilog/log.h>

extern "C" {
#include <libavutil/error.h>
}

#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xFF01
#define LOG_TAG "FFM1"

namespace {

constexpr int kOutRate = 48000;
constexpr int kOutChannels = 2;
constexpr size_t kRingBytes = (size_t)kOutRate * kOutChannels * 2;  // 1 秒缓冲
constexpr int kChunkFrames = 2048;                                  // 每次解码 2048 帧

const char *StateName(int state) {
    switch (state) {
        case 1: return "playing";
        case 2: return "paused";
        case 3: return "stopped";
        default: return "idle";
    }
}

}  // namespace

AudioPlayer::AudioPlayer() : ring_(kRingBytes, 0) {}

AudioPlayer::~AudioPlayer() { Release(); }

// ---------------- 错误串（解码线程也会写，必须加锁） ----------------

void AudioPlayer::SetError(const std::string &what) {
    std::lock_guard<std::mutex> lock(errMutex_);
    err_ = what;
}

std::string AudioPlayer::LastError() const {
    std::lock_guard<std::mutex> lock(errMutex_);
    return err_;
}

// ---------------- 环形缓冲 ----------------

size_t AudioPlayer::RingWrite(const uint8_t *data, size_t len) {
    std::lock_guard<std::mutex> lock(ringMutex_);
    const size_t space = ring_.size() - ringUsed_;
    const size_t n = len < space ? len : space;
    for (size_t i = 0; i < n; ++i) {
        ring_[ringWrite_] = data[i];
        ringWrite_ = (ringWrite_ + 1) % ring_.size();
    }
    ringUsed_ += n;
    return n;
}

size_t AudioPlayer::RingRead(uint8_t *data, size_t len) {
    std::lock_guard<std::mutex> lock(ringMutex_);
    const size_t n = len < ringUsed_ ? len : ringUsed_;
    for (size_t i = 0; i < n; ++i) {
        data[i] = ring_[ringRead_];
        ringRead_ = (ringRead_ + 1) % ring_.size();
    }
    ringUsed_ -= n;
    return n;
}

size_t AudioPlayer::RingUsed() const {
    std::lock_guard<std::mutex> lock(ringMutex_);
    return ringUsed_;
}

void AudioPlayer::RingClear() {
    std::lock_guard<std::mutex> lock(ringMutex_);
    ringRead_ = ringWrite_ = ringUsed_ = 0;
}

// ---------------- OHAudio ----------------

int AudioPlayer::BuildRenderer() {
    OH_AudioStreamBuilder *builder = nullptr;
    OH_AudioStream_Result r = OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER);
    if (r != AUDIOSTREAM_SUCCESS || builder == nullptr) {
        SetError("OH_AudioStreamBuilder_Create rc=" + std::to_string((int)r));
        return -1;
    }

    auto set = [&](OH_AudioStream_Result rr, const char *what) -> bool {
        if (rr != AUDIOSTREAM_SUCCESS) {
            SetError(std::string("builder ") + what + " rc=" + std::to_string((int)rr));
            return false;
        }
        return true;
    };

    const bool ok =
        set(OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_MUSIC), "SetRendererInfo") &&
        set(OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE), "SetSampleFormat") &&
        set(OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW), "SetEncodingType") &&
        set(OH_AudioStreamBuilder_SetSamplingRate(builder, kOutRate), "SetSamplingRate") &&
        set(OH_AudioStreamBuilder_SetChannelCount(builder, kOutChannels), "SetChannelCount") &&
        set(OH_AudioStreamBuilder_SetLatencyMode(builder, AUDIOSTREAM_LATENCY_MODE_NORMAL), "SetLatencyMode") &&
        set(OH_AudioStreamBuilder_SetRendererWriteDataCallbackAdvanced(builder, OnWriteDataCb, this),
            "SetWriteDataCallbackAdvanced");
    if (!ok) {
        OH_AudioStreamBuilder_Destroy(builder);
        return -1;
    }

    r = OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer_);
    OH_AudioStreamBuilder_Destroy(builder);
    if (r != AUDIOSTREAM_SUCCESS || renderer_ == nullptr) {
        renderer_ = nullptr;
        SetError("GenerateRenderer rc=" + std::to_string((int)r));
        return -1;
    }

    // 回读实际生效的参数：配置没生效时这里会露馅
    int32_t v = 0;
    if (OH_AudioRenderer_GetSamplingRate(renderer_, &v) == AUDIOSTREAM_SUCCESS) rendererRate_ = v;
    v = 0;
    if (OH_AudioRenderer_GetChannelCount(renderer_, &v) == AUDIOSTREAM_SUCCESS) rendererChannels_ = v;
    v = 0;
    if (OH_AudioRenderer_GetFrameSizeInCallback(renderer_, &v) == AUDIOSTREAM_SUCCESS) rendererFrameSize_ = v;
    OH_AudioStream_SampleFormat sf = AUDIOSTREAM_SAMPLE_S16LE;
    if (OH_AudioRenderer_GetSampleFormat(renderer_, &sf) == AUDIOSTREAM_SUCCESS) rendererFormat_ = (int)sf;
    OH_AudioStream_EncodingType et = AUDIOSTREAM_ENCODING_TYPE_RAW;
    if (OH_AudioRenderer_GetEncodingType(renderer_, &et) == AUDIOSTREAM_SUCCESS) rendererEncoding_ = (int)et;

    OH_LOG_INFO(LOG_APP,
                "[player] renderer rate=%{public}d ch=%{public}d fmt=%{public}d enc=%{public}d frameSize=%{public}d",
                rendererRate_, rendererChannels_, rendererFormat_, rendererEncoding_, rendererFrameSize_);
    if (rendererRate_ != kOutRate || rendererChannels_ != kOutChannels) {
        OH_LOG_ERROR(LOG_APP, "[player] 渲染器参数与请求不一致（请求 48000/2，实际 %{public}d/%{public}d）",
                     rendererRate_, rendererChannels_);
        SetError("renderer params mismatch");
        return -1;
    }
    return 0;
}

// 回调线程：填 PCM。解码到头后不再写任何字节，位置就不会继续涨。
int32_t AudioPlayer::OnWriteDataCb(OH_AudioRenderer * /*renderer*/, void *userData,
                                   void *audioData, int32_t audioDataSize) {
    auto *self = static_cast<AudioPlayer *>(userData);
    if (self == nullptr || audioData == nullptr || audioDataSize <= 0) return 0;
    return self->FillAudio(static_cast<uint8_t *>(audioData), audioDataSize);
}

int AudioPlayer::FillAudio(uint8_t *buf, int32_t size) {
    callbackCount_.fetch_add(1);

    const size_t got = RingRead(buf, (size_t)size);
    if (got > 0) gotFirstData_.store(true);
    if (got == (size_t)size) return size;

    // 解码到头且缓冲已空：一个字节都不写，让系统睡，避免用静音把播放进度推走
    if (decoderEof_.load() && RingUsed() == 0) {
        return 0;
    }

    // 数据没跟上（启动阶段或抖动）：补静音并计数，保证系统时钟不停
    memset(buf + got, 0, (size_t)size - got);
    gaps_.fetch_add(1);
    return size;
}

// ---------------- 解码线程 ----------------

void AudioPlayer::DecodeLoop() {
    std::vector<uint8_t> chunk;
    int64_t written = 0;
    while (running_.load()) {
        if (RingUsed() > ring_.size() * 3 / 4) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        int ret = decoder_->ReadPcm(chunk, kChunkFrames, kOutRate, kOutChannels);
        if (ret == AVERROR_EOF) {
            decoderEof_.store(true);
            break;
        }
        if (ret < 0) {
            SetError("decode: " + decoder_->LastError());
            decoderEof_.store(true);
            break;
        }
        if (chunk.empty()) continue;
        size_t off = 0;
        while (off < chunk.size() && running_.load()) {
            off += RingWrite(chunk.data() + off, chunk.size() - off);
            if (off < chunk.size()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        written += (int64_t)chunk.size();
    }
    OH_LOG_INFO(LOG_APP, "[player] decode loop end, pcm=%{public}lld bytes, eof=%{public}d",
                (long long)written, decoderEof_.load() ? 1 : 0);
}

// 用流式路径把整个文件解一遍，算 FNV-1a 64，用来和桌面参考值对拍。
// 只有第一次 Open 做，避免每次重播都多解一遍。
int AudioPlayer::SelfCheck() {
    if (!pcmHash_.empty()) return 0;

    AudioDecoder probe;
    AudioInfo info;
    if (probe.Open(path_, &info) < 0) {
        OH_LOG_WARN(LOG_APP, "[player] selfcheck: open failed %{public}s", probe.LastError().c_str());
        return -1;
    }
    std::vector<uint8_t> chunk;
    unsigned long long h = 1469598103934665603ULL;
    int64_t total = 0;
    int peak = 0;
    for (;;) {
        int ret = probe.ReadPcm(chunk, 4096, kOutRate, kOutChannels);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) {
            OH_LOG_WARN(LOG_APP, "[player] selfcheck: read failed %{public}s", probe.LastError().c_str());
            return -1;
        }
        for (size_t i = 0; i < chunk.size(); ++i) {
            h ^= chunk[i];
            h *= 1099511628211ULL;
        }
        for (size_t i = 0; i + 1 < chunk.size(); i += 2) {
            const int v = (int16_t)(chunk[i] | (chunk[i + 1] << 8));
            const int a = v < 0 ? -v : v;
            if (a > peak) peak = a;
        }
        total += (int64_t)chunk.size();
    }
    char hex[24] = {0};
    snprintf(hex, sizeof(hex), "%016llx", h);
    pcmHash_ = hex;
    pcmBytes_ = total;
    pcmPeak_ = peak;
    OH_LOG_INFO(LOG_APP, "[player] selfcheck pcm=%{public}lld bytes peak=%{public}d hash=%{public}s (48k/2ch/S16)",
                (long long)total, peak, hex);
    return 0;
}

// ---------------- 状态机 ----------------

int AudioPlayer::Open(const std::string &path) {
    Release();
    path_ = path;
    decoder_.reset(new AudioDecoder());

    AudioInfo info;
    if (decoder_->Open(path, &info) < 0) {
        SetError(decoder_->LastError());
        decoder_.reset();
        path_.clear();
        return -1;
    }
    codec_ = info.codec;
    durationMs_ = info.durationMs;

    SelfCheck();  // 哈希对拍，失败不影响播放

    if (BuildRenderer() != 0) return -1;

    OH_LOG_INFO(LOG_APP, "[player] open ok codec=%{public}s duration=%{public}lldms pcmBytes=%{public}lld hash=%{public}s",
                codec_.c_str(), (long long)durationMs_, (long long)pcmBytes_, pcmHash_.c_str());
    return 0;
}

int AudioPlayer::Play() {
    if (path_.empty()) {
        SetError("player: no file opened");
        return -1;
    }
    if (state_.load() == 3 || (decoderEof_.load() && RingUsed() == 0)) {
        const std::string path = path_;
        if (Open(path) != 0) return -1;
    }
    if (renderer_ == nullptr && BuildRenderer() != 0) return -1;

    if (state_.load() == 2) {  // paused -> 继续
        if (OH_AudioRenderer_Start(renderer_) != AUDIOSTREAM_SUCCESS) {
            SetError("OH_AudioRenderer_Start failed");
            return -1;
        }
        state_.store(1);
        return 0;
    }
    if (running_.load()) return 0;

    decoderEof_.store(false);
    gotFirstData_.store(false);
    gaps_.store(0);
    callbackCount_.store(0);

    if (OH_AudioRenderer_Start(renderer_) != AUDIOSTREAM_SUCCESS) {
        SetError("OH_AudioRenderer_Start failed");
        return -1;
    }
    state_.store(1);
    running_.store(true);
    thread_ = std::thread(&AudioPlayer::DecodeLoop, this);
    return 0;
}

int AudioPlayer::Pause() {
    if (renderer_ == nullptr || state_.load() != 1) return 0;
    if (OH_AudioRenderer_Pause(renderer_) != AUDIOSTREAM_SUCCESS) {
        SetError("OH_AudioRenderer_Pause failed");
        return -1;
    }
    state_.store(2);
    return 0;
}

int AudioPlayer::Stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (renderer_ != nullptr) {
        OH_AudioRenderer_Stop(renderer_);
        OH_AudioRenderer_Flush(renderer_);
    }
    RingClear();
    decoderEof_.store(false);
    gotFirstData_.store(false);
    gaps_.store(0);
    callbackCount_.store(0);
    state_.store(3);
    return 0;
}

void AudioPlayer::Release() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (renderer_ != nullptr) {
        OH_AudioRenderer_Stop(renderer_);
        OH_AudioRenderer_Release(renderer_);
        renderer_ = nullptr;
    }
    RingClear();
    decoder_.reset();
    decoderEof_.store(false);
    gotFirstData_.store(false);
    gaps_.store(0);
    callbackCount_.store(0);
    state_.store(0);
    durationMs_ = 0;
    codec_.clear();
    pcmHash_.clear();
    pcmBytes_ = 0;
    path_.clear();
    rendererRate_ = rendererChannels_ = rendererFrameSize_ = 0;
}

std::string AudioPlayer::StatusJson() const {
    // 位置用官方口径：渲染器已写帧数，而不是我们数回调
    int64_t framesWritten = 0;
    if (renderer_ != nullptr) OH_AudioRenderer_GetFramesWritten(renderer_, &framesWritten);
    int64_t positionMs = rendererRate_ > 0 ? framesWritten * 1000 / rendererRate_ : 0;
    if (durationMs_ > 0 && positionMs > durationMs_) positionMs = durationMs_;  // 别超过总长

    uint32_t underflow = 0;
    if (renderer_ != nullptr) OH_AudioRenderer_GetUnderflowCount(renderer_, &underflow);

    const bool finished = decoderEof_.load() && RingUsed() == 0;

    std::ostringstream os;
    os << "{\"state\":\"" << StateName(state_.load()) << "\""
       << ",\"positionMs\":" << positionMs
       << ",\"durationMs\":" << durationMs_
       << ",\"framesWritten\":" << framesWritten
       << ",\"underflow\":" << underflow
       << ",\"gaps\":" << gaps_.load()
       << ",\"callbacks\":" << callbackCount_.load()
       << ",\"rendererRate\":" << rendererRate_
       << ",\"rendererChannels\":" << rendererChannels_
       << ",\"rendererFrameSize\":" << rendererFrameSize_
       << ",\"pcmHash\":\"" << pcmHash_ << "\""
       << ",\"pcmBytes\":" << pcmBytes_
       << ",\"pcmPeak\":" << pcmPeak_
       << ",\"finished\":" << (finished ? "true" : "false")
       << ",\"error\":\"" << LastError() << "\"}";
    return os.str();
}
