#ifndef FF_VIDEO_TRANSCODE_H
#define FF_VIDEO_TRANSCODE_H

#include <string>

// M4.3：FFmpeg 解封装/解码 → OH_VideoEncoder(h264_oh) 硬编 + AAC 软编 → mp4 封装。
int ImageProbe(const std::string &path, std::string *result);
int VideoTranscode(const std::string &srcPath, const std::string &dstPath,
                   const std::string &format, const std::string &resolution,
                   float volume, const std::string &imagePath, const std::string &optionsJson,
                   std::string *result);
std::string VideoTranscodeStatusJson();

#endif
