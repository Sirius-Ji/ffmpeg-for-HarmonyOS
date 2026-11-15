from pathlib import Path
import hashlib
import io
import tarfile
import zipfile

VERSION = '0.2.2'
ROOT = Path('.')
OUT = ROOT / 'dist/release'
OUT.mkdir(parents=True, exist_ok=True)
HAR_IN = ROOT / 'harmony/ffmpeg_ohos/build/default/outputs/default/ffmpeg_ohos.har'
LIBS = ROOT / 'harmony/ffmpeg_ohos/libs'
PRE = ROOT / 'prebuilt'
INC = LIBS / 'arm64-v8a/include'
NAMES = ['libavcodec.so.63', 'libavformat.so.63', 'libavutil.so.61',
         'libswresample.so.7', 'libavfilter.so.12', 'libswscale.so.10',
         'libmp3lame.so.0', 'libffz.so.1']
ABIS = ('arm64-v8a', 'x86_64')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read(p):
    return Path(p).read_bytes()


# Single source: HAR libs must be byte-identical to prebuilt libs.
for abi in ABIS:
    for n in NAMES:
        a = read(PRE / abi / 'lib' / n)
        b = read(LIBS / abi / n)
        if a != b:
            raise SystemExit('prebuilt/HAR mismatch: %s/%s' % (abi, n))
print('single-source: prebuilt == HAR libs')

# Rebuild HAR with a manifest generated from the actual HAR bytes.
with tarfile.open(HAR_IN, 'r:gz') as src:
    members = [(m, src.extractfile(m).read() if m.isfile() else None) for m in src.getmembers()]
base = [(m, d) for m, d in members if not (m.isfile() and m.name.endswith('/BUILD-INFO.txt'))]
manifest = ['ffmpeg-ohos-har ' + VERSION]
for m, d in base:
    if m.isfile() and not m.name.endswith('/BUILD-INFO.txt'):
        manifest.append('%s %d %s' % (m.name, len(d), sha(d)))
man = ('\n'.join(manifest) + '\n').encode()
har_out = OUT / ('ffmpeg_ohos-v%s.har' % VERSION)
with tarfile.open(har_out, 'w:gz') as dst:
    for m, d in base:
        dst.addfile(m, io.BytesIO(d) if d is not None else None)
    ti = tarfile.TarInfo('package/BUILD-INFO.txt')
    ti.size = len(man)
    dst.addfile(ti, io.BytesIO(man))
print('HAR', har_out, har_out.stat().st_size)

# SDK ZIP: all files from one include/lib set, manifest from actual ZIP bytes.
sdk_files = {}
for p in INC.rglob('*'):
    if p.is_file():
        sdk_files['include/' + p.relative_to(INC).as_posix()] = read(p)
for abi in ABIS:
    for n in NAMES:
        sdk_files['lib/%s/%s' % (abi, n)] = read(LIBS / abi / n)
for src, dst in [(ROOT / 'build/sdk/CMakeLists.txt', 'CMakeLists.txt'),
                 (ROOT / 'build/sdk/README.md', 'README.md'),
                 (ROOT / 'NOTICE.md', 'NOTICE.md'),
                 (ROOT / 'LICENSES/FreeType-FTL.txt', 'LICENSES/FreeType-FTL.txt'),
                 (ROOT / 'LICENSES/HarfBuzz-MIT.txt', 'LICENSES/HarfBuzz-MIT.txt'),
                 (ROOT / 'LICENSES/FFmpeg-LGPL-2.1.txt', 'LICENSES/FFmpeg-LGPL-2.1.txt')]:
    sdk_files[dst] = read(src)
man = ['ffmpeg-ohos-sdk ' + VERSION]
for k in sorted(sdk_files):
    man.append('%s %d %s' % (k, len(sdk_files[k]), sha(sdk_files[k])))
zpath = OUT / ('ffmpeg-ohos-sdk-v%s.zip' % VERSION)
with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for k in sorted(sdk_files):
        z.writestr(k, sdk_files[k])
    z.writestr('BUILD-INFO.txt', '\n'.join(man) + '\n')
print('SDK', zpath, zpath.stat().st_size)

# SHA256SUMS covers every release asset except itself.
sums = OUT / 'SHA256SUMS.txt'
lines = []
for p in sorted(OUT.iterdir()):
    if p.is_file() and p.name != sums.name:
        lines.append('%s  %s' % (sha(read(p)), p.name))
sums.write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
print('SHA256SUMS', sums, len(lines), 'files')
