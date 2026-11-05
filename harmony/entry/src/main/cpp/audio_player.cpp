#include "audio_player.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <sys/resource.h>
#include <unistd.h>

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
constexpr size_t kBytesPerSecond = (size_t)kOutRate * kOutChannels * 2;  // 192000 B/s
constexpr size_t kRingBytes = 2 * kBytesPerSecond;                       // 2 秒缓冲（384 KB）
constexpr size_t kPreloadBytes = kBytesPerSecond / 5;                    // 启动前预填 200 ms
constexpr int kChunkFrames = 8192;                                       // 每次解码 8192 帧（≈170 ms）

const char *StateName(int state) {
    switch (state) {
        case 1: return "playing";
        case 2: return "paused";
        case 3: return "stopped";
        default: return "idle";
    }
}

// 临时关掉硬解（自检对拍用），析构时恢复原状态
struct HwDisableGuard {
    bool prev;
    HwDisableGuard() : prev(HardwareDecodeEnabled()) { SetHardwareDecodeEnabled(false); }
    ~HwDisableGuard() { SetHardwareDecodeEnabled(prev); }
};

int64_t NowUs() {
    return (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 进程累计 CPU 时间（用户态 + 内核态），单位微秒
int64_t CpuUs() {
    struct rusage ru {};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    return (int64_t)ru.ru_utime.tv_sec * 1000000 + ru.ru_utime.tv_usec +
           (int64_t)ru.ru_stime.tv_sec * 1000000 + ru.ru_stime.tv_usec;
}

// 当前进程常驻内存（KB），读不到返回 -1
long long RssKb() {
    FILE *f = fopen("/proc/self/statm", "r");
    if (f == nullptr) return -1;
    long totalPages = 0;
    long residentPages = 0;
    const int n = fscanf(f, "%ld %ld", &totalPages, &residentPages);
    fclose(f);
    if (n != 2) return -1;
    const long pageSize = sysconf(_SC_PAGESIZE);
    return (long long)residentPages * pageSize / 1024;
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
    if (size > maxCallbackBytes_.load()) maxCallbackBytes_.store(size);

    const size_t got = RingRead(buf, (size_t)size);
    if (got > 0) {
        gotFirstData_.store(true);
        realBytes_.fetch_add(got);
    }
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
    lastPerfUs_ = NowUs();
    lastCpuUs_ = CpuUs();

    while (running_.load()) {
        // 尽量把 ring 填满（留 4KB 余量），而不是等掉到某条水位线以下才动
        if (RingUsed() + 4096 >= ring_.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const int64_t t0 = NowUs();
        int ret = decoder_->ReadPcm(chunk, kChunkFrames, kOutRate, kOutChannels);
        const int64_t t1 = NowUs();
        decodeUs_.fetch_add((uint64_t)(t1 - t0));
        decodeCalls_.fetch_add(1);

        if (ret == AVERROR_EOF) {
            decoderEof_.store(true);
            break;
        }
        if (ret < 0) {
            if (!running_.load()) break;  // Stop 触发的取消，不算错误
            SetError("decode: " + decoder_->LastError());
            decoderEof_.store(true);
            break;
        }
        if (chunk.empty()) continue;
        framesDecoded_.fetch_add((int64_t)(chunk.size() / (2 * kOutChannels)));

        size_t off = 0;
        while (off < chunk.size() && running_.load()) {
            off += RingWrite(chunk.data() + off, chunk.size() - off);
            if (off < chunk.size()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        written += (int64_t)chunk.size();

        // 每 2 秒打一行性能日志：平均解码耗时、进程 CPU、RSS、ring 水位
        if (t1 - lastPerfUs_ >= 2000000) {
            const int64_t cpu = CpuUs();
            const int64_t wallUs = t1 - lastPerfUs_;
            const int64_t cpuUs = cpu - lastCpuUs_;
            const int pct = wallUs > 0 ? (int)(cpuUs * 100 / wallUs) : 0;
            cpuPercent_.store(pct);
            rssKb_.store(RssKb());
            lastPerfUs_ = t1;
            lastCpuUs_ = cpu;

            const uint64_t calls = decodeCalls_.load();
            const int64_t frames = framesDecoded_.load();
            const double avgUs = frames > 0 ? (double)decodeUs_.load() / (double)frames : 0.0;
            const std::string route = decoder_ ? decoder_->Route() : "?";
            char line[260];
            snprintf(line, sizeof(line),
                     "route=%s decode_avg=%.3fms/frame calls=%llu cpu=%d%% rss=%lldKB ring=%dKB written=%lldKB",
                     route.c_str(), avgUs / 1000.0, (unsigned long long)calls, pct, rssKb_.load(),
                     (int)(RingUsed() / 1024), (long long)(written / 1024));
            OH_LOG_INFO(LOG_APP, "[player] perf %{public}s", line);
        }
    }
    OH_LOG_INFO(LOG_APP, "[player] decode loop end, pcm=%{public}lld bytes, eof=%{public}d",
                (long long)written, decoderEof_.load() ? 1 : 0);
    threadDone_.store(true);
}

// 用流式路径把整个文件解两遍：
//   A) 按源采样率/声道解 —— 校验“有没有把样本数搞多/搞少”（拉伸问题会在这里暴露）
//   B) 按 48k/2ch 解 —— 与桌面 ffmpeg 的参考哈希对拍
// 只有第一次 Open 做，避免每次重播都多解两遍。
int AudioPlayer::SelfCheck() {
    if (!pcmHash_.empty()) return 0;

    // 自检要和桌面 ffmpeg 对拍，必须走软解；硬解的 S16 舍入可能不同
    HwDisableGuard hwGuard;

    AudioInfo info;
    {
        AudioDecoder probe;
        if (probe.Open(path_, &info) < 0) {
            OH_LOG_WARN(LOG_APP, "[player] selfcheck: open failed %{public}s", probe.LastError().c_str());
            return -1;
        }
        srcRate_ = info.sampleRate;
        srcChannels_ = info.channels;

        // A) 源参数
        unsigned long long h = 1469598103934665603ULL;
        int64_t total = 0;
        std::vector<uint8_t> chunk;
        for (;;) {
            int ret = probe.ReadPcm(chunk, 4096, srcRate_, srcChannels_);
            if (ret == AVERROR_EOF) break;
            if (ret < 0) {
                OH_LOG_WARN(LOG_APP, "[player] selfcheck(src): %{public}s", probe.LastError().c_str());
                return -1;
            }
            for (size_t i = 0; i < chunk.size(); ++i) {
                h ^= chunk[i];
                h *= 1099511628211ULL;
            }
            total += (int64_t)chunk.size();
        }
        char hex[24] = {0};
        snprintf(hex, sizeof(hex), "%016llx", h);
        pcmSrcHash_ = hex;
        pcmSrcBytes_ = total;
        const int64_t expect = durationMs_ > 0
                                   ? durationMs_ * (int64_t)srcRate_ * srcChannels_ * 2 / 1000
                                   : 0;
        OH_LOG_INFO(LOG_APP,
                    "[player] selfcheck src=%{public}d/%{public}d bytes=%{public}lld expect~%{public}lld hash=%{public}s",
                    srcRate_, srcChannels_, (long long)total, (long long)expect, hex);
    }

    {
        // B) 48k/2ch
        AudioDecoder probe;
        if (probe.Open(path_, &info) < 0) return -1;
        std::vector<uint8_t> chunk;
        unsigned long long h = 1469598103934665603ULL;
        int64_t total = 0;
        int peak = 0;
        for (;;) {
            int ret = probe.ReadPcm(chunk, 8192, kOutRate, kOutChannels);
            if (ret == AVERROR_EOF) break;
            if (ret < 0) {
                OH_LOG_WARN(LOG_APP, "[player] selfcheck(out): %{public}s", probe.LastError().c_str());
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
        const int64_t expect = durationMs_ > 0 ? durationMs_ * (int64_t)kBytesPerSecond / 1000 : 0;
        OH_LOG_INFO(LOG_APP,
                    "[player] selfcheck out=48000/2 bytes=%{public}lld expect~%{public}lld peak=%{public}d hash=%{public}s",
                    (long long)total, (long long)expect, peak, hex);
    }
    return 0;
}

// ---------------- 状态机 ----------------

int AudioPlayer::Open(const std::string &path) {
    Release();
    SetError("");  // 清掉上一次会话遗留的错误串
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

    // 自检要对整个文件解两遍，只对短样例做；长文件会拖慢打开速度
    if (durationMs_ > 10000) {
        OH_LOG_INFO(LOG_APP, "[player] skip selfcheck for long file (%{public}lldms)",
                    (long long)durationMs_);
    } else {
        SelfCheck();  // 哈希对拍，失败不影响播放
    }

    if (BuildRenderer() != 0) return -1;

    OH_LOG_INFO(LOG_APP,
                "[player] open ok codec=%{public}s duration=%{public}lldms pcmBytes=%{public}lld hash=%{public}s route=%{public}s reason=%{public}s",
                codec_.c_str(), (long long)durationMs_, (long long)pcmBytes_, pcmHash_.c_str(),
                decoder_ ? decoder_->Route().c_str() : "?", decoder_ ? decoder_->RouteReason().c_str() : "");
    return 0;
}

int AudioPlayer::OpenFd(int fd) {
    // 先复制一份再 Release：重播时传进来的 fd 可能正是 srcFd_ 自己
    const int keep = ::dup(fd);
    if (keep < 0) {
        SetError("dup(fd) failed");
        return -1;
    }
    Release();
    SetError("");  // 清掉上一次会话遗留的错误串
    srcFd_ = keep;
    path_ = "fd://" + std::to_string(fd);

    decoder_.reset(new AudioDecoder());
    AudioInfo info;
    if (decoder_->OpenFd(srcFd_, &info) < 0) {
        SetError(decoder_->LastError());
        decoder_.reset();
        return -1;
    }
    codec_ = info.codec;
    durationMs_ = info.durationMs;
    srcRate_ = info.sampleRate;
    srcChannels_ = info.channels;

    // fd 来源不做 selfcheck：几十 MB 的曲目没必要解两遍
    if (BuildRenderer() != 0) return -1;

    OH_LOG_INFO(LOG_APP,
                "[player] open fd ok codec=%{public}s duration=%{public}lldms src=%{public}d/%{public}d",
                codec_.c_str(), (long long)durationMs_, srcRate_, srcChannels_);
    return 0;
}

int AudioPlayer::Play() {
    SetError("");  // 每次显式播放都从干净状态开始
    if (path_.empty()) {
        SetError("player: no file opened");
        return -1;
    }
    if (state_.load() == 3 || (decoderEof_.load() && RingUsed() == 0)) {
        if (srcFd_ >= 0) {
            if (OpenFd(srcFd_) != 0) return -1;
        } else {
            const std::string path = path_;
            if (Open(path) != 0) return -1;
        }
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
    realBytes_.store(0);
    maxCallbackBytes_.store(0);
    decodeUs_.store(0);
    decodeCalls_.store(0);
    framesDecoded_.store(0);
    cpuPercent_.store(0);
    rssKb_.store(0);

    // 先起解码线程，让 ring 预填 200ms 再开渲染器 —— 否则启动阶段必然欠载
    threadDone_.store(false);
    running_.store(true);
    thread_ = std::thread(&AudioPlayer::DecodeLoop, this);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (std::chrono::steady_clock::now() < deadline && running_.load() &&
           !decoderEof_.load() && RingUsed() < kPreloadBytes) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    OH_LOG_INFO(LOG_APP, "[player] preload ring=%{public}d bytes, eof=%{public}d",
                (int)RingUsed(), decoderEof_.load() ? 1 : 0);

    if (OH_AudioRenderer_Start(renderer_) != AUDIOSTREAM_SUCCESS) {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
        SetError("OH_AudioRenderer_Start failed");
        return -1;
    }
    state_.store(1);
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

void AudioPlayer::JoinThreadOrDetach() {
    if (!thread_.joinable()) return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (!threadDone_.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (threadDone_.load()) {
        thread_.join();
        OH_LOG_INFO(LOG_APP, "[player] decode thread joined");
        return;
    }
    // 绝不让 UI 线程无限等：detach 并放弃该 decoder（泄漏一个实例，换取不 ANR）。
    // 只有卡在 OH_AudioCodec_* 才熔断；卡在 av_read_frame 是 I/O 阻塞，允许后续继续尝试硬解。
    const char *stage = decoder_ ? decoder_->HwStageName() : "?";
    const bool ohcodecStuck = decoder_ && decoder_->HwStuckInOhCodec();
    if (ohcodecStuck) {
        OH_LOG_ERROR(LOG_APP, "[player] decode thread stuck >1.5s at %{public}s, circuit breaker ON",
                     stage);
        MarkOhCodecFatal();  // 本进程内不再创建任何 OH 解码实例
    } else {
        OH_LOG_ERROR(LOG_APP,
                     "[player] decode thread stuck >1.5s at %{public}s, I/O stall, leak thread only (no breaker)",
                     stage);
    }
    thread_.detach();
    (void)decoder_.release();
}

int AudioPlayer::Stop() {
    OH_LOG_INFO(LOG_APP, "[player] stop begin");
    running_.store(false);
    if (decoder_) decoder_->RequestStop();  // 让 ReadPcm 里的硬解轮询尽快返回
    JoinThreadOrDetach();
    OH_LOG_INFO(LOG_APP, "[player] stop joined");
    if (renderer_ != nullptr) {
        OH_AudioRenderer_Stop(renderer_);
        OH_AudioRenderer_Flush(renderer_);
    }
    OH_LOG_INFO(LOG_APP, "[player] stop renderer done");
    // 主动释放系统解码器句柄，避免下一次 Open 抢不到 DSP / 上一个实例卡在 Stop
    decoder_.reset();
    OH_LOG_INFO(LOG_APP, "[player] stop decoder released");
    RingClear();
    decoderEof_.store(false);
    gotFirstData_.store(false);
    gaps_.store(0);
    callbackCount_.store(0);
    realBytes_.store(0);
    maxCallbackBytes_.store(0);
    decodeUs_.store(0);
    decodeCalls_.store(0);
    framesDecoded_.store(0);
    cpuPercent_.store(0);
    state_.store(3);
    return 0;
}

void AudioPlayer::Release() {
    running_.store(false);
    if (decoder_) decoder_->RequestStop();
    JoinThreadOrDetach();
    if (renderer_ != nullptr) {
        OH_AudioRenderer_Stop(renderer_);
        OH_AudioRenderer_Release(renderer_);
        renderer_ = nullptr;
    }
    RingClear();
    if (decoder_) {
        OH_LOG_INFO(LOG_APP, "[player] release decoder");
        decoder_.reset();
    }
    if (srcFd_ >= 0) {
        ::close(srcFd_);
        srcFd_ = -1;
    }
    decoderEof_.store(false);
    gotFirstData_.store(false);
    gaps_.store(0);
    callbackCount_.store(0);
    realBytes_.store(0);
    maxCallbackBytes_.store(0);
    state_.store(0);
    durationMs_ = 0;
    codec_.clear();
    pcmHash_.clear();
    pcmSrcHash_.clear();
    pcmBytes_ = 0;
    pcmSrcBytes_ = 0;
    pcmPeak_ = 0;
    srcRate_ = 0;
    srcChannels_ = 0;
    path_.clear();
    rendererRate_ = rendererChannels_ = rendererFrameSize_ = 0;
}

std::string AudioPlayer::StatusJson() const {
    // 位置用官方口径：渲染器已写帧数，而不是回调次数/字节数
    int64_t framesWritten = 0;
    if (renderer_ != nullptr) OH_AudioRenderer_GetFramesWritten(renderer_, &framesWritten);
    int64_t positionMs = rendererRate_ > 0 ? framesWritten * 1000 / rendererRate_ : 0;
    if (durationMs_ > 0 && positionMs > durationMs_) positionMs = durationMs_;  // 别超过总长

    // 真实从 ring 取走的音频字节 → 音乐实际推进到哪里（不含补的静音）
    const int64_t audioMs = (int64_t)(realBytes_.load() * 1000 / kBytesPerSecond);

    uint32_t underflow = 0;
    if (renderer_ != nullptr) OH_AudioRenderer_GetUnderflowCount(renderer_, &underflow);

    const bool finished = decoderEof_.load() && RingUsed() == 0;

    const uint64_t calls = decodeCalls_.load();
    const int64_t decodedFrames = framesDecoded_.load();
    const double avgUsPerFrame = decodedFrames > 0 ? (double)decodeUs_.load() / (double)decodedFrames : 0.0;

    std::ostringstream os;
    os << "{\"state\":\"" << StateName(state_.load()) << "\""
       << ",\"positionMs\":" << positionMs
       << ",\"audioMs\":" << audioMs
       << ",\"durationMs\":" << durationMs_
       << ",\"framesWritten\":" << framesWritten
       << ",\"underflow\":" << underflow
       << ",\"gaps\":" << gaps_.load()
       << ",\"callbacks\":" << callbackCount_.load()
       << ",\"maxCallbackBytes\":" << maxCallbackBytes_.load()
       << ",\"decodeAvgUs\":" << (int64_t)avgUsPerFrame
       << ",\"decodeCalls\":" << calls
       << ",\"cpuPercent\":" << cpuPercent_.load()
       << ",\"rssKb\":" << rssKb_.load()
       << ",\"rendererRate\":" << rendererRate_
       << ",\"rendererChannels\":" << rendererChannels_
       << ",\"rendererFrameSize\":" << rendererFrameSize_
       << ",\"srcRate\":" << srcRate_
       << ",\"srcChannels\":" << srcChannels_
       << ",\"pcmHash\":\"" << pcmHash_ << "\""
       << ",\"pcmBytes\":" << pcmBytes_
       << ",\"pcmPeak\":" << pcmPeak_
       << ",\"pcmSrcBytes\":" << pcmSrcBytes_
       << ",\"finished\":" << (finished ? "true" : "false")
       << ",\"route\":\"" << (decoder_ ? decoder_->Route() : "") << "\""
       << ",\"routeReason\":\"" << (decoder_ ? decoder_->RouteReason() : "") << "\""
       << ",\"error\":\"" << LastError() << "\"}";
    return os.str();
}
