#ifndef FF_TRANSCODER_H
#define FF_TRANSCODER_H

#include <string>

// 把 srcFd 里第一条音轨解码 → 重采样到 48 kHz/2ch → AAC 编码 → ipod(m4a) 封装 → 写入 dstFd。
// 两个 fd 都由调用方持有；内部各 dup 一份，函数返回后调用方可以立刻 close。
// 成功返回 0 并填 summary（JSON）；失败返回负值并填 err。
int TranscodeToAac(int srcFd, int dstFd, std::string *summary, std::string *err);

#endif  // FF_TRANSCODER_H
