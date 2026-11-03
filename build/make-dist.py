"""把 arm64-v8a 的 FFmpeg 头文件与带版本号的 .so 打成 zip，方便直接拷进别的工程。"""
import os
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "prebuilt", "arm64-v8a")
DIST = os.path.join(ROOT, "dist")
OUT = os.path.join(DIST, "ffmpeg-m1-arm64-v8a.zip")

os.makedirs(DIST, exist_ok=True)
count = 0
with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
    for dirpath, _dirs, files in os.walk(os.path.join(SRC, "include")):
        for fn in files:
            p = os.path.join(dirpath, fn)
            z.write(p, os.path.relpath(p, SRC))
            count += 1
    libdir = os.path.join(SRC, "lib")
    for fn in sorted(os.listdir(libdir)):
        p = os.path.join(libdir, fn)
        if not os.path.isfile(p):
            continue
        # 只收 libxxx.so.NN（NEEDED 里用的名字），跳过 .so 与 .so.NN.MM.PP
        tail = fn.rsplit(".so.", 1)
        if len(tail) != 2 or not tail[1].isdigit():
            continue
        z.write(p, "lib/" + fn)
        count += 1

print("files:", count)
print("size: %.1f MB" % (os.path.getsize(OUT) / 1048576))
print("path:", OUT)
