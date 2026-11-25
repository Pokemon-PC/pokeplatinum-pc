#!/usr/bin/env python3
"""Compiles one or more sources of the PC build (syntax only) with the exact
flags CMake uses, and prints the first errors. For quick iteration:

    python tools/pc_port/cc1.py src/main.c [more sources...] [--build build-pc] [-n 20]
"""
import argparse
import json
import os
import shlex
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sources", nargs="+")
    ap.add_argument("--build", default="build-pc")
    ap.add_argument("-n", type=int, default=15, help="errors shown per file")
    a = ap.parse_args()
    db = json.load(open(os.path.join(ROOT, a.build, "compile_commands.json")))
    by_file = {os.path.normcase(os.path.normpath(e["file"])): e for e in db}
    bad = 0
    for src in a.sources:
        key = os.path.normcase(os.path.normpath(os.path.join(ROOT, src)))
        e = by_file.get(key)
        if e is None:
            print("%s: not in compile_commands.json" % src)
            bad += 1
            continue
        args = shlex.split(e["command"], posix=False) if "command" in e else e["arguments"]
        args = [x.strip('"') for x in args]
        if "-o" in args:
            i = args.index("-o")
            del args[i:i + 2]
        args = [x for x in args if x not in ("-c",)]
        for flag in ("-MD", "-MT", "-MF"):
            while flag in args:
                i = args.index(flag)
                del args[i:i + (1 if flag == "-MD" else 2)]
        args += ["-fsyntax-only", "-ferror-limit=%d" % a.n]
        r = subprocess.run(args, cwd=e["directory"], capture_output=True, text=True, encoding="utf-8", errors="replace")
        errs = [l for l in r.stderr.splitlines() if "error:" in l]
        print("%s: %s" % (src, "OK" if r.returncode == 0 else "%d errors" % len(errs)))
        for l in errs[:a.n]:
            print("   " + l.replace(ROOT.replace("\\", "/") + "/", ""))
        bad += r.returncode != 0
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
