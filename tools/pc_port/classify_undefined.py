#!/usr/bin/env python3
"""Classify unresolved link symbols of the PC build by where the DS build
defines them (asm data, excluded library source, C++ lobby, ...).

Usage: python tools/pc_port/classify_undefined.py UNDEFINED_LIST [--details]
UNDEFINED_LIST: one symbol per line (from lld "undefined symbol:" errors).
"""
import collections
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def index_definitions():
    defs = {}
    label = re.compile(r"^\s*([A-Za-z_]\w*):", re.M)
    for s in list(ROOT.glob("asm/*.s")) + list(ROOT.glob("lib/*/asm/*.s")) + list(ROOT.glob("lib/asm/*.s")):
        for name in label.findall(s.read_text(encoding="utf-8", errors="replace")):
            defs.setdefault(name, s.relative_to(ROOT).as_posix())
    func = re.compile(r"^(?:[A-Za-z_][\w \t\*]*?[\s\*])?([A-Za-z_]\w*)\s*\([^;]*$", re.M)
    var = re.compile(r"^(?:const\s+|static\s+)*[A-Za-z_]\w*[\s\*]+([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*=", re.M)
    for pattern in ("lib/**/*.c", "lib/**/*.cpp", "src/**/*.cpp", "src/**/*.c"):
        for f in ROOT.glob(pattern):
            if "platform_pc" in f.parts:
                continue
            text = f.read_text(encoding="utf-8", errors="replace")
            for name in func.findall(text) + var.findall(text):
                defs.setdefault(name, f.relative_to(ROOT).as_posix())
    return defs


def bucket(path):
    if path is None:
        return "unknown"
    if path.startswith("asm/"):
        return "asm data (asm/)"
    if path.startswith("lib/asm/") or "/asm/" in path:
        return "asm data (" + "/".join(path.split("/")[:2]) + ")"
    if path.endswith(".cpp"):
        return "C++ (" + "/".join(path.split("/")[:2]) + ")"
    parts = path.split("/")
    if path.startswith("lib/NitroSDK/src/"):
        return "NitroSDK/" + (parts[3] if len(parts) > 4 else parts[3].rsplit(".", 1)[0])
    if path.startswith("lib/"):
        return "/".join(parts[:2])
    return "C source not built: " + path


def main():
    undefined = [l.strip() for l in open(sys.argv[1], encoding="utf-8") if l.strip()]
    defs = index_definitions()
    groups = collections.defaultdict(list)
    for sym in undefined:
        groups[bucket(defs.get(sym))].append(sym)
    for name, syms in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print(f"{len(syms):5}  {name}")
        if "--details" in sys.argv or name == "unknown":
            print("       " + " ".join(sorted(syms)[:40]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
