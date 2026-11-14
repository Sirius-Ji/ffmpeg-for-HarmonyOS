#ifndef FF_VIDEO_PLAYER_H
#define FF_VIDEO_PLAYER_H

#include <string>

// M4.1：把一个本地 mp4 的 H.264 视频轨解到 XComponent 的 surface 上。
// 成功只代表播放线程已启动；实际首帧/错误看 hilog（TAG FFM1）。
int VideoPlay(const std::string &path, const std::string &surfaceId, std::string *result);
void VideoStop();

#endif  // FF_VIDEO_PLAYER_H
