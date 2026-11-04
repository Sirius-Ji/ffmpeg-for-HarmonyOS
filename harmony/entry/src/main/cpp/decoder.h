#ifndef FF_AUDIO_DECODER_H
#define FF_AUDIO_DECODER_H

#include <cstdint>
#include <string>
#include <vector>

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

// 音频解码器：avformat 解封装 + avcodec 软解 + swresample 统一成 S16 交错。
// 两种用法：
//   1) DecodeAllToWav / ReadPackets —— M1 的整文件自检与解封装统计
//   2) ReadPcm —— M2 的流式拉取，给播放器按块取 PCM
// M3 会在这层下面加 OH_AudioCodec 后端，对外接口保持不变。
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    // 打开文件并建立解码器；成功返回 0
    int Open(const std::string &path, AudioInfo *info);

    // 用已打开的 fd 打开（picker 选出的文件是 URI，转成 fd 后走这里，不用复制文件）。
    // 内部会 dup 一份，调用方可以立刻 close 自己的 fd。
    int OpenFd(int fd, AudioInfo *info);

    // 整文件解到 16-bit PCM WAV（用于和桌面 ffmpeg 对拍）
    int DecodeAllToWav(const std::string &outPath, int64_t *framesOut, int64_t *bytesOut);

    // 只读封装：数一数音频包的个数/字节/PTS 范围，不解码
    int ReadPackets(ReadStats *out);

    // 流式解码：拉一块 S16 交错 PCM 到 out，统一重采样到 outRate / outChannels。
    // 返回 0 = 有数据；AVERROR_EOF = 播完了；其他负数 = 出错。
    int ReadPcm(std::vector<uint8_t> &out, int maxFrames, int outRate, int outChannels);
    bool Eof() const { return eof_; }

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
    // Open / OpenFd 共用：找流、建解码器、填 info
    int FinishOpen(AudioInfo *info);

    // 流式路径：把下一帧（或 swr 残留）转换进 pending_，供 ReadPcm 取用
    int StreamFillPending(int outRate, int outChannels);

    void *fmt_ = nullptr;        // AVFormatContext*
    void *dec_ = nullptr;        // AVCodecContext*
    void *frame_ = nullptr;      // AVFrame*
    void *packet_ = nullptr;     // AVPacket*
    void *swr_ = nullptr;        // SwrContext*（整文件解码用）
    void *swrStream_ = nullptr;  // SwrContext*（流式解码用）
    void *avio_ = nullptr;       // AVIOContext*（fd 打开时自建，Close 里连同 buffer 一起释放）
    int fd_ = -1;                // dup 出来的 fd，归解码器所有
    std::vector<uint8_t> pending_;
    size_t pendingPos_ = 0;
    bool flushSent_ = false;
    bool eof_ = false;
    int streamIndex_ = -1;
    int outChannels_ = 0;
    int outSampleRate_ = 0;
    unsigned long long hash_ = 0;
    std::string err_;
};

#endif  // FF_AUDIO_DECODER_H
