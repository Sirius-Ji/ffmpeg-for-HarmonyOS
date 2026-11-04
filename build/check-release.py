"""Release 前置校验：组件白名单 + 压缩包内容 + README 一致性。

用法:
    python build/check-release.py            # 校验 dist/release 下已有的包
    python build/check-release.py --no-zip   # 只校验编译配置（打包前用）

任何一项对不上都会打印明细并以非零退出码结束，make-release.py 会据此中止打包。

组件判定同时看两处：
  1. build/<abi>/ffbuild/config.mak 里的 CONFIG_xxx_ENCODER=yes（configure 的最终结果）
  2. build/<abi>/config_components.h 里的 #define CONFIG_xxx_ENCODER 1
两处都过才算数——只看其中一处都可能被依赖裁剪骗过。
"""
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASE_DIR = os.path.join(ROOT, "dist", "release")
ABIS = ("arm64-v8a", "x86_64")
README = os.path.join(ROOT, "README.md")

# v0.1.1 发布能力（与 build/build-ffmpeg.sh 的白名单一致）
REQUIRED = {
    "decoder": [
        "h264_oh", "hevc_oh",
        "aac", "ac3", "alac", "ape", "dca", "flac", "mp3", "mp3float",
        "opus", "pcm_s16le", "pcm_s24le", "pcm_s32le", "wmav2",
    ],
    "encoder": ["aac", "alac", "flac", "pcm_s16le", "pcm_s24le"],
    "muxer": ["mp4", "mov", "ipod", "mp3", "flac", "wav", "matroska", "adts"],
    "demuxer": ["mov", "matroska", "ogg", "flac", "ape", "dsf", "wav", "mp3", "aac", "tta", "wv", "caf", "asf"],
    "bsf": ["aac_adtstoasc"],
}

# 发布包必须带的库（libavfilter 不发布）
MUST_SHIP = ("avcodec", "avformat", "avutil", "swresample")
BANNED_IN_README = ("libavfilter", "avfilter")

TAIL = {"decoder": "DECODER", "encoder": "ENCODER", "muxer": "MUXER",
        "demuxer": "DEMUXER", "bsf": "BSF"}


