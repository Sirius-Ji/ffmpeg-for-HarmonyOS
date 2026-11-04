"""生成 Release 资源包：每个 ABI 一个 zip，内含 include/ + .so + 许可证。

用法:
    python build/make-release.py                 # 默认 n9.0-ohos
    RELEASE_VERSION=n9.0-ohos-r1 python build/make-release.py

产物:
    dist/release/ffmpeg-<version>-<abi>.zip
    dist/release/SHA256SUMS.txt
"""
import hashlib
import os
import re
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = os.environ.get("RELEASE_VERSION", "n9.0-ohos")
OUTDIR = os.path.join(ROOT, "dist", "release")
ABIS = ("arm64-v8a", "x86_64")

# 发布包只带调用方真正需要的四个库；libavfilter 目前没有启用任何 filter，先不发
LIB_NAMES = ["avcodec", "avformat", "avutil", "swresample"]
if os.environ.get("INCLUDE_AVFILTER") == "1":
    LIB_NAMES.append("avfilter")

LIB_RE = re.compile(r"^lib(%s)\.so\.(\d+)$" % "|".join(LIB_NAMES))


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
        "files:",
    ]
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
    made = []

    for abi in ABIS:
        src = os.path.join(ROOT, "prebuilt", abi)
        if not os.path.isdir(src):
            print(f"skip {abi}: 没有 {src}")
            continue

        libdir = os.path.join(src, "lib")
        lib_files = {}
        for fn in sorted(os.listdir(libdir)):
            m = LIB_RE.match(fn)
            if m and os.path.isfile(os.path.join(libdir, fn)):
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
        print(f"{name}: {os.path.getsize(out)/1048576:.2f} MB, "
              f"{len(lib_files)} libs, {len(os.listdir(os.path.join(src,'lib')))} files in lib/")

    sums = os.path.join(OUTDIR, "SHA256SUMS.txt")
    with open(sums, "w", encoding="utf-8", newline="\n") as f:
        for path, _size in made:
            f.write(f"{sha256(path)}  {os.path.basename(path)}\n")
    print("SHA256SUMS:", sums)
    print("done.")


if __name__ == "__main__":
    main()
