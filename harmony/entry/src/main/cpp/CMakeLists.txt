cmake_minimum_required(VERSION 3.5.0)
project(ffmpegaudio)

set(NATIVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# FFmpeg 产物位置：harmony/entry/libs/<abi>/{lib*.so.*, include/}
# 库文件名带大版本号，和 SONAME 一致：libavcodec.so.63 等。
set(FFMPEG_ROOT ${NATIVE_ROOT}/../../../libs/${OHOS_ARCH})
set(FFMPEG_INCLUDE ${FFMPEG_ROOT}/include)

if(NOT EXISTS ${FFMPEG_ROOT}/libavcodec.so.63)
    message(FATAL_ERROR
        "找不到 FFmpeg 产物: ${FFMPEG_ROOT}\n"
        "先在仓库根目录跑：bash build/build-ffmpeg.sh all && bash build/install-into-hap.sh")
endif()

# ---- FFmpeg 的 IMPORTED 目标 ----
# 用 IMPORTED 而不是直接写库路径，好处是接口和依赖关系清晰，
# 以后换成静态库 / 官方 so 只改这里。
add_library(ff_avformat SHARED IMPORTED)
set_target_properties(ff_avformat PROPERTIES
    IMPORTED_LOCATION ${FFMPEG_ROOT}/libavformat.so.63
    INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_INCLUDE})

add_library(ff_avcodec SHARED IMPORTED)
set_target_properties(ff_avcodec PROPERTIES
    IMPORTED_LOCATION ${FFMPEG_ROOT}/libavcodec.so.63
    INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_INCLUDE})

add_library(ff_avutil SHARED IMPORTED)
set_target_properties(ff_avutil PROPERTIES
    IMPORTED_LOCATION ${FFMPEG_ROOT}/libavutil.so.61
    INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_INCLUDE})

add_library(ff_swresample SHARED IMPORTED)
set_target_properties(ff_swresample PROPERTIES
    IMPORTED_LOCATION ${FFMPEG_ROOT}/libswresample.so.7
    INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_INCLUDE})

# ---- NAPI 模块 ----
add_library(entry SHARED
    napi_init.cpp
    decoder.cpp)

target_include_directories(entry PRIVATE ${NATIVE_ROOT})

target_link_libraries(entry PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    ff_avformat
    ff_avcodec
    ff_avutil
    ff_swresample)

# 链接时用 lib 目录解析传递依赖（libavcodec 依赖 libswresample 等）
target_link_options(entry PRIVATE "-Wl,-rpath-link,${FFMPEG_ROOT}")
