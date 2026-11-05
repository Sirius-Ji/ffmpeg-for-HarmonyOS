// M1/M2 的 NAPI 层：
//   M1 —— open（拿信息）、read（解封装统计）、decode（落 WAV + 算哈希）
//   M2 —— playerOpen/Play/Pause/Stop/Status/Release（OHAudio 出声）
// 关键步骤都往 HiLog 打一行，tag = FFM1，控制台过滤即可。
#include <hilog/log.h>
#include <napi/native_api.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "audio_player.h"
#include "decoder.h"
#include "hardware_probe.h"
#include "transcoder.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xFF01
#define LOG_TAG "FFM1"

namespace {

std::string JsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

std::string GetString(napi_env env, napi_value value) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return {};
    std::vector<char> buf(len + 1, 0);
    if (napi_get_value_string_utf8(env, value, buf.data(), buf.size(), &len) != napi_ok) return {};
    return std::string(buf.data(), len);
}

napi_value MakeString(napi_env env, const std::string &s) {
    napi_value out = nullptr;
    napi_create_string_utf8(env, s.c_str(), s.size(), &out);
    return out;
}

napi_value ErrorJson(napi_env env, const std::string &what) {
    OH_LOG_ERROR(LOG_APP, "[error] %{public}s", what.c_str());
    return MakeString(env, "{\"error\":\"" + JsonEscape(what) + "\"}");
}

// open: 打开文件、建解码器、把流信息打到 HiLog
napi_value Probe(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) return ErrorJson(env, "probe(path): missing path");

    const std::string path = GetString(env, argv[0]);
    OH_LOG_INFO(LOG_APP, "[open] path=%{public}s", path.c_str());

    AudioDecoder decoder;
    AudioInfo audioInfo;
    int ret = decoder.Open(path, &audioInfo);
    if (ret < 0) return ErrorJson(env, decoder.LastError());

    OH_LOG_INFO(LOG_APP,
                "[open] format=%{public}s codec=%{public}s rate=%{public}d ch=%{public}d "
                "duration=%{public}lldms bitrate=%{public}lld",
                audioInfo.format.c_str(), audioInfo.codec.c_str(), audioInfo.sampleRate,
                audioInfo.channels, (long long)audioInfo.durationMs, (long long)audioInfo.bitRate);
    return MakeString(env, decoder.InfoJson(audioInfo));
}

// read: 只解封装，不解码，统计音频包
napi_value ReadPackets(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) return ErrorJson(env, "readPackets(path): missing path");

    const std::string path = GetString(env, argv[0]);
    AudioDecoder decoder;
    AudioInfo audioInfo;
    int ret = decoder.Open(path, &audioInfo);
    if (ret < 0) return ErrorJson(env, decoder.LastError());

    ReadStats stats;
    ret = decoder.ReadPackets(&stats);
    if (ret < 0) return ErrorJson(env, decoder.LastError());

    OH_LOG_INFO(LOG_APP,
                "[read] packets=%{public}lld bytes=%{public}lld pts=%{public}lld..%{public}lldms eof=1",
                (long long)stats.packets, (long long)stats.bytes,
                (long long)stats.firstPtsMs, (long long)stats.lastPtsMs);

    std::string json = "{\"packets\":" + std::to_string(stats.packets) +
                       ",\"bytes\":" + std::to_string(stats.bytes) +
                       ",\"firstPtsMs\":" + std::to_string(stats.firstPtsMs) +
                       ",\"lastPtsMs\":" + std::to_string(stats.lastPtsMs) + "}";
    return MakeString(env, json);
}

