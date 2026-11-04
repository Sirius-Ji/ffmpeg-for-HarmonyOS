#include "audio_player.h"

#include <chrono>
#include <cstring>
#include <sstream>

extern "C" {
#include <libavutil/error.h>
}

#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>

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

std::string ResName(OH_AudioStream_Result r) {
    return std::to_string((int)r);
}

}  // namespace

AudioPlayer::AudioPlayer() : ring_(kRingBytes, 0) {}

AudioPlayer::~AudioPlayer() { Release(); }

void AudioPlayer::SetError(const std::string &what) { err_ = what; }

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
    OH_AudioStream_Result r =
        OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER);
    if (r != AUDIOSTREAM_SUCCESS || builder == nullptr) {
        SetError("OH_AudioStreamBuilder_Create failed: " + ResName(r));
        return -1;
    }

    OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_MUSIC);
    OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
    OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
    OH_AudioStreamBuilder_SetSamplingRate(builder, kOutRate);
    OH_AudioStreamBuilder_SetChannelCount(builder, kOutChannels);
    OH_AudioStreamBuilder_SetLatencyMode(builder, AUDIOSTREAM_LATENCY_MODE_NORMAL);
    // API 20 的 Advanced 版本：返回“实际写入字节数”，比只能整块返回的版本好控流
    OH_AudioStreamBuilder_SetRendererWriteDataCallbackAdvanced(
        builder, OnWriteDataCb, this);

    r = OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer_);
    OH_AudioStreamBuilder_Destroy(builder);
    if (r != AUDIOSTREAM_SUCCESS || renderer_ == nullptr) {
        renderer_ = nullptr;
        SetError("GenerateRenderer failed: " + ResName(r));
        return -1;
    }
    return 0;
}

// OHAudio 的回调线程：往 audioData 里填 PCM，缺口补静音
int32_t AudioPlayer::OnWriteDataCb(OH_AudioRenderer * /*renderer*/, void *userData,
                                   void *audioData, int32_t audioDataSize) {
    auto *self = static_cast<AudioPlayer *>(userData);
    if (self == nullptr || audioData == nullptr || audioDataSize <= 0) return 0;
    return self->FillAudio(static_cast<uint8_t *>(audioData), audioDataSize);
}

int AudioPlayer::FillAudio(uint8_t *buf, int32_t size) {
    const size_t got = RingRead(buf, (size_t)size);
    if (got < (size_t)size) {
        memset(buf + got, 0, (size_t)size - got);  // 欠载：补静音，别让时钟停
        underruns_.fetch_add(1);
    }
    playedBytes_.fetch_add((uint64_t)size);
    return size;
}

// ---------------- 解码线程 ----------------

void AudioPlayer::DecodeLoop() {
    std::vector<uint8_t> chunk;
    while (running_.load()) {
        if (RingUsed() > ring_.size() * 3 / 4) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        int ret = decoder_->ReadPcm(chunk, kChunkFrames, kOutRate, kOutChannels);
        if (ret == AVERROR_EOF) {
            finished_.store(true);
            break;
        }
        if (ret < 0) {
            SetError("decode: " + decoder_->LastError());
            break;
        }
        if (chunk.empty()) continue;
        size_t off = 0;
        while (off < chunk.size() && running_.load()) {
            off += RingWrite(chunk.data() + off, chunk.size() - off);
            if (off < chunk.size()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
}

// ---------------- 对外状态机 ----------------

int AudioPlayer::Open(const std::string &path) {
    Release();
    path_ = path;
    decoder_.reset(new AudioDecoder());

    AudioInfo info;
    int ret = decoder_->Open(path, &info);
    if (ret < 0) {
        SetError(decoder_->LastError());
        decoder_.reset();
        path_.clear();
        return -1;
    }
    codec_ = info.codec;
    durationMs_ = info.durationMs;
    finished_.store(false);

    return BuildRenderer();
}

int AudioPlayer::Play() {
    if (path_.empty()) {
        SetError("player: no file opened");
        return -1;
    }
    // 上一轮播完 / Stop 过：重新打开一遍，从头播
    if (state_.load() == 3 || finished_.load()) {
        const std::string path = path_;
        if (Open(path) != 0) return -1;
    }
    if (renderer_ == nullptr) {
        if (BuildRenderer() != 0) return -1;
    }
    if (state_.load() == 2) {  // paused -> 继续
        OH_AudioStream_Result r = OH_AudioRenderer_Start(renderer_);
        if (r != AUDIOSTREAM_SUCCESS) {
            SetError("OH_AudioRenderer_Start failed: " + ResName(r));
            return -1;
        }
        state_.store(1);
        return 0;
    }
    if (running_.load()) return 0;

    OH_AudioStream_Result r = OH_AudioRenderer_Start(renderer_);
    if (r != AUDIOSTREAM_SUCCESS) {
        SetError("OH_AudioRenderer_Start failed: " + ResName(r));
        return -1;
    }
    state_.store(1);
    running_.store(true);
    thread_ = std::thread(&AudioPlayer::DecodeLoop, this);
    return 0;
}

int AudioPlayer::Pause() {
    if (renderer_ == nullptr || state_.load() != 1) return 0;
    OH_AudioStream_Result r = OH_AudioRenderer_Pause(renderer_);
    if (r != AUDIOSTREAM_SUCCESS) {
        SetError("OH_AudioRenderer_Pause failed: " + ResName(r));
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
    playedBytes_.store(0);
    underruns_.store(0);
    finished_.store(false);
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
    playedBytes_.store(0);
    underruns_.store(0);
    finished_.store(false);
    state_.store(0);
    durationMs_ = 0;
    codec_.clear();
    path_.clear();
}

std::string AudioPlayer::StatusJson() const {
    const uint64_t played = playedBytes_.load();
    const int64_t positionMs =
        (int64_t)(played / ((uint64_t)kOutRate * kOutChannels * 2 / 1000));
    std::ostringstream os;
    os << "{\"state\":\"" << StateName(state_.load()) << "\""
       << ",\"positionMs\":" << positionMs
       << ",\"durationMs\":" << durationMs_
       << ",\"underruns\":" << underruns_.load()
       << ",\"finished\":" << (finished_.load() ? "true" : "false")
       << ",\"sampleRate\":" << kOutRate
       << ",\"channels\":" << kOutChannels
       << ",\"codec\":\"" << codec_ << "\""
       << ",\"error\":\"" << err_ << "\"}";
    return os.str();
}
