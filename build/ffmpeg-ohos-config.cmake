# ffmpeg-ohos-config.cmake — FFmpeg for HarmonyOS 的 CMake 包配置
#
# 用法（解压目录 = 包含 include/ 与 lib/ 的那一级）：
#   list(APPEND CMAKE_PREFIX_PATH /path/to/ffmpeg-ohos/<abi>)
#   find_package(ffmpeg_ohos REQUIRED)
#   target_link_libraries(entry PRIVATE ffmpeg_ohos::ffmpeg)
#
# 提供：
#   ffmpeg_ohos::ffmpeg          INTERFACE imported target（include + 5 个 .so + rpath-link）
#   FFMPEG_OHOS_INCLUDE_DIR
#   FFMPEG_OHOS_LIBRARY_DIR
#   FFMPEG_OHOS_LIBRARIES        5 个 .so 的绝对路径（含 libmp3lame.so.0）

get_filename_component(_ffmpeg_ohos_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

set(FFMPEG_OHOS_INCLUDE_DIR "${_ffmpeg_ohos_root}/include")
set(FFMPEG_OHOS_LIBRARY_DIR "${_ffmpeg_ohos_root}/lib")

set(_ffmpeg_ohos_libs
    "${FFMPEG_OHOS_LIBRARY_DIR}/libavformat.so.63"
    "${FFMPEG_OHOS_LIBRARY_DIR}/libavcodec.so.63"
    "${FFMPEG_OHOS_LIBRARY_DIR}/libswresample.so.7"
    "${FFMPEG_OHOS_LIBRARY_DIR}/libavutil.so.61"
    "${FFMPEG_OHOS_LIBRARY_DIR}/libmp3lame.so.0")

foreach(_ffmpeg_ohos_lib IN LISTS _ffmpeg_ohos_libs)
  if(NOT EXISTS "${_ffmpeg_ohos_lib}")
    set(ffmpeg_ohos_FOUND FALSE)
    set(ffmpeg_ohos_NOT_FOUND_MESSAGE "ffmpeg_ohos: missing ${_ffmpeg_ohos_lib}")
    return()
  endif()
endforeach()

set(FFMPEG_OHOS_LIBRARIES "${_ffmpeg_ohos_libs}")

if(NOT TARGET ffmpeg_ohos::ffmpeg)
  add_library(ffmpeg_ohos::ffmpeg INTERFACE IMPORTED)
  set_target_properties(ffmpeg_ohos::ffmpeg PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_OHOS_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES "${_ffmpeg_ohos_libs}"
      INTERFACE_LINK_OPTIONS "LINKER:-rpath-link,${FFMPEG_OHOS_LIBRARY_DIR}")
endif()

set(ffmpeg_ohos_FOUND TRUE)
