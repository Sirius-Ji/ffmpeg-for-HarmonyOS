#ifndef FF_VIDEO_TRANSCODE_H
#define FF_VIDEO_TRANSCODE_H

#include <string>

// M4.3：FFmpeg 解封装/解码 → OH_VideoEncoder(h264_oh) 硬编 + AAC 软编 → mp4 封装。
int VideoTranscodeToMp4(const std::string &srcPath, const std::string &dstPath,
                        std::string *result);
std::string VideoTranscodeStatusJson();

#endif
