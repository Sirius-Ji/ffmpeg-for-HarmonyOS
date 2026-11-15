from pathlib import Path
import hashlib
import tarfile
import zipfile

ROOT = Path('.')
HAR = ROOT / 'harmony/ffmpeg_ohos/build/default/outputs/default/ffmpeg_ohos.har'
OUT = ROOT / 'dist/release'
OUT.mkdir(parents=True, exist_ok=True)
LIBS = ROOT / 'harmony/ffmpeg_ohos/libs'
INC = LIBS / 'arm64-v8a/include'
NAMES = ['libavcodec.so.63', 'libavformat.so.63', 'libavutil.so.61',
         'libswresample.so.7', 'libavfilter.so.12', 'libswscale.so.10',
         'libmp3lame.so.0', 'libffz.so.1']

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

lines = ['ffmpeg-ohos-sdk v0.2.1', 'License: LGPL-2.1-or-later', '']
for abi in ('arm64-v8a', 'x86_64'):
    lines.append('[%s]' % abi)
    for n in NAMES:
        p = LIBS / abi / n
        lines.append('%s %d %s' % (n, p.stat().st_size, sha(p)))
    lines.append('')

sdk_info = OUT / 'BUILD-INFO.txt'
sdk_info.write_text('\n'.join(lines), encoding='utf-8', newline='\n')

# Build ZIP SDK
zpath = OUT / 'ffmpeg-ohos-sdk-v0.2.1.zip'
with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for p in INC.rglob('*'):
        if p.is_file():
            z.write(p, 'include/' + p.relative_to(INC).as_posix())
    for abi in ('arm64-v8a', 'x86_64'):
        for n in NAMES:
            z.write(LIBS / abi / n, 'lib/%s/%s' % (abi, n))
    z.write(ROOT / 'build/sdk/CMakeLists.txt', 'CMakeLists.txt')
    z.write(ROOT / 'build/sdk/README.md', 'README.md')
    z.write(sdk_info, 'BUILD-INFO.txt')

# Copy HAR and inject BUILD-INFO.txt at package root (additive copy)
har_out = OUT / 'ffmpeg_ohos-v0.2.1.har'
with tarfile.open(HAR, 'r:gz') as src, tarfile.open(har_out, 'w:gz') as dst:
    for m in src.getmembers():
        f = src.extractfile(m) if m.isfile() else None
        dst.addfile(m, f)
    data = sdk_info.read_bytes()
    ti = tarfile.TarInfo('package/BUILD-INFO.txt')
    ti.size = len(data)
    import io
    dst.addfile(ti, io.BytesIO(data))

print('ZIP', zpath, zpath.stat().st_size)
print('HAR', har_out, har_out.stat().st_size)
