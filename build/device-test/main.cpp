// 设备端自检程序：直接跑在 /data/local/tmp 里，不经过 HAP。
// 用法: ./ffmtest <file1> [file2 ...]
#include <cstdio>
#include <cstring>

#include "decoder.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <audio-file> [...]\n", argv[0]);
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        AudioDecoder decoder;
        AudioInfo info;
        int ret = decoder.Open(argv[i], &info);
        if (ret < 0) {
            printf("{\"file\":\"%s\",\"ok\":false,\"error\":\"%s\"}\n", argv[i], decoder.LastError().c_str());
            ++failures;
            continue;
        }
        char wav[1024];
        snprintf(wav, sizeof(wav), "%s.wav", argv[i]);
        int64_t frames = 0;
        int64_t bytes = 0;
        ret = decoder.DecodeAllToWav(wav, &frames, &bytes);
        printf("{\"file\":\"%s\",\"ok\":%s,\"codec\":\"%s\",\"format\":\"%s\",\"rate\":%d,\"ch\":%d,"
               "\"frames\":%lld,\"bytes\":%lld,\"hash\":\"%016llx\"}\n",
               argv[i], ret == 0 ? "true" : "false", info.codec.c_str(), info.format.c_str(),
               info.sampleRate, info.channels, (long long)frames, (long long)bytes,
               (unsigned long long)decoder.LastHash());
        if (ret != 0) ++failures;
    }
    return failures == 0 ? 0 : 1;
}