def read_text(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def libs_on_disk(abi):
    libdir = os.path.join(ROOT, "prebuilt", abi, "lib")
    out = set()
    if not os.path.isdir(libdir):
        return out
    for fn in os.listdir(libdir):
        m = re.match(r"^lib(\w+)\.so\.(\d+)$", fn)
        if m:
            out.add((m.group(1), m.group(2)))
    return out


def check_components(abi, failures):
    mak = os.path.join(ROOT, "build", abi, "ffbuild", "config.mak")
    cc = os.path.join(ROOT, "build", abi, "config_components.h")
    cfg = os.path.join(ROOT, "build", abi, "config.h")
    for p in (mak, cc, cfg):
        if not os.path.exists(p):
            failures.append(f"[{abi}] 找不到编译配置: {p}")
            return

    mak_text = read_text(mak)
    cc_text = read_text(cc)
    for kind, names in REQUIRED.items():
        tail = TAIL[kind]
        for name in names:
            key = f"CONFIG_{name.upper()}_{tail}"
            if not re.search(r"^%s=yes\b" % key, mak_text, re.M):
                failures.append(f"[{abi}] 缺组件: {name} ({kind}) —— config.mak 里 {key} != yes")
            elif not re.search(r"^#define %s 1\b" % key, cc_text, re.M):
                failures.append(f"[{abi}] 组件被裁掉: {name} ({kind}) —— {key} 不是 1")

    cfg_text = read_text(cfg)
    for flag in ("CONFIG_GPL", "CONFIG_NONFREE", "CONFIG_VERSION3"):
        if not re.search(r"^#define %s 0\b" % flag, cfg_text, re.M):
            failures.append(f"[{abi}] 许可开关异常: {flag} 不是 0")


def find_package(abi):
    if not os.path.isdir(RELEASE_DIR):
        return None
    for fn in sorted(os.listdir(RELEASE_DIR)):
        if fn.endswith(f"-{abi}.zip"):
            return fn
    return None


def check_zip(abi, failures):
    name = find_package(abi)
    if name is None:
        failures.append(f"[{abi}] dist/release 下没有找到包")
        return None, None
    path = os.path.join(RELEASE_DIR, name)
    try:
        z = zipfile.ZipFile(path)
    except Exception as exc:  # noqa: BLE001
        failures.append(f"[{abi}] 打不开 {name}: {exc}")
        return name, None

    names = set(z.namelist())
    disk = libs_on_disk(abi)
    packed = set()
    for fn in names:
        m = re.match(r"^lib/lib(\w+)\.so\.(\d+)$", fn)
        if m:
            packed.add((m.group(1), m.group(2)))

    if disk - packed:
        failures.append(f"[{abi}] 包里缺库: {sorted('lib%s.so.%s' % x for x in disk - packed)}")
    if packed - disk:
        failures.append(f"[{abi}] 包里有磁盘上不存在的库: {sorted(packed - disk)}")

    for need in MUST_SHIP:
        if not any(n == need for n, _ in disk):
            failures.append(f"[{abi}] 必需库缺失: lib{need}.so.*（未编译出来）")

    for hdr in ("include/libavcodec/avcodec.h", "include/libavformat/avformat.h",
                "include/libavutil/avutil.h", "include/libswresample/swresample.h"):
        if hdr not in names:
            failures.append(f"[{abi}] 包里缺头文件: {hdr}")

    for meta in ("NOTICE.md", "LICENSES/LGPL-2.1.txt", "BUILD-INFO.txt"):
        if meta not in names:
            failures.append(f"[{abi}] 包里缺: {meta}")

    standalone = os.path.join(RELEASE_DIR, f"BUILD-INFO-{abi}.txt")
    if not os.path.exists(standalone):
        failures.append(f"[{abi}] 缺独立附件: BUILD-INFO-{abi}.txt")
    return name, packed


def check_readme(failures, packages, libs_by_abi):
    if not os.path.exists(README):
        failures.append("找不到 README.md")
        return
    text = read_text(README)

    mentioned = set((m.group(1), m.group(2)) for m in re.finditer(r"lib(\w+)\.so\.(\d+)", text))
    actual = set()
    for libs in libs_by_abi.values():
        actual |= libs
    if actual:
        if actual - mentioned:
            failures.append("README 未提到实际发布的库: " + str(sorted('lib%s.so.%s' % x for x in actual - mentioned)))
        if mentioned - actual:
            failures.append("README 提到的库不在产物里: " + str(sorted('lib%s.so.%s' % x for x in mentioned - actual)))
        if len(mentioned) != len(MUST_SHIP):
            failures.append(f"README 里 .so 数量不对: 提到 {len(mentioned)} 个，应发布 {len(MUST_SHIP)} 个")

    for banned in BANNED_IN_README:
        if banned in text:
            failures.append(f"README 仍提到已不发布的库名: {banned}")

    for pkg in packages:
        if pkg and pkg not in text:
            failures.append(f"README 未提到发布包名: {pkg}")


def main():
    no_zip = "--no-zip" in sys.argv
    failures = []
    libs_by_abi = {}
    packages = []

    for abi in ABIS:
        check_components(abi, failures)
        if no_zip:
            libs_by_abi[abi] = {x for x in libs_on_disk(abi) if x[0] in MUST_SHIP}
        else:
            pkg, libs = check_zip(abi, failures)
            packages.append(pkg)
            if libs is not None:
                libs_by_abi[abi] = libs

    if not no_zip:
        check_readme(failures, packages, libs_by_abi)

    print("== 检查项 ==")
    print(f"   ABI        : {', '.join(ABIS)}")
    print("   组件白名单 : " + ", ".join(f"{k}={len(v)}" for k, v in REQUIRED.items()))
    print(f"   发布库     : {', '.join(MUST_SHIP)}")
    print(f"   检查压缩包 : {'否' if no_zip else '是'}")

    if failures:
        print("\n!! 校验失败：")
        for f in failures:
            print("  -", f)
        return 1

    print("\nOK: 组件、压缩包与 README 一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