// decode: 整文件解成 S16 WAV，并算 FNV-1a 64 供和桌面比对
napi_value DecodeToWav(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) return ErrorJson(env, "decodeToWav(path, wavPath): need 2 args");

    const std::string path = GetString(env, argv[0]);
    const std::string wavPath = GetString(env, argv[1]);

    AudioDecoder decoder;
    AudioInfo audioInfo;
    int ret = decoder.Open(path, &audioInfo);
    if (ret < 0) return ErrorJson(env, decoder.LastError());

    int64_t frames = 0;
    int64_t bytes = 0;
    ret = decoder.DecodeAllToWav(wavPath, &frames, &bytes);
    if (ret < 0) return ErrorJson(env, decoder.LastError());

    char hashHex[24] = {0};
    snprintf(hashHex, sizeof(hashHex), "%016llx", (unsigned long long)decoder.LastHash());

    OH_LOG_INFO(LOG_APP,
                "[decode] codec=%{public}s frames=%{public}lld bytes=%{public}lld hash=%{public}s wav=%{public}s",
                audioInfo.codec.c_str(), (long long)frames, (long long)bytes, hashHex, wavPath.c_str());

    std::string json = "{\"frames\":" + std::to_string(frames) +
                       ",\"bytes\":" + std::to_string(bytes) +
                       ",\"hash\":\"" + std::string(hashHex) + "\"" +
                       ",\"wav\":\"" + JsonEscape(wavPath) + "\"" +
                       ",\"codec\":\"" + JsonEscape(audioInfo.codec) + "\"" +
                       ",\"sampleRate\":" + std::to_string(audioInfo.sampleRate) + "}";
    return MakeString(env, json);
}

// ---------------- M2：播放器 ----------------
// M2 先做单实例：一个进程一个播放器，够验证链路，接口也简单。

std::unique_ptr<AudioPlayer> g_player;

AudioPlayer *EnsurePlayer() {
    if (!g_player) g_player.reset(new AudioPlayer());
    return g_player.get();
}

// player* 统一返回 { ok: boolean, data: string }。
// 不要用"JSON 里有没有 error 字段"来判断成败——StatusJson 永远带 error:""。
napi_value MakeResult(napi_env env, bool ok, const std::string &payload) {
    napi_value obj = nullptr;
    napi_create_object(env, &obj);
    napi_value okVal = nullptr;
    napi_get_boolean(env, ok, &okVal);
    napi_set_named_property(env, obj, "ok", okVal);
    napi_value dataVal = nullptr;
    napi_create_string_utf8(env, payload.c_str(), payload.size(), &dataVal);
    napi_set_named_property(env, obj, "data", dataVal);
    return obj;
}

napi_value ResultError(napi_env env, const std::string &what) {
    OH_LOG_ERROR(LOG_APP, "[error] %{public}s", what.c_str());
    return MakeResult(env, false, "{\"error\":\"" + JsonEscape(what) + "\"}");
}

napi_value PlayerOpen(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) return ResultError(env, "playerOpen(path): missing path");

    const std::string path = GetString(env, argv[0]);
    AudioPlayer *player = EnsurePlayer();
    if (player->Open(path) != 0) return ResultError(env, player->LastError());

    OH_LOG_INFO(LOG_APP, "[player] open %{public}s", path.c_str());
    return MakeResult(env, true, player->StatusJson());
}

// 用 fd 打开：ArkTS 侧 picker 拿到 URI → fs.openSync(uri) → 把 fd 传进来
napi_value PlayerOpenFd(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) return ResultError(env, "playerOpenFd(fd): missing fd");

    int32_t fd = -1;
    if (napi_get_value_int32(env, argv[0], &fd) != napi_ok || fd < 0) {
        return ResultError(env, "playerOpenFd(fd): invalid fd");
    }

    AudioPlayer *player = EnsurePlayer();
    OH_LOG_INFO(LOG_APP, "[player] playerOpenFd fd=%{public}d", fd);
    if (player->OpenFd(fd) != 0) return ResultError(env, player->LastError());

    OH_LOG_INFO(LOG_APP, "[player] open via fd %{public}d ok", fd);
    return MakeResult(env, true, player->StatusJson());
}

