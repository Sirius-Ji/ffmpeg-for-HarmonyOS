"""生成 Release 资源包：每个 ABI 一个 zip，内含 include/ + .so + 许可证。

打包完成后会调用 build/check-release.py 自动校验（组件 / 包内容 / README 一致性），
校验失败就删掉本次产物并以非零退出码结束。

用法:
    python build/make-release.py
    RELEASE_VERSION=n9.0-ohos python build/make-release.py

产物:
    dist/release/ffmpeg-<version>-<abi>.zip
    dist/release/BUILD-INFO-<abi>.txt     # 独立附件（同时也在 zip 内）
    dist/release/SHA256SUMS.txt
"""
import hashlib
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = os.environ.get("RELEASE_VERSION", "n9.0-ohos")
OUTDIR = os.path.join(ROOT, "dist", "release")
CHECKER = os.path.join(ROOT, "build", "check-release.py")
ABIS = ("arm64-v8a", "x86_64")

# v0.1.1：libavfilter 不随包发布（滤镜开关仍在编译里，但不作为发布能力）
LIB_NAMES = ["avcodec", "avformat", "avutil", "swresample"]
LIB_RE = re.compile(r"^lib(%s)\.so\.(\d+)$" % "|".join(LIB_NAMES))

COMPONENTS = {
    "decoders": "aac ac3 alac ape dca flac mp3 mp3float opus pcm_s16le pcm_s24le pcm_s32le wmav2 h264_oh hevc_oh",
    "encoders": "aac alac flac pcm_s16le pcm_s24le",
    "muxers": "mp4 mov ipod mp3 flac wav matroska adts",
    "parsers": "aac ac3 dca flac mpegaudio opus",
    "notes": "不包含 MP3 编码（未编入 libmp3lame）；MP3 仅支持解码与封装",
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_info(abi, lib_files):
    lines = [
        f"package : ffmpeg-{VERSION}-{abi}",
        "library : FFmpeg n9.0 (commit d32b387), LGPL-2.1-or-later",
        f"abi     : {abi}",
        "target  : aarch64-linux-ohos / x86_64-linux-ohos (musl)",
        "",
        "components:",
    ]
    for key, val in COMPONENTS.items():
        lines.append(f"  {key:9s}: {val}")
    lines += ["", "files:"]
    for name, path in sorted(lib_files.items()):
        lines.append(f"  lib/{name}  sha256={sha256(path)}  size={os.path.getsize(path)}")
    lines += [
        "",
        "notes:",
        "  * 链接时请使用带大版本号的文件名（SONAME 一致），不要改名成 libxxx.so",
        "  * 运行时把同名 .so 放到 HAP 的 libs/<abi>/ 目录",
        "  * 页对齐 16KB（PT_LOAD align = 0x4000）",
        "  * 许可证全文见 LICENSES/LGPL-2.1.txt，声明见 NOTICE.md",
    ]
    return "\n".join(lines) + "\n"


def main():
    os.makedirs(OUTDIR, exist_ok=True)

    # 清掉旧产物，避免校验时把上一版当成这一版
    for fn in os.listdir(OUTDIR):
        if fn.endswith(".zip") or fn == "SHA256SUMS.txt" or fn.startswith("BUILD-INFO"):
            os.remove(os.path.join(OUTDIR, fn))

    made = []
    infos = {}
    for abi in ABIS:
        src = os.path.join(ROOT, "prebuilt", abi)
        if not os.path.isdir(src):
            print(f"skip {abi}: 没有 {src}")
            continue

        libdir = os.path.join(src, "lib")
        lib_files = {}
        for fn in sorted(os.listdir(libdir)):
            if LIB_RE.match(fn) and os.path.isfile(os.path.join(libdir, fn)):
                lib_files[fn] = os.path.join(libdir, fn)
        if not lib_files:
            print(f"skip {abi}: lib 目录里没匹配到 .so")
            continue

        name = f"ffmpeg-{VERSION}-{abi}.zip"
        out = os.path.join(OUTDIR, name)
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for dirpath, _dirs, files in os.walk(os.path.join(src, "include")):
                for fn in files:
                    p = os.path.join(dirpath, fn)
                    z.write(p, os.path.relpath(p, src))
            for fn, p in lib_files.items():
                z.write(p, os.path.join("lib", fn))
            z.write(os.path.join(ROOT, "NOTICE.md"), "NOTICE.md")
            z.write(os.path.join(ROOT, "LICENSES", "LGPL-2.1.txt"),
                    os.path.join("LICENSES", "LGPL-2.1.txt"))
            z.writestr("BUILD-INFO.txt", build_info(abi, lib_files))

        made.append((out, os.path.getsize(out)))
        infos[abi] = lib_files
        print(f"{name}: {os.path.getsize(out)/1048576:.2f} MB, {len(lib_files)} libs")

    if not made:
        print("没有生成任何包")
        return 1

    # BUILD-INFO 单独抽出来当 Release 独立附件
    for abi, lib_files in infos.items():
        p = os.path.join(OUTDIR, f"BUILD-INFO-{abi}.txt")
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(build_info(abi, lib_files))
        made.append((p, os.path.getsize(p)))
        print(f"BUILD-INFO-{abi}.txt: {os.path.getsize(p)} B")

    # 打包后自动校验；失败就删产物、非零退出
    print("\n== 运行 check-release.py ==")
    rc = subprocess.call([sys.executable, CHECKER])
    if rc != 0:
        print("\n!! 校验失败，删除本次生成的产物")
        for path, _size in made:
            if os.path.exists(path):
                os.remove(path)
        return 1

    sums = os.path.join(OUTDIR, "SHA256SUMS.txt")
    with open(sums, "w", encoding="utf-8", newline="\n") as f:
        for path, _size in made:
            f.write(f"{sha256(path)}  {os.path.basename(path)}\n")
    print("\nSHA256SUMS:", sums)
    print("done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
