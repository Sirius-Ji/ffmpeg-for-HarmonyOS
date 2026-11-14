#ifndef FF_VIDEO_PROBE_H
#define FF_VIDEO_PROBE_H

#include <string>

// M4.0：探测系统视频硬解能力（H.264 / H.265）与最大分辨率。
// 不会真正解码；只查 capability + 尝试 CreateByMime/Destroy。
int VideoProbe(std::string *json);

#endif  // FF_VIDEO_PROBE_H
