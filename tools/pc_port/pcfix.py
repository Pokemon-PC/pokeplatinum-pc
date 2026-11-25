#!/usr/bin/env python3
"""Applies a PC-only change to a game source while keeping the DS code:

    old  ->  #ifdef PLATFORM_PC
             // PC port: <why>
             new
             #else
             old
             #endif

Used from small patch scripts: pcfix.apply(path, old, new, why). `old` must
be whole lines and occur exactly once. The file's line endings are kept.
"""
import os

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def apply(path, old, new, why):
    p = os.path.join(ROOT, path)
    raw = open(p, encoding="utf-8", newline="").read()
    crlf = "\r\n" in raw
    text = raw.replace("\r\n", "\n")
    if text.count(old) != 1:
        raise SystemExit("%s: found %d times:\n%s" % (path, text.count(old), old))
    indent = old[:len(old) - len(old.lstrip())]
    block = "#ifdef PLATFORM_PC\n%s// PC port: %s\n%s\n#else\n%s\n#endif" % (indent, why, new, old)
    text = text.replace(old, block)
    if crlf:
        text = text.replace("\n", "\r\n")
    open(p, "w", encoding="utf-8", newline="").write(text)


def include(path, after, header, why):
    """Adds `#include header` (PC only) after the line `after`."""
    p = os.path.join(ROOT, path)
    raw = open(p, encoding="utf-8", newline="").read()
    crlf = "\r\n" in raw
    text = raw.replace("\r\n", "\n")
    if text.count(after + "\n") != 1:
        raise SystemExit("%s: anchor found %d times: %s" % (path, text.count(after + "\n"), after))
    text = text.replace(after + "\n", after + "\n#ifdef PLATFORM_PC\n// PC port: %s\n#include %s\n#endif\n" % (why, header), 1)
    if crlf:
        text = text.replace("\n", "\r\n")
    open(p, "w", encoding="utf-8", newline="").write(text)
