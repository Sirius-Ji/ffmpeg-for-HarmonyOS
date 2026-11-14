#ifndef FF_HARDWARE_PROBE_H
#define FF_HARDWARE_PROBE_H

#include <string>

// M3.0 探针：查系统 OH_AudioCodec 对 AAC / MP3 / FLAC 的能力，
// 返回 JSON（能力是否存在、支持的采样率、在 44.1/48/96k 上 Configure/Prepare/Start 的返回码）。
// 成功返回 0。
int HardwareProbe(std::string *json);

#endif  // FF_HARDWARE_PROBE_H
