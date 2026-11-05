# find_package(ffmpeg_ohos) 的入口文件（CMake 会搜索 <pkg>-config.cmake）。
# 实际内容在 ffmpeg-ohos-config.cmake，保持文件名与 Release 文档一致。
include("${CMAKE_CURRENT_LIST_DIR}/ffmpeg-ohos-config.cmake")
