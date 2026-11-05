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
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASE_DIR = os.path.join(ROOT, "dist", "release")
ABIS = ("arm64-v8a", "x86_64")
README = os.path.join(ROOT, "README.md")

# v0.1.1 发布能力（与 build/build-ffmpeg.sh 的白名单一致）
REQUIRED = {
    "decoder": [
        "h264_oh", "hevc_oh",
        "aac", "ac3", "eac3", "alac", "ape", "dca", "flac", "mp3", "mp3float",
        "opus", "pcm_s16le", "pcm_s24le", "pcm_s32le", "wmav2",
    ],
    "encoder": ["aac", "alac", "flac", "libmp3lame", "pcm_s16le", "pcm_s24le", "opus"],
    "muxer": ["mp4", "mov", "ipod", "mp3", "flac", "wav", "matroska", "adts", "ogg", "opus"],
    "demuxer": ["mov", "matroska", "ogg", "flac", "ape", "dsf", "wav", "mp3", "aac", "tta", "wv", "caf", "asf"],
    "bsf": ["aac_adtstoasc"],
}

# 发布包必须带的库（libavfilter 不发布；libmp3lame 是外部依赖）
MUST_SHIP = ("avcodec", "avformat", "avutil", "swresample", "mp3lame")
# 只禁“声称发布了 libavfilter”的写法；README 里说“已移除 libavfilter”是允许的
BANNED_IN_README = ("libavfilter.so", "avfilter.so")

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
                "include/libavutil/avutil.h", "include/libswresample/swresample.h",
                "include/lame/lame.h"):
        if hdr not in names:
            failures.append(f"[{abi}] 包里缺头文件: {hdr}")

    for meta in ("NOTICE.md", "LICENSES/LGPL-2.1.txt", "BUILD-INFO.txt",
                 "lib/cmake/ffmpeg_ohos/ffmpeg-ohos-config.cmake",
                 "lib/cmake/ffmpeg_ohos/ffmpeg_ohos-config.cmake"):
        if meta not in names:
            failures.append(f"[{abi}] 包里缺: {meta}")

    try:
        info = z.read("BUILD-INFO.txt").decode("utf-8", "replace")

        # 动态校验 release_notes：必须包含当前最新版本 Tag，防止文案变化石
        notes = [ln.split(":", 1)[1].strip() for ln in info.splitlines()
                 if ln.strip().startswith("release_notes:") and ":" in ln]
        tag = latest_tag()
        if not notes:
            failures.append(f"[{abi}] BUILD-INFO 缺 release_notes 字段")
        elif not tag:
            failures.append("无法确定当前版本 Tag（git describe 失败，可设 RELEASE_TAG）")
        elif tag not in notes[0]:
            failures.append(f"[{abi}] release_notes 未包含当前版本 Tag {tag}: {notes[0][:60]}…")

        # 严格比对：包内每个 .so 的 sha256 必须与 BUILD-INFO.txt 记录一致
        recorded = {}
        for line in info.splitlines():
            m = re.match(r"\s+lib/(\S+)\s+sha256=([0-9a-f]{64})\s+size=(\d+)", line)
            if m:
                recorded["lib/" + m.group(1)] = m.group(2)
        so_names = [n for n in names if re.match(r"^lib/lib\w+\.so\.\d+$", n)]
        if len(so_names) != len(MUST_SHIP):
            failures.append(f"[{abi}] 包内 .so 数量不对: {len(so_names)}，应发布 {len(MUST_SHIP)} 个")
        for fn in sorted(so_names):
            if fn not in recorded:
                failures.append(f"[{abi}] BUILD-INFO 未记录 {fn} 的 sha256")
                continue
            digest = hashlib.sha256(z.read(fn)).hexdigest()
            if digest != recorded[fn]:
                failures.append(
                    f"[{abi}] {fn} sha256 与 BUILD-INFO 不一致: zip={digest[:16]}… info={recorded[fn][:16]}…")
    except KeyError:
        failures.append(f"[{abi}] 读不到 BUILD-INFO.txt")

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

    if "eac3" not in text:
        failures.append("README 未提到新增的 eac3 解码器")
    if "ffmpeg-ohos-config.cmake" not in text:
        failures.append("README 未提到 ffmpeg-ohos-config.cmake")

    for pkg in packages:
        if pkg and pkg not in text:
            failures.append(f"README 未提到发布包名: {pkg}")


def latest_tag():
    env = os.environ.get("RELEASE_TAG")
    if env:
        return env.strip()
    try:
        out = subprocess.check_output(["git", "describe", "--tags", "--abbrev=0"],
                                      cwd=ROOT, text=True, stderr=subprocess.DEVNULL)
        return out.strip()
    except Exception:  # noqa: BLE001
        return ""


