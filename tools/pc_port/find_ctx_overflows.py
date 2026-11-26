#!/usr/bin/env python3
"""Finds "context" structs that outgrow a fixed-size buffer on the 64-bit PC.

The game keeps per-object work data in fixed byte arrays sized for the DS
(map object work areas of 16/32 bytes, the overworld animation context of
160 bytes, sprite animation data...). A struct holding pointers is larger
on PC and silently overflows into the fields that follow (function
pointers!). For each call `FUNC(x, sizeof(T))` (or a funcs table whose
first field is `sizeof(T)`), this compiles T's file once for the PC and once
for a 32-bit ARM target and prints both sizes against the buffer's limit.

    python tools/pc_port/find_ctx_overflows.py [build-pc/compile_commands.json]
"""
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# (regex capturing the type, limit in bytes on the PC build, what)
PATTERNS = [
    (r"sub_02062A54\(\s*\w+\s*,\s*sizeof\((\w+)\)", 16, "MapObject unk_D8"),
    (r"sub_02062A7C\(\s*\w+\s*,\s*sizeof\((\w+)\)", 16, "MapObject unk_E8"),
    (r"MapObject_InitMovementData\(\s*\w+\s*,\s*sizeof\((\w+)\)", 16, "MapObject movementData"),
    (r"sub_02062ACC\(\s*\w+\s*,\s*sizeof\((\w+)\)", 32, "MapObject unk_108"),
    (r"OverworldAnimManager(?:Extended)?Funcs \w+ = \{\s*(?:\{\s*)?(?:\.ctxSize\s*=\s*)?sizeof\((\w+)\)", 320, "OverworldAnimManager funcsCtx"),
]


def compile_size(entry, path, typename, target):
    args = [a.strip('"') for a in shlex.split(entry["command"], posix=False)]
    out, skip = [], False
    for a in args[1:]:
        if skip:
            skip = False
            continue
        if a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if a in ("-c", "-MD") or a.endswith(os.path.basename(path)):
            continue
        out.append(a)
    src = open(path, encoding="utf-8", errors="replace").read()
    src += "\nunsigned pc_sz_probe(void) { return sizeof(%s); }\n" % typename
    tmp = os.path.join(tempfile.gettempdir(), "ctxprobe_" + os.path.basename(path))
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(src)
    cmd = [args[0]] + (["--target=" + target] if target else []) + ["-Wno-everything", "-I", os.path.dirname(path)] \
        + out + ["-S", "-emit-llvm", "-O1", "-o", "-", tmp]
    r = subprocess.run(cmd, cwd=entry["directory"], capture_output=True, text=True)
    m = re.search(r"@pc_sz_probe\(\)[^{]*\{\s*(?:entry:\s*)?ret i32 (\d+)", r.stdout)
    return int(m.group(1)) if m else None


def main():
    cc_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build-pc", "compile_commands.json")
    byfile = {os.path.normpath(e["file"]).lower(): e for e in json.load(open(cc_path))}
    found = []
    for dirpath, _, files in os.walk(os.path.join(ROOT, "src")):
        if "platform_pc" in dirpath:
            continue
        for f in files:
            if f.endswith(".c"):
                p = os.path.join(dirpath, f)
                s = open(p, encoding="utf-8", errors="replace").read()
                for rx, limit, what in PATTERNS:
                    for m in re.finditer(rx, s):
                        found.append((p, m.group(1), limit, what))
    seen, bad = set(), 0
    for p, t, limit, what in found:
        if (p, t) in seen:
            continue
        seen.add((p, t))
        e = byfile.get(os.path.normpath(p).lower())
        if e is None:
            continue
        pc = compile_size(e, p, t, None)
        ds = compile_size(e, p, t, "armv7-w64-mingw32")
        over = pc is not None and pc > limit
        bad += over
        if over or "-v" in sys.argv:
            print("%-5s %-28s PC %4s DS %4s limit %3d  %s (%s)" % ("OVER" if over else "ok", what, pc, ds, limit, t,
                                                                 os.path.relpath(p, ROOT)))
    print("%d types checked, %d too large for their buffer on PC" % (len(seen), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
