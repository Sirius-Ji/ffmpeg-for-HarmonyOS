#ifndef FF_AUDIO_DECODER_H
#define FF_AUDIO_DECODER_H

#include <cstdint>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

class OhCodecDecoder;

// M3.1：硬件解码总开关（默认开）。关掉后强制走 FFmpeg，用于 A/B 对比。
void SetHardwareDecodeEnabled(bool on);
bool HardwareDecodeEnabled();

// M3：熔断。一旦硬解线程卡死，本进程内不再创建任何 OH 解码实例，全部走软解。
void MarkOhCodecFatal();
bool OhCodecFatal();

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

    // 让正在 ReadPcm 的线程尽快返回（Stop 时先调用它，再 join）
    void RequestStop() { stopRequested_ = true; }

    // 把当前解码器的信息以 JSON 形式返回，方便 ArkTS 直接展示
    std::string InfoJson(const AudioInfo &info) const;

    bool IsOpen() const { return fmt_ != nullptr; }
    void Close();
    const std::string &LastError() const { return err_; }
    // 实际走的解码路径：ffmpeg / ohcodec / ffmpeg(fallback)
    const std::string &Route() const { return route_; }
    const std::string &RouteReason() const { return routeWhy_; }
    // 硬解线程当前卡在哪个系统调用（熔断时用来打日志）
    const char *HwStageName() const;
    // 卡在 OH_AudioCodec_* 才算硬解故障；卡在 av_read_frame 属于 I/O 阻塞
    bool HwStuckInOhCodec() const;
    // 解出 PCM 的 FNV-1a 64 校验和（和桌面端脚本比对用）
    unsigned long long LastHash() const { return hash_; }

private:
    int DrainDecoder(void *wav);
    void SetError(const std::string &what);
    // Open / OpenFd 共用：找流、建解码器、填 info
    int FinishOpen(AudioInfo *info);

    // 流式路径：把下一帧（或 swr 残留）转换进 pending_，供 ReadPcm 取用
    int StreamFillPending(int outRate, int outChannels);

    // M3：AAC / MP3 / FLAC 硬解后端
    int HwFillPending(int outRate, int outChannels);
    int HwSwrToPending(const uint8_t *data, int samples, int srcRate, int outRate, int outChannels);
    void BuildElementary(void *pkt);
    void FallbackToSoftware(const std::string &why);

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

    // ---- M3 硬解状态 ----
    std::unique_ptr<OhCodecDecoder> hw_;
    void *swrHw_ = nullptr;
    std::vector<uint8_t> adts_;
    int64_t hwLastPtsUs_ = 0;
    int hwRate_ = 0;
    int hwCh_ = 0;
    bool hwEosSent_ = false;
    bool hwGotEos_ = false;       // 系统是否回显了 EOS 标记
    int hwEosIdle_ = 0;           // EOS 之后连续空轮询次数（1 次 ≈ 1ms）
    int hwEosTry_ = 0;            // 尝试送 EOS 的次数，超限就跳过 EOS 直接收尾
    int hwDeadlineExt_ = 0;       // 尾部数据不足时，延长总预算的次数
    int64_t hwExpectedSamples_ = 0;  // 按容器时长估算的期望输出采样数
    int64_t hwSrcSamplesOut_ = 0;    // 已从硬解取出的源采样数（用于按容器时长截断）
    std::atomic<int> hwStage_{0};  // 0 idle / 1 pull / 2 read / 3 eos / 4 push / 5 fallback / 6 close
    std::atomic<bool> stopRequested_{false};
    std::string route_ = "ffmpeg";
    std::string routeWhy_;
};

#endif  // FF_AUDIO_DECODER_H
