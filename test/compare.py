"""设备侧解码结果与桌面参考 PCM 对拍。

设备端会打印形如 {"frames":220500,"bytes":882000,"hash":"9f3a..."} 的 JSON，
本脚本算同一批参考 PCM 的 FNV-1a 64，直接比哈希；如果能拿到设备 WAV，
再做逐样本比对（长度差 / 最大绝对差 / 不一致样本数）。

用法：
  python test/compare.py                 # 只打印参考哈希表
  python test/compare.py --wav test.m4a=out.m4a.wav   # 另外逐样本比对
"""
import argparse
import os
import sys
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, "test", "ref")
SAMPLE = "test.m4a"


def fnv1a64(data: bytes) -> int:
    h = 1469598103934665603
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def ref_table():
    rows = []
    for name in sorted(os.listdir(REF)):
        if not name.endswith(".pcm"):
            continue
        p = os.path.join(REF, name)
        data = open(p, "rb").read()
        rows.append((name[:-4], len(data), fnv1a64(data)))
    print(f"{'样本':<12}{'PCM 字节':>10}  {'FNV-1a 64':<18}")
    for name, size, h in rows:
        print(f"{name:<12}{size:>10}  0x{h:016x}")
    return {name: (size, h) for name, size, h in rows}


def compare_wav(sample: str, wav_path: str):
    ref_pcm = os.path.join(REF, sample + ".pcm")
    if not os.path.exists(ref_pcm):
        print(f"!! 没有参考 PCM: {ref_pcm}")
        return 1
    ref = open(ref_pcm, "rb").read()
    with wave.open(wav_path, "rb") as w:
        dev = w.readframes(w.getnframes()) if False else w.readframes(w.getnframes())
        ch, sw, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
    print(f"{sample}: ref={len(ref)}B   dev={len(dev)}B   wav={ch}ch/{sw*8}bit/{rate}Hz")
    n = min(len(ref), len(dev)) // 2
    import array

    a = array.array("h")
    a.frombytes(ref[: n * 2])
    b = array.array("h")
    b.frombytes(dev[: n * 2])
    if sys.byteorder == "big":
        a.byteswap()
        b.byteswap()
    maxdiff = 0
    ndiff = 0
    for x, y in zip(a, b):
        d = abs(x - y)
        if d:
            ndiff += 1
            if d > maxdiff:
                maxdiff = d
    same_hash = fnv1a64(ref) == fnv1a64(dev)
    print(f"  样本数={n}  不一致={ndiff}  最大绝对差={maxdiff}  哈希一致={same_hash}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav", action="append", default=[], metavar="SAMPLE=WAVPATH")
    args = ap.parse_args()
    ref_table()
    rc = 0
    for item in args.wav:
        if "=" not in item:
            print("!! --wav 需要 sample=path 形式")
            rc = 1
            continue
        sample, wav_path = item.split("=", 1)
        rc |= compare_wav(sample, wav_path)
    return rc


if __name__ == "__main__":
    sys.exit(main())