// 转码示例：src fd + dst fd → AAC(ipod/m4a)。两个 fd 调用方持有，这里内部 dup。
napi_value TranscodeToAacFn(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) return ResultError(env, "transcodeToAac(srcFd, dstFd): 参数不足");

    int32_t srcFd = -1;
    int32_t dstFd = -1;
    if (napi_get_value_int32(env, argv[0], &srcFd) != napi_ok || srcFd < 0 ||
        napi_get_value_int32(env, argv[1], &dstFd) != napi_ok || dstFd < 0) {
        return ResultError(env, "transcodeToAac(srcFd, dstFd): fd 非法");
    }

    std::string summary;
    std::string err;
    OH_LOG_INFO(LOG_APP, "[transcode] start srcFd=%{public}d dstFd=%{public}d", srcFd, dstFd);
    const int rc = TranscodeToAac(srcFd, dstFd, &summary, &err);
    if (rc != 0) return ResultError(env, err.empty() ? "transcode failed" : err);
    return MakeResult(env, true, summary);
}

// 通用转码：src fd + dst fd + 输出格式（aac / mp3 / opus / flac / wav）
bool IsSupportedFormat(const std::string &format) {
    return format == "aac" || format == "mp3" || format == "opus" ||
           format == "flac" || format == "wav";
}

napi_value TranscodeToFn(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) return ResultError(env, "transcodeTo(srcFd, dstFd, format): 参数不足");

    int32_t srcFd = -1;
    int32_t dstFd = -1;
    if (napi_get_value_int32(env, argv[0], &srcFd) != napi_ok || srcFd < 0 ||
        napi_get_value_int32(env, argv[1], &dstFd) != napi_ok || dstFd < 0) {
        return ResultError(env, "transcodeTo(srcFd, dstFd, format): fd 非法");
    }
    const std::string format = GetString(env, argv[2]);
    if (!IsSupportedFormat(format)) {
        return ResultError(env, "transcodeTo: format 只支持 aac/mp3/opus/flac/wav，收到 " + format);
    }

    std::string summary;
    std::string err;
    OH_LOG_INFO(LOG_APP, "[transcode] start srcFd=%{public}d dstFd=%{public}d format=%{public}s",
                srcFd, dstFd, format.c_str());
    const int rc = TranscodeAudio(srcFd, dstFd, format, &summary, &err);
    if (rc != 0) return ResultError(env, err.empty() ? "transcode failed" : err);
    return MakeResult(env, true, summary);
}

// 走路径的转码：native 侧 open() 两个文件，省掉 ArkTS 层取 fd
napi_value TranscodeByPathFn(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) return ResultError(env, "transcodeByPath(srcPath, dstPath, format): 参数不足");

    const std::string srcPath = GetString(env, argv[0]);
    const std::string dstPath = GetString(env, argv[1]);
    const std::string format = GetString(env, argv[2]);
    if (!IsSupportedFormat(format)) {
        return ResultError(env, "transcodeByPath: format 只支持 aac/mp3/opus/flac/wav，收到 " + format);
    }

    const int inFd = ::open(srcPath.c_str(), O_RDONLY);
    if (inFd < 0) return ResultError(env, "transcodeByPath: 打不开源文件 " + srcPath);
    const int outFd = ::open(dstPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (outFd < 0) {
        ::close(inFd);
        return ResultError(env, "transcodeByPath: 创建目标文件失败 " + dstPath);
    }

    std::string summary;
    std::string err;
    OH_LOG_INFO(LOG_APP, "[transcode] byPath %{public}s -> %{public}s (%{public}s)",
                srcPath.c_str(), dstPath.c_str(), format.c_str());
    const int rc = TranscodeAudio(inFd, outFd, format, &summary, &err);
    ::close(inFd);
    ::close(outFd);
    if (rc != 0) return ResultError(env, err.empty() ? "transcode failed" : err);
    return MakeResult(env, true, summary);
}