def find_cmake():
    deveco = os.environ.get("DEVECO_HOME", r"D:\Program Files\DevEco Studio")
    cands = [
        os.environ.get("CMAKE"),
        shutil.which("cmake"),
        os.path.join(deveco, "sdk", "default", "openharmony", "native",
                     "build-tools", "cmake", "bin", "cmake.exe"),
    ]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def find_ninja():
    deveco = os.environ.get("DEVECO_HOME", r"D:\Program Files\DevEco Studio")
    cands = [
        shutil.which("ninja"),
        os.path.join(deveco, "sdk", "default", "openharmony", "native",
                     "build-tools", "cmake", "bin", "ninja.exe"),
    ]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def check_cmake_consumer(abi, failures):
    """解包后用 3 行 CMakeLists 跑一次 find_package，防止包名/路径回归。"""
    name = find_package(abi)
    if name is None:
        return  # check_zip 已经报过
    cmake = find_cmake()
    if cmake is None:
        failures.append("找不到 cmake，无法执行消费测试（可设 CMAKE=...）")
        return
    tmp = tempfile.mkdtemp(prefix="ffmpeg-ohos-cmake-")
    try:
        pkg = os.path.join(tmp, "pkg")
        with zipfile.ZipFile(os.path.join(RELEASE_DIR, name)) as z:
            z.extractall(pkg)
        src = os.path.join(tmp, "src")
        os.makedirs(src)
        with open(os.path.join(src, "CMakeLists.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("cmake_minimum_required(VERSION 3.16)\n"
                    "project(ffmpeg_ohos_consumer NONE)\n"
                    "find_package(ffmpeg_ohos REQUIRED)\n")
        build = os.path.join(tmp, "build")
        cmd = [cmake, "-S", src, "-B", build, f"-DCMAKE_PREFIX_PATH={pkg}"]
        ninja = find_ninja()
        if ninja:
            cmd += ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"]
        r = subprocess.run(cmd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        if r.returncode != 0:
            tail = (r.stdout + "\n" + r.stderr).strip().splitlines()[-10:]
            failures.append(f"[{abi}] CMake 消费测试失败: " + " | ".join(tail))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _parse_build_whitelist():
    text = read_text(os.path.join(ROOT, "build", "build-ffmpeg.sh"))
    out = {}
    for kind in ("decoder", "encoder", "muxer", "demuxer", "bsf"):
        m = re.search(r"--enable-%s=([^\s\\]+)" % kind, text)
        if m:
            out[kind] = set(m.group(1).split(","))
    return out


def _parse_make_release_components():
    text = read_text(os.path.join(ROOT, "build", "make-release.py"))
    out = {}
    for kind in ("decoders", "encoders", "muxers", "demuxers", "bsf"):
        m = re.search(r'"%s"\s*:\s*"([^"]*)"' % kind, text)
        if m:
            out[kind] = set(m.group(1).split())
    return out


def check_source_consistency(failures):
    build = _parse_build_whitelist()
    maker = _parse_make_release_components()
    pairs = [("decoder", "decoders"), ("encoder", "encoders"), ("muxer", "muxers"),
             ("demuxer", "demuxers"), ("bsf", "bsf")]
    for bkey, mkey in pairs:
        b = build.get(bkey)
        m = maker.get(mkey)
        req = set(REQUIRED.get(bkey, []))
        if b is None or m is None:
            failures.append(f"源码白名单解析失败: {bkey}")
            continue
        if b != m:
            failures.append(f"{bkey} 白名单不一致（build-ffmpeg.sh vs make-release.py）: {sorted(b ^ m)}")
        if req != b:
            failures.append(f"{bkey} 白名单与 check-release.py 不一致: {sorted(req ^ b)}")


def check_source_files(failures):
    required = [
        "README.md", "CHANGELOG.md", "NOTICE.md", "LICENSES/LGPL-2.1.txt",
        "build/build-ffmpeg.sh", "build/make-release.py", "build/check-release.py",
        "build/ffmpeg-ohos-config.cmake", "build/ffmpeg_ohos-config.cmake",
        ".github/workflows/ci.yml",
    ]
    for rel in required:
        if not os.path.exists(os.path.join(ROOT, rel)):
            failures.append(f"缺少文件: {rel}")
    tag = latest_tag()
    if tag and os.path.exists(README) and tag not in read_text(README):
        failures.append(f"README 未包含最新版本号 {tag}")


def main():
    source_only = "--source-only" in sys.argv
    no_zip = "--no-zip" in sys.argv
    failures = []

    if source_only:
        check_source_files(failures)
        check_source_consistency(failures)
        check_readme(failures, [], {})
        print("== 源码一致性检查（--source-only）==")
        print(f"   版本 Tag   : {latest_tag() or '(未知)'}")
        print("   组件白名单 : 与 build-ffmpeg.sh / make-release.py 对拍")
        if failures:
            print("\n!! 校验失败：")
            for f in failures:
                print("  -", f)
            return 1
        print("\nOK: README、版本号、组件白名单与脚本一致")
        return 0

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
        check_cmake_consumer(ABIS[0], failures)

    print("== 检查项 ==")
    print(f"   ABI        : {', '.join(ABIS)}")
    print(f"   版本 Tag   : {latest_tag() or '(未知)'}")
    print("   组件白名单 : " + ", ".join(f"{k}={len(v)}" for k, v in REQUIRED.items()))
    print(f"   发布库     : {', '.join(MUST_SHIP)}")
    print(f"   检查压缩包 : {'否' if no_zip else '是'}")
    print(f"   CMake 消费 : {'否' if no_zip else '是'}")

    if failures:
        print("\n!! 校验失败：")
        for f in failures:
            print("  -", f)
        return 1

    print("\nOK: 组件、压缩包、CMake 消费与 README 一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
