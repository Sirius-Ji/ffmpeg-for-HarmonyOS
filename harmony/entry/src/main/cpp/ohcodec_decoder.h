#ifndef FF_OHCODEC_DECODER_H
#define FF_OHCODEC_DECODER_H

#include <cstdint>
#include <string>
#include <vector>

struct OH_AVCodec;

// M3：系统 OH_AudioCodec（同步模式）解码后端，支持 AAC / MP3 / FLAC。
// 输入是「裸码流」：AAC 需要 ADTS，MP3 / FLAC 直接用 demuxer 出来的包。
class OhCodecDecoder {
public:
    ~OhCodecDecoder();

    // L0 能力检查：系统有没有这个 mime 的解码器、是否支持这个采样率
    static bool CapabilityAllows(const char *mime, int sampleRate, int channels, std::string *why);

    // 创建 + Configure + Prepare + Start；失败返回负值并写 why
    int Open(const char *mime, int sampleRate, int channels, const uint8_t *extradata,
             int extradataSize, std::string *why);
    // 同步销毁（正常路径）
    void Close();
    // 异步销毁：把 Flush/Stop/Destroy 丢到 detached 线程，调用方立刻返回。
    // 用于实例已经卡死、不能再让解码线程等它的场景。
    void AbandonAsync();
    bool IsOpen() const { return codec_ != nullptr; }
    const char *mime() const { return mime_; }

    // 推送一包裸码流。
    //   0  = 已推入
    //   1  = 输入缓冲暂不可用（调用方先拉输出，再重试）
    //  <0  = 错误
    int Push(const uint8_t *data, int size, int64_t ptsUs, std::string *why);

    // 推 EOS 空包；同样可能返回 1（需要先拉输出再重试）
    int SendEos(std::string *why);

    // 拉一帧 PCM（S16LE 交错）。
    //   0  = 有数据
    //   1  = 暂时没有（超时/稍后再试）
    //   2  = 收到 EOS
    //  <0  = 错误
    int Pull(std::vector<uint8_t> *pcm, int *samples, std::string *why);

    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }

    // 统计：推入的包数/字节，输出的字节/采样数（用来核对 PCM 帧数是否对得上）
    long long pushedPackets() const { return pushedPackets_; }
    long long pushedBytes() const { return pushedBytes_; }
    long long outBytes() const { return outBytes_; }
    long long outSamples() const { return outSamples_; }

private:
    OH_AVCodec *codec_ = nullptr;
    const char *mime_ = "";
    int sampleRate_ = 0;
    int channels_ = 0;
    bool eosSent_ = false;
    bool started_ = false;
    long long pushedPackets_ = 0;
    long long pushedBytes_ = 0;
    long long outBytes_ = 0;
    long long outSamples_ = 0;
};

#endif  // FF_OHCODEC_DECODER_H
