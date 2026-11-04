#ifndef FF_AUDIO_PLAYER_H
#define FF_AUDIO_PLAYER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "decoder.h"

// OHAudio 的类型定义：OH_AudioRenderer 是 struct OH_AudioRendererStruct 的 typedef，
// 不能自己 forward declare（会与 SDK 冲突），所以直接引基础头文件。
#include <ohaudio/native_audiostream_base.h>

// M2 的最小播放器：解码线程 + 环形缓冲 + OHAudio 数据写入回调。
// 输出格式固定 S16LE / 48 kHz / 2ch —— 任何源都在 swr 里归一化，
// 这样 OHAudio 的配置不用跟着源变，播放器状态机简单。
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    // 打开文件并建好 renderer；成功返回 0
    int Open(const std::string &path);
    int Play();
    int Pause();
    int Stop();
    void Release();

    std::string StatusJson() const;
    const std::string &LastError() const { return err_; }

private:
    static int32_t OnWriteDataCb(OH_AudioRenderer *renderer, void *userData,
                                 void *audioData, int32_t audioDataSize);
    int FillAudio(uint8_t *buf, int32_t size);
    void DecodeLoop();
    int BuildRenderer();
    void SetError(const std::string &what);

    // 环形缓冲（单读单写，都加锁，量级小无所谓）
    size_t RingWrite(const uint8_t *data, size_t len);
    size_t RingRead(uint8_t *data, size_t len);
    size_t RingUsed() const;
    void RingClear();

    std::unique_ptr<AudioDecoder> decoder_;
    OH_AudioRenderer *renderer_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> playedBytes_{0};
    std::atomic<int> state_{0};  // 0 idle / 1 playing / 2 paused / 3 stopped

    mutable std::mutex ringMutex_;
    std::vector<uint8_t> ring_;
    size_t ringRead_ = 0;
    size_t ringWrite_ = 0;
    size_t ringUsed_ = 0;

    std::string path_;
    std::string codec_;
    int64_t durationMs_ = 0;
    std::string err_;
};

#endif  // FF_AUDIO_PLAYER_H
