#!/usr/bin/env python3
"""Minimal NitroFS reader for .nds images (FNT + FAT), no external deps.

    python tools/pc_port/nitrofs.py list ROM.nds
    python tools/pc_port/nitrofs.py extract ROM.nds OUT_DIR
    python tools/pc_port/nitrofs.py compare ROM.nds MANIFEST FILES_DIR

`compare` checks, for every path in MANIFEST (one NitroFS path per line, e.g.
build-pc/nitrofs_manifest.txt), whether it exists in the ROM and whether the
ROM copy is byte-identical to FILES_DIR/<path> when that file exists.
"""
import hashlib
import pathlib
import struct
import sys


def read_fs(rom):
    fnt_off, fnt_size, fat_off, fat_size = struct.unpack_from("<4I", rom, 0x40)
    nfiles = fat_size // 8
    fat = [struct.unpack_from("<2I", rom, fat_off + 8 * i) for i in range(nfiles)]
    files = {}

    def walk(dir_id, prefix):
        entry = fnt_off + 8 * (dir_id & 0xFFF)
        sub_off, first_file, _ = struct.unpack_from("<IHH", rom, entry)
        pos = fnt_off + sub_off
        fid = first_file
        while True:
            t = rom[pos]
            pos += 1
            if t == 0:
                break
            n = t & 0x7F
            name = rom[pos:pos + n].decode("ascii", "replace")
            pos += n
            if t & 0x80:
                sub = struct.unpack_from("<H", rom, pos)[0]
                pos += 2
                walk(sub, prefix + name + "/")
            else:
                start, end = fat[fid]
                files[prefix + name] = (start, end)
                fid += 1

    walk(0xF000, "")
    return files


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, rom_path = argv[1], argv[2]
    rom = pathlib.Path(rom_path).read_bytes()
    print(f"ROM: {rom[0:12].rstrip(bytes(1)).decode()} code {rom[12:16].decode()}", file=sys.stderr)
    files = read_fs(rom)
    if cmd == "list":
        for p, (s, e) in sorted(files.items()):
            print(f"{e - s:9} {p}")
    elif cmd == "extract":
        out = pathlib.Path(argv[3])
        for p, (s, e) in files.items():
            dst = out / p
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(rom[s:e])
        print(f"extracted {len(files)} files to {out}")
    elif cmd == "compare":
        manifest = [l.strip() for l in open(argv[3], encoding="utf-8") if l.strip()]
        files_dir = pathlib.Path(argv[4])
        stats = {"in_rom": 0, "missing_from_rom": 0, "same": 0, "different": 0, "local_absent": 0}
        diffs, missing = [], []
        for p in manifest:
            if p not in files:
                stats["missing_from_rom"] += 1
                missing.append(p)
                continue
            stats["in_rom"] += 1
            local = files_dir / p
            if not local.is_file():
                stats["local_absent"] += 1
                continue
            s, e = files[p]
            if hashlib.sha1(rom[s:e]).digest() == hashlib.sha1(local.read_bytes()).digest():
                stats["same"] += 1
            else:
                stats["different"] += 1
                diffs.append((p, e - s, local.stat().st_size))
        print(f"manifest: {len(manifest)} paths, ROM: {len(files)} files")
        for k, v in stats.items():
            print(f"  {k:18} {v}")
        if missing:
            print("missing from ROM:", " ".join(missing[:20]))
        print("different (path, rom size, local size):")
        for d in diffs:
            print(f"  {d[0]:50} {d[1]:9} {d[2]:9}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
