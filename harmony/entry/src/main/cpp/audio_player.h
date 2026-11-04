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
// 输出格式固定 S16LE / 48 kHz / 2ch —— 任何源都在 swr 里归一化。
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    int Open(const std::string &path);
    int Play();
    int Pause();
    int Stop();
    void Release();

    std::string StatusJson() const;
    std::string LastError() const;

private:
    static int32_t OnWriteDataCb(OH_AudioRenderer *renderer, void *userData,
                                 void *audioData, int32_t audioDataSize);
    int FillAudio(uint8_t *buf, int32_t size);
    void DecodeLoop();
    int BuildRenderer();
    int SelfCheck();  // 用流式路径整解一遍算哈希，用来和桌面参考值对拍
    void SetError(const std::string &what);

    size_t RingWrite(const uint8_t *data, size_t len);
    size_t RingRead(uint8_t *data, size_t len);
    size_t RingUsed() const;
    void RingClear();

    std::unique_ptr<AudioDecoder> decoder_;
    OH_AudioRenderer *renderer_ = nullptr;
    std::thread thread_;

    std::atomic<bool> running_{false};
    std::atomic<bool> decoderEof_{false};
    std::atomic<bool> gotFirstData_{false};
    std::atomic<int> state_{0};              // 0 idle / 1 playing / 2 paused / 3 stopped
    std::atomic<uint64_t> gaps_{0};          // 我们主动补静音的缺口次数
    std::atomic<uint64_t> callbackCount_{0}; // 回调次数（诊断用）
    std::atomic<uint64_t> realBytes_{0};     // 真正从 ring 取走的数据字节（音乐进度）
    std::atomic<int> maxCallbackBytes_{0};   // 回调要求过的最大块（诊断回调粒度）

    mutable std::mutex ringMutex_;
    std::vector<uint8_t> ring_;
    size_t ringRead_ = 0;
    size_t ringWrite_ = 0;
    size_t ringUsed_ = 0;

    // 渲染器实际参数（构建后用 Get* 回读，确认配置真的生效）
    int rendererRate_ = 0;
    int rendererChannels_ = 0;
    int rendererFrameSize_ = 0;
    int rendererFormat_ = 0;
    int rendererEncoding_ = 0;

    // 流式解码自检结果
    std::string pcmHash_;
    int64_t pcmBytes_ = 0;
    int pcmPeak_ = 0;
    std::string pcmSrcHash_;   // 按源采样率/声道解出的哈希（与 M1 整文件路径对齐用）
    int64_t pcmSrcBytes_ = 0;
    int srcRate_ = 0;
    int srcChannels_ = 0;

    std::string path_;
    std::string codec_;
    int64_t durationMs_ = 0;

    mutable std::mutex errMutex_;
    std::string err_;
};

#endif  // FF_AUDIO_PLAYER_H
