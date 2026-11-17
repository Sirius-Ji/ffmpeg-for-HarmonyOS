#!/usr/bin/env python3
"""M5.2 artifact gates: NEEDED closure, manifest hashes, SHA256SUMS coverage."""
from pathlib import Path
import hashlib
import struct
import sys
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / 'dist/release'
VERSION = '0.3.1'
WHITELIST = {
    'libc.so', 'libm.so', 'libdl.so', 'libpthread.so', 'librt.so',
    'libace_napi.z.so', 'libhilog_ndk.z.so', 'libohaudio.so',
    'libnative_media_vdec.so', 'libnative_media_venc.so',
    'libnative_media_codecbase.so', 'libnative_media_core.so',
    'libnative_media_acodec.so', 'libnative_window.so',
    # 宿主 App 必须提供的 NDK C++ runtime（HAR 明确不随包）
    'libc++_shared.so',
}

def sha(data):
    return hashlib.sha256(data).hexdigest()

def elf_needed(data):
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        return []
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phentsize = struct.unpack_from('<H', data, 54)[0]
    phnum = struct.unpack_from('<H', data, 56)[0]
    loads, dyn = [], None
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type = struct.unpack_from('<I', data, off)[0]
        p_offset = struct.unpack_from('<Q', data, off + 8)[0]
        p_vaddr = struct.unpack_from('<Q', data, off + 16)[0]
        p_filesz = struct.unpack_from('<Q', data, off + 32)[0]
        if p_type == 1:
            loads.append((p_vaddr, p_vaddr + p_filesz, p_offset))
        elif p_type == 2:
            dyn = (p_offset, p_filesz)
    if not dyn:
        return []
    strtab = strsz = None
    needed = []
    off, size = dyn
    for i in range(0, size, 16):
        tag, val = struct.unpack_from('<QQ', data, off + i)
        if tag == 0:
            break
        if tag == 1:
            needed.append(val)
        elif tag == 5:
            strtab = val
        elif tag == 10:
            strsz = val
    if strtab is None or strsz is None:
        return []
    stroff = None
    for start, end, foff in loads:
        if start <= strtab < end:
            stroff = foff + (strtab - start)
            break
    if stroff is None:
        return []
    out = []
    for v in needed:
        s = data[stroff + v:data.find(b'\0', stroff + v)]
        out.append(s.decode('utf-8', 'replace'))
    return out

def check_needed(libs, label, failures):
    names = set(libs)
    for name, data in libs.items():
        for dep in elf_needed(data):
            if dep in names:
                continue
            if dep in WHITELIST:
                continue
            failures.append(f'{label}: {name} NEEDED {dep} 不在包内/系统白名单')
        if 'libz.so' in elf_needed(data):
            failures.append(f'{label}: {name} 仍依赖 libz.so')

def check_manifest(files, manifest, label, failures):
    listed = {}
    for line in manifest.decode('utf-8', 'replace').splitlines()[1:]:
        parts = line.split()
        if len(parts) == 3:
            listed[parts[0]] = (int(parts[1]), parts[2])
    actual = {k: v for k, v in files.items() if not k.endswith('BUILD-INFO.txt')}
    for k, (size, digest) in listed.items():
        if k not in files:
            failures.append(f'{label}: 清单文件缺失 {k}')
        elif len(files[k]) != size or sha(files[k]) != digest:
            failures.append(f'{label}: 哈希不匹配 {k}')
    for k in actual:
        if k not in listed:
            failures.append(f'{label}: 清单未覆盖 {k}')

def main():
    failures = []
    har = DIST / f'ffmpeg_ohos-v{VERSION}.har'
    sdk = DIST / f'ffmpeg-ohos-sdk-v{VERSION}.zip'
    for p in (har, sdk):
        if not p.exists():
            failures.append('缺少发布物 ' + str(p))

    if har.exists():
        with tarfile.open(har, 'r:gz') as t:
            files = {m.name: t.extractfile(m).read() for m in t.getmembers() if m.isfile()}
        for abi in ('arm64-v8a', 'x86_64'):
            libs = {Path(k).name: v for k, v in files.items()
                    if k.startswith(f'package/libs/{abi}/') and '.so' in Path(k).name}
            check_needed(libs, f'HAR/{abi}', failures)
        man = files.get('package/BUILD-INFO.txt')
        if man is None:
            failures.append('HAR: 缺少 package/BUILD-INFO.txt')
        else:
            check_manifest(files, man, 'HAR', failures)
            for abi in ('arm64-v8a', 'x86_64'):
                if f'package/libs/{abi}/libffz.so.1' not in files:
                    failures.append(f'HAR: 缺少 {abi} libffz.so.1')

    if sdk.exists():
        with zipfile.ZipFile(sdk) as z:
            files = {n: z.read(n) for n in z.namelist() if not n.endswith('/')}
        for abi in ('arm64-v8a', 'x86_64'):
            libs = {Path(k).name: v for k, v in files.items()
                    if k.startswith(f'lib/{abi}/') and '.so' in Path(k).name}
            check_needed(libs, f'SDK/{abi}', failures)
        man = files.get('BUILD-INFO.txt')
        if man is None:
            failures.append('SDK: 缺少 BUILD-INFO.txt')
        else:
            check_manifest(files, man, 'SDK', failures)
        for need in ('NOTICE.md', 'LICENSES/FreeType-FTL.txt', 'LICENSES/HarfBuzz-MIT.txt'):
            if need not in files:
                failures.append('SDK: 缺少 ' + need)

    sums = DIST / 'SHA256SUMS.txt'
    if sums.exists():
        listed = {}
        for line in sums.read_text(encoding='utf-8').splitlines():
            if '  ' in line:
                h, n = line.split('  ', 1)
                listed[n] = h
        for p in DIST.iterdir():
            if p.is_file() and p.name != sums.name:
                if p.name not in listed:
                    failures.append('SHA256SUMS 未覆盖 ' + p.name)
                elif sha(p.read_bytes()) != listed[p.name]:
                    failures.append('SHA256SUMS 哈希不匹配 ' + p.name)
    else:
        failures.append('缺少 SHA256SUMS.txt')

    if failures:
        print('!! M5.2 artifact gates FAILED')
        for f in failures:
            print('  -', f)
        return 1
    print('OK: M5.2 NEEDED 闭包 / HAR+SDK 清单哈希 / SHA256SUMS 全覆盖')
    return 0

if __name__ == '__main__':
    sys.exit(main())
