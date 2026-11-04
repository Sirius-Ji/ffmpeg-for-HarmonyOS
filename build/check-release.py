"""Release 前置校验：组件白名单 + 压缩包内容 + README 一致性。

用法:
    python build/check-release.py            # 校验 dist/release 下已有的包
    python build/check-release.py --no-zip   # 只校验编译配置（打包前用）

任何一项对不上都会打印明细并以非零退出码结束，make-release.py 会据此中止打包。
"""
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASE_DIR = os.path.join(ROOT, "dist", "release")
ABIS = ("arm64-v8a", "x86_64")
README = os.path.join(ROOT, "README.md")

# 必须编进库里的组件（名字对应 configure 的 --enable-* 参数）
REQUIRED = {
    "decoder": [
        "aac", "alac", "ape", "ac3", "dca", "flac", "mp3", "opus", "pcm_s16le", "wmav2",
        "h264_oh", "hevc_oh",
    ],
    "encoder": ["aac", "flac", "pcm_s16le"],
    "muxer": ["mp4", "mov", "mp3", "flac", "wav", "matroska"],
    "demuxer": ["mov", "matroska", "ogg", "flac", "ape", "dsf", "wav", "mp3", "aac", "tta", "wv", "caf", "asf"],
    "filter": ["aresample", "aformat", "anull", "volume"],
    "bsf": ["aac_adtstoasc"],
}

# 发布包必须带的库（去掉 avfilter 就会被这里拦住）
MUST_SHIP = {"avcodec", "avformat", "avutil", "swresample", "avfilter"}


def read_text(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def libs_on_disk(abi):
    """prebuilt/<abi>/lib 下带大版本号的 .so -> {(name, major)}"""
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
    cc = os.path.join(ROOT, "build", abi, "config_components.h")
    cfg = os.path.join(ROOT, "build", abi, "config.h")
    if not os.path.exists(cc) or not os.path.exists(cfg):
        failures.append(f"[{abi}] 找不到编译配置: {cc}（先跑 build/build-ffmpeg.sh）")
        return

    text = read_text(cc)
    for kind, names in REQUIRED.items():
        for name in names:
            macro = "CONFIG_%s_%s" % (name.upper(), kind.upper())
            if not re.search(r"^#define %s 1\b" % macro, text, re.M):
                failures.append(f"[{abi}] 缺组件: {name} ({kind}) —— {macro} != 1")

    cfg_text = read_text(cfg)
    for flag in ("CONFIG_GPL", "CONFIG_NONFREE", "CONFIG_VERSION3"):
        if not re.search(r"^#define %s 0\b" % flag, cfg_text, re.M):
            failures.append(f"[{abi}] 许可开关异常: {flag} 不是 0")


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

    missing = disk - packed
    extra = packed - disk
    if missing:
        failures.append(f"[{abi}] 包里缺库: {sorted('lib%s.so.%s' % x for x in missing)}")
    if extra:
        failures.append(f"[{abi}] 包里有磁盘上不存在的库: {sorted(extra)}")

    for need in MUST_SHIP:
        if not any(n == need for n, _ in disk):
            failures.append(f"[{abi}] 必需库缺失: lib{need}.so.*（未编译出来）")

    for hdr in ("include/libavcodec/avcodec.h", "include/libavformat/avformat.h",
                "include/libavutil/avutil.h", "include/libswresample/swresample.h",
                "include/libavfilter/avfilter.h"):
        if hdr not in names:
            failures.append(f"[{abi}] 包里缺头文件: {hdr}")

    for meta in ("NOTICE.md", "LICENSES/LGPL-2.1.txt", "BUILD-INFO.txt"):
        if meta not in names:
            failures.append(f"[{abi}] 包里缺: {meta}")
    return name, packed


def find_package(abi):
    if not os.path.isdir(RELEASE_DIR):
        return None
    for fn in sorted(os.listdir(RELEASE_DIR)):
        if fn.endswith(f"-{abi}.zip"):
            return fn
    return None


def check_readme(failures, packages, libs_by_abi):
    if not os.path.exists(README):
        failures.append("找不到 README.md")
        return
    text = read_text(README)

    # 1) README 里出现的 .so 集合必须和实际发布的一致
    mentioned = set((m.group(1), m.group(2)) for m in re.finditer(r"lib(\w+)\.so\.(\d+)", text))
    actual = set()
    for libs in libs_by_abi.values():
        actual |= libs
    if actual:
        missing = actual - mentioned
        stale = mentioned - actual
        if missing:
            failures.append("README 未提到实际发布的库: " + str(sorted('lib%s.so.%s' % x for x in missing)))
        if stale:
            failures.append("README 提到的库不在产物里: " + str(sorted('lib%s.so.%s' % x for x in stale)))

    # 2) 每个 Release 包名都要在 README 里出现
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
            libs_by_abi[abi] = libs_on_disk(abi)
        else:
            pkg, libs = check_zip(abi, failures)
            packages.append(pkg)
            if libs is not None:
                libs_by_abi[abi] = libs

    if not no_zip:
        check_readme(failures, packages, libs_by_abi)

    print("== 检查项 ==")
    print(f"   ABI          : {', '.join(ABIS)}")
    print(f"   组件白名单   : " + ", ".join(f"{k}={len(v)}" for k, v in REQUIRED.items()))
    print(f"   发布库       : " + ", ".join(sorted(MUST_SHIP)))
    print(f"   检查压缩包   : {'否' if no_zip else '是'}")

    if failures:
        print("\n!! 校验失败：")
        for f in failures:
            print("  -", f)
        return 1

    print("\nOK：组件、压缩包与 README 一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
