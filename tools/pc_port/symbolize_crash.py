#!/usr/bin/env python3
"""Turn the "[ppl:(?:crash|assert)]" backtrace of a PC-port log into function + file:line.

Usage: python tools/pc_port/symbolize_crash.py LOG [EXE] [--addr2line PATH]
EXE defaults to build-clang/pokeheartgold.exe; addr2line to llvm-addr2line on PATH.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    tool = "llvm-addr2line"
    if "--addr2line" in sys.argv:
        tool = sys.argv[sys.argv.index("--addr2line") + 1]
        args = [a for a in args if a != tool]
    log = pathlib.Path(args[0]).read_text(encoding="utf-8", errors="replace")
    exe = args[1] if len(args) > 1 else str(ROOT / "build-clang" / "pokeheartgold.exe")
    addrs = re.findall(r"\[ppl:(?:crash|assert)\]\s+#\d+ (0x[0-9A-Fa-f]+)", log)
    head = re.search(r"\[ppl:(?:crash|assert)\] exception.*", log)
    if head:
        print(head.group(0))
    if not addrs:
        print("no backtrace in log")
        return 1
    # Return addresses point after the call: look up addr-1 for callers.
    query = [addrs[0]] + [hex(int(a, 16) - 1) for a in addrs[1:]]
    out = subprocess.run([tool, "-f", "-C", "-i", "-e", exe] + query, capture_output=True, text=True).stdout
    lines = out.splitlines()
    i = 0
    for n, a in enumerate(addrs):
        if i + 1 >= len(lines):
            break
        fn, loc = lines[i], lines[i + 1]
        loc = loc.replace(str(ROOT).replace("\\", "/") + "/", "").replace(str(ROOT) + "\\", "")
        print(f"#{n:02d} {a}  {fn}  {loc}")
        i += 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
