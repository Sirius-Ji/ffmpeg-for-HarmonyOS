# ffmpeg-ohos-sdk v0.2.1

LGPL-2.1-or-later FFmpeg SDK for HarmonyOS NEXT (arm64-v8a / x86_64).

## Layout

```
include/                 FFmpeg + freetype + harfbuzz + lame headers
lib/arm64-v8a/           7 shared libraries + libffz.so.1
lib/x86_64/
CMakeLists.txt           add_subdirectory / include example
BUILD-INFO.txt           ABI, SONAME and checksum manifest
```

## CMake

```cmake
add_subdirectory(ffmpeg-ohos-sdk)
target_link_libraries(your_target PRIVATE ffmpeg_ohos_sdk)
```

`libc++_shared.so` is intentionally not shipped; the host app must provide the
NDK C++ runtime matching its own toolchain.

`libffz.so.1` is the renamed zlib (SONAME `libffz.so.1`) and is a runtime
dependency of `libavcodec.so.63`.
