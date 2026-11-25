#!/usr/bin/env python3
"""Compares two NARC archives member by member (e.g. the repository's US
version and the one of the player's localized ROM): count, sizes, 4-byte tags,
and which members are identical. Helps find indices that moved.

    python tools/pc_port/narc_compare.py A.narc B.narc
"""
import hashlib
import struct
import sys


def members(path):
    d = open(path, "rb").read()
    hdr = struct.unpack_from("<H", d, 12)[0]
    n = struct.unpack_from("<H", d, hdr + 8)[0]
    fat = hdr + 12
    btaf_size = struct.unpack_from("<I", d, hdr + 4)[0]
    btnf = hdr + btaf_size
    btnf_size = struct.unpack_from("<I", d, btnf + 4)[0]
    img = btnf + btnf_size + 8
    out = []
    for i in range(n):
        s, e = struct.unpack_from("<2I", d, fat + 8 * i)
        out.append(d[img + s:img + e])
    return out


def tag(b):
    t = b[:4]
    return t.decode("ascii", "replace") if all(48 <= c < 123 for c in t) else t.hex()


def main():
    a, b = members(sys.argv[1]), members(sys.argv[2])
    print("members: %d vs %d" % (len(a), len(b)))
    hb = {hashlib.sha1(x).hexdigest(): i for i, x in enumerate(b)}
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else None
        y = b[i] if i < len(b) else None
        same = x is not None and y is not None and x == y
        moved = "" if same or x is None else ("  (A's member is B[%d])" % hb[hashlib.sha1(x).hexdigest()]
                                              if hashlib.sha1(x).hexdigest() in hb else "")
        print("%3d  %-6s %6s | %-6s %6s  %s%s" % (i, tag(x) if x else "-", len(x) if x else "-",
                                                tag(y) if y else "-", len(y) if y else "-",
                                                "=" if same else "", moved))


if __name__ == "__main__":
    main()
