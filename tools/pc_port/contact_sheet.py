#!/usr/bin/env python3
"""Tiles the game's screenshots (frame_*.png, place_*.png: RGB, as written
by the PC port) into one contact sheet, to review a scripted run at a glance.

    python tools/pc_port/contact_sheet.py OUT.png IMG... [--cols N] [--scale 2]
(--scale 2 halves each picture.)
"""
import argparse
import struct
import sys
import zlib


def read_png(path):
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", path
    o, idat = 8, b""
    w = h = 0
    while o < len(d):
        n = struct.unpack(">I", d[o:o + 4])[0]
        t = d[o + 4:o + 8]
        body = d[o + 8:o + 8 + n]
        if t == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", body[:10])
            bpp = {2: 3, 6: 4}[ct]
        elif t == b"IDAT":
            idat += body
        o += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line) if bpp == 3 else bytes(v for k, v in enumerate(line) if k % 4 != 3))
        prev = line
    return w, h, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("images", nargs="+")
    ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--scale", type=int, default=2)
    a = ap.parse_args()
    pics = [read_png(p) for p in a.images]
    s = a.scale
    cw = max(p[0] for p in pics) // s + 4
    ch = max(p[1] for p in pics) // s + 4
    cols = min(a.cols, len(pics))
    nrows = (len(pics) + cols - 1) // cols
    W, H = cols * cw, nrows * ch
    out = bytearray(b"\x40" * (W * H * 3))
    for i, (w, h, rows) in enumerate(pics):
        ox, oy = (i % cols) * cw + 2, (i // cols) * ch + 2
        for y in range(0, h - s + 1, s):
            r = rows[y]
            dy = oy + y // s
            for x in range(0, w - s + 1, s):
                o = (dy * W + ox + x // s) * 3
                out[o:o + 3] = r[x * 3:x * 3 + 3]
    raw = b"".join(b"\0" + bytes(out[y * W * 3:(y + 1) * W * 3]) for y in range(H))

    def chunk(t, b):
        c = struct.pack(">I", len(b)) + t + b
        return c + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    open(a.out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    print(f"{a.out}: {len(pics)} pictures, {cols}x{nrows}")


if __name__ == "__main__":
    sys.exit(main())