// M3.0 探针：系统 OH_AudioCodec 对 AAC/MP3/FLAC 的能力与可配置性
napi_value HardwareProbeFn(napi_env env, napi_callback_info /*info*/) {
    std::string json;
    if (HardwareProbe(&json) != 0) return ResultError(env, "hardwareProbe failed");
    OH_LOG_INFO(LOG_APP, "[probe] json=%{public}s", json.c_str());
    return MakeResult(env, true, json);
}

// M3.1：硬解开关（A/B 对比用），影响下一次 Open
napi_value DecoderSetHardwareFn(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    bool on = true;
    if (argc >= 1) napi_get_value_bool(env, argv[0], &on);
    SetHardwareDecodeEnabled(on);
    OH_LOG_INFO(LOG_APP, "[decoder] hardware decode %{public}s", on ? "on" : "off");
    return MakeResult(env, true, std::string("{\"hardware\":") + (on ? "true" : "false") + "}");
}

napi_value PlayerPlay(napi_env env, napi_callback_info /*info*/) {
    AudioPlayer *player = EnsurePlayer();
    if (player->Play() != 0) return ResultError(env, player->LastError());
    OH_LOG_INFO(LOG_APP, "[player] play");
    return MakeResult(env, true, player->StatusJson());
}

napi_value PlayerPause(napi_env env, napi_callback_info /*info*/) {
    AudioPlayer *player = EnsurePlayer();
    if (player->Pause() != 0) return ResultError(env, player->LastError());
    OH_LOG_INFO(LOG_APP, "[player] pause");
    return MakeResult(env, true, player->StatusJson());
}

napi_value PlayerStop(napi_env env, napi_callback_info /*info*/) {
    AudioPlayer *player = EnsurePlayer();
    player->Stop();
    OH_LOG_INFO(LOG_APP, "[player] stop");
    return MakeResult(env, true, player->StatusJson());
}

napi_value PlayerStatus(napi_env env, napi_callback_info /*info*/) {
    AudioPlayer *player = EnsurePlayer();
    return MakeResult(env, true, player->StatusJson());
}

napi_value PlayerRelease(napi_env env, napi_callback_info /*info*/) {
    if (g_player) {
        g_player->Release();
        g_player.reset();
    }
    OH_LOG_INFO(LOG_APP, "[player] release");
    return MakeResult(env, true, "{\"state\":\"idle\"}");
}

napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"probe", nullptr, Probe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"readPackets", nullptr, ReadPackets, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"decodeToWav", nullptr, DecodeToWav, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"playerOpen", nullptr, PlayerOpen, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"playerOpenFd", nullptr, PlayerOpenFd, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"playerPlay", nullptr, PlayerPlay, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"playerPause", nullptr, PlayerPause, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"playerStop", nullptr, PlayerStop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"playerStatus", nullptr, PlayerStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"playerRelease", nullptr, PlayerRelease, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"transcodeToAac", nullptr, TranscodeToAacFn, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"transcodeTo", nullptr, TranscodeToFn, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"transcodeByPath", nullptr, TranscodeByPathFn, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"hardwareProbe", nullptr, HardwareProbeFn, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"decoderSetHardware", nullptr, DecoderSetHardwareFn, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    OH_LOG_INFO(LOG_APP, "[init] libentry.so loaded, ffmpeg=%{public}s", "avformat/avcodec/avutil/swresample");
    return exports;
}

napi_module g_module = {
    1,          // nm_version
    0,          // nm_flags
    nullptr,    // nm_filename
    Init,       // nm_register_func
    "entry",    // nm_modname，必须和 libentry.so 对应
    nullptr,    // nm_priv
    {0},        // reserved
};

}  // namespace

extern "C" __attribute__((constructor)) void RegisterEntryModule(void) {
    napi_module_register(&g_module);
}
