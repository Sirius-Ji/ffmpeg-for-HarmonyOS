#ifndef FF_TRANSCODER_H
#define FF_TRANSCODER_H

#include <string>

// 把 srcFd 里第一条音轨解码 → 重采样到 48 kHz/2ch → 编码 → 封装 → 写入 dstFd。
// format: "aac"（mp4/m4a）、"mp3"（libmp3lame，mp3）、"opus"（原生 opus，opus/ogg）。
// 两个 fd 都由调用方持有；内部各 dup 一份，函数返回后调用方可以立刻 close。
// 成功返回 0 并填 summary（JSON）；失败返回负值并填 err。
int TranscodeAudio(int srcFd, int dstFd, const std::string &format, std::string *summary, std::string *err);

// 兼容旧调用：等价于 format="aac"
int TranscodeToAac(int srcFd, int dstFd, std::string *summary, std::string *err);

#endif  // FF_TRANSCODER_H
