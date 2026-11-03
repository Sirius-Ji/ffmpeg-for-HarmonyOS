"""审计产物里未定义的 OH_* 符号，逐个对 NDK 头文件的 availability 版本。

判定只看“声明处”：
  1) 符号后面 400 字符内出现 availability(ohos, introduced=N)
  2) 否则符号前面 1200 字符内出现 @since N
多个声明取最小值；都没有则视为 API 9（首批 NDK API）。

用法: python build/audit-oh-symbols.py [abi]
环境变量: DEVECO_HOME（DevEco 安装目录），默认 D:/Program Files/DevEco Studio
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEVECO_HOME = os.environ.get("DEVECO_HOME", r"D:/Program Files/DevEco Studio")
NDK = os.path.join(DEVECO_HOME, "sdk", "default", "openharmony", "native")
INC = os.path.join(NDK, "sysroot", "usr", "include")
LLVM = os.path.join(NDK, "llvm", "bin")
ABI = sys.argv[1] if len(sys.argv) > 1 else "arm64-v8a"
LIBDIR = os.path.join(ROOT, "prebuilt", ABI, "lib")


def undef_symbols(path):
    out = subprocess.run(
        [os.path.join(LLVM, "llvm-nm.exe"), "-D", "--undefined-only", path],
        capture_output=True, text=True,
    ).stdout
    return sorted({line.split()[-1] for line in out.splitlines() if line.strip()})


def scan_headers(wanted):
    found = {}
    for dirpath, _dirs, files in os.walk(INC):
        for fn in files:
            if not fn.endswith(".h"):
                continue
            try:
                text = open(os.path.join(dirpath, fn), encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            for sym in wanted:
                for m in re.finditer(re.escape(sym) + r"\s*\(", text):
                    tail = text[m.end():m.end() + 400]
                    ver = re.search(r"introduced=([0-9]+)\.", tail)
                    if ver:
                        v = int(ver.group(1))
                    else:
                        head = text[max(0, m.start() - 1200):m.start()]
                        since = re.findall(r"@since\s+([0-9]+)", head)
                        if not since:
                            continue
                        v = int(since[-1])
                    prev = found.get(sym)
                    found[sym] = v if prev is None else min(prev, v)
    return found


def main():
    syms = set()
    libs = ("libavcodec.so.63", "libavutil.so.61", "libavformat.so.63", "libswresample.so.7")
    for lib in libs:
        p = os.path.join(LIBDIR, lib)
        if os.path.exists(p):
            syms.update(s for s in undef_symbols(p) if s.startswith(("OH_", "HMS_")))
    versions = scan_headers(syms)

    rows = sorted(((s, versions.get(s)) for s in syms), key=lambda r: (-(r[1] or 0), r[0]))
    worst = max([v for _s, v in rows if isinstance(v, int)], default=0)
    lines = ["| 符号 | 引入 API |", "|---|---|"]
    for sym, ver in rows:
        lines.append(f"| `{sym}` | {ver if ver is not None else '? (未在头文件中找到)'} |")
    lines += [
        "",
        f"统计：引用 {len(rows)} 个 OH 符号，最高引入版本 = API {worst}。",
        f"构建 NDK = API 26，目标基线 = API 20，结论：{'通过' if worst <= 20 else '不通过'}",
    ]
    out = os.path.join(ROOT, "prebuilt", ABI, "oh-symbol-audit.md")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print(f"symbols={len(rows)} worst_api={worst} verdict={'PASS' if worst <= 20 else 'FAIL'}")
    print("written:", out)
    return 0 if worst <= 20 else 1


if __name__ == "__main__":
    sys.exit(main())
