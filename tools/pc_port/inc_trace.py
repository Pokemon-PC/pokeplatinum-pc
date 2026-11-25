#!/usr/bin/env python3
"""Shows the include chain that brings a header into one source of the PC
build (clang -H), e.g. to find who pulls <windows.h>:

    python tools/pc_port/inc_trace.py src/foo.c windows.h [--build build-pc]
"""
import argparse
import json
import os
import shlex
import subprocess

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

ap = argparse.ArgumentParser()
ap.add_argument("source")
ap.add_argument("header")
ap.add_argument("--build", default="build-pc")
a = ap.parse_args()
db = json.load(open(os.path.join(ROOT, a.build, "compile_commands.json")))
want = os.path.normcase(os.path.normpath(os.path.join(ROOT, a.source)))
e = [x for x in db if os.path.normcase(os.path.normpath(x["file"])) == want][0]
args = [x.strip('"') for x in shlex.split(e["command"], posix=False)]
i = args.index("-o")
del args[i:i + 2]
args = [x for x in args if x not in ("-c", "-MD")]
for f in ("-MT", "-MF"):
    if f in args:
        i = args.index(f)
        del args[i:i + 2]
r = subprocess.run(args + ["-fsyntax-only", "-H", "-w"], cwd=e["directory"], capture_output=True, text=True,
                   encoding="utf-8", errors="replace")
lines = [l for l in r.stderr.splitlines() if l.startswith(".")]
hits = [k for k, l in enumerate(lines) if a.header.lower() in l.lower().replace("\\", "/")]
if not hits:
    print("not included")
else:
    k = hits[0]
    chain = [lines[k]]
    depth = len(lines[k].split(" ")[0])
    for j in range(k - 1, -1, -1):
        d = len(lines[j].split(" ")[0])
        if d < depth:
            chain.append(lines[j])
            depth = d
    for l in reversed(chain):
        print(l)
