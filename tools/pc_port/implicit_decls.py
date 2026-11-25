#!/usr/bin/env python3
"""Lists the functions a build log reports as implicitly declared, with the
file calling them and the header that declares them (if any), so that each
caller can include the right header under PLATFORM_PC.

An implicit declaration returns `int`: on a 64-bit host a returned pointer is
truncated, so these are real bugs on PC, not only warnings.

    python tools/pc_port/implicit_decls.py BUILD_LOG
"""
import collections
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def find_decl(fn):
    pat = re.compile(r"^[^#\n/]*\b%s\s*\(" % re.escape(fn), re.M)
    hits = []
    for d in ("include", "src", "lib"):
        for dp, _, fs in os.walk(os.path.join(ROOT, d)):
            for f in fs:
                if f.endswith(".h"):
                    p = os.path.join(dp, f)
                    t = open(p, encoding="utf-8", errors="replace").read()
                    m = pat.search(t)
                    if m:
                        hits.append((os.path.relpath(p, ROOT).replace(os.sep, "/"), m.group(0).strip()))
    return hits


def main():
    log = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    calls = collections.defaultdict(set)
    for f, fn in re.findall(r"^(\S+?):\d+:\d+: error: call to undeclared (?:library )?function '(\w+)'", log, re.M):
        calls[fn].add(f.split("pokeplatinum/")[-1].replace("\\", "/").split("pokeplatinum\\")[-1])
    for fn in sorted(calls):
        decl = find_decl(fn)
        print("%s  <- %s" % (fn, ", ".join(sorted(calls[fn]))))
        for h, d in decl[:2]:
            print("    %s: %s" % (h, d))
        if not decl:
            print("    (no declaration in a header)")


if __name__ == "__main__":
    main()
