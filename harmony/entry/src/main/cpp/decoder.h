#ifndef FF_AUDIO_DECODER_H
#define FF_AUDIO_DECODER_H

#include <cstdint>
#include <string>

struct AudioInfo {
    std::string format;
    std::string codec;
    int sampleRate = 0;
    int channels = 0;
    int64_t durationMs = 0;
    int64_t bitRate = 0;
};

// 只解封装不解码时的统计（M1 的 "read" 一步）
struct ReadStats {
    int64_t packets = 0;
    int64_t bytes = 0;
    int64_t firstPtsMs = 0;
    int64_t lastPtsMs = 0;
};

// M1 的最小解码器：avformat 解封装 + avcodec 软解 + swresample 统一成 S16 交错。
// M3 会在这层下面加 OH_AudioCodec 后端，接口保持不变。
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    // 打开文件并建立解码器；成功返回 0
    int Open(const std::string &path, AudioInfo *info);

    // 整文件解到 16-bit PCM WAV（用于和桌面 ffmpeg 对拍）
    int DecodeAllToWav(const std::string &outPath, int64_t *framesOut, int64_t *bytesOut);

    // 只读封装：数一数音频包的个数/字节/PTS 范围，不解码
    int ReadPackets(ReadStats *out);

    // 把当前解码器的信息以 JSON 形式返回，方便 ArkTS 直接展示
    std::string InfoJson(const AudioInfo &info) const;

    bool IsOpen() const { return fmt_ != nullptr; }
    void Close();
    const std::string &LastError() const { return err_; }
    // 解出 PCM 的 FNV-1a 64 校验和（和桌面端脚本比对用）
    unsigned long long LastHash() const { return hash_; }

private:
    int DrainDecoder(void *wav);
    void SetError(const std::string &what);

    void *fmt_ = nullptr;        // AVFormatContext*
    void *dec_ = nullptr;        // AVCodecContext*
    void *frame_ = nullptr;      // AVFrame*
    void *packet_ = nullptr;     // AVPacket*
    void *swr_ = nullptr;        // SwrContext*
    int streamIndex_ = -1;
    int outChannels_ = 0;
    int outSampleRate_ = 0;
    unsigned long long hash_ = 0;
    std::string err_;
};

#endif  // FF_AUDIO_DECODER_H
