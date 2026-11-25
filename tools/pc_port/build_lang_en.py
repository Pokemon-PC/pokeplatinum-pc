#!/usr/bin/env python3
"""Builds the English data layer (pc_data/en) from the repository, for the
language choice of the PC port (config.ini `language = en`, `--lang en`).

    python tools/pc_port/build_lang_en.py --template pc_data/fr \\
        --msgenc build-pc/hosttools/msgenc.exe \\
        --generated build-pc/generated_root --out pc_data/en

The player's ROM (any language) provides the whole game in pc_data/<lang>.
The English layer only holds what differs by language:
  - msgdata/pl_msg.narc: the 724 text banks, encoded with msgenc from the
    repository's res/text/*.json (and the banks the data tools generate:
    species, moves, items, trainers...), in generated/text_banks.txt order,
    packed like the ROM's archive (its file name table, 4-byte alignment);
  - the prebuilt files of the repository (res/prebuilt, platinum.us/filesys.csv)
    that differ from the ROM's: the repository has the English version.
Everything else is language-neutral and still comes from the ROM's layer
(src/platform_pc/fs_pc.c puts it under the English one).

Files the repository builds from pictures and that hold some text (title
logo, some menus) stay in the ROM's language.
"""
import argparse
import concurrent.futures
import filecmp
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MSG_NARC = os.path.join("msgdata", "pl_msg.narc")


def read_narc(path):
    d = open(path, "rb").read()
    if d[:4] != b"NARC":
        sys.exit(f"{path}: not a NARC")
    sections, o = {}, 16
    while o < len(d):
        tag, size = d[o:o + 4], struct.unpack_from("<I", d, o + 4)[0]
        sections[tag] = d[o:o + size]
        o += size
    count = struct.unpack_from("<H", sections[b"BTAF"], 8)[0]
    return count, sections[b"BTNF"]


def write_narc(path, members, btnf):
    gmif_data = bytearray()
    fat = []
    for m in members:
        while len(gmif_data) % 4:
            gmif_data += b"\xff"
        start = len(gmif_data)
        gmif_data += m
        fat.append((start, len(gmif_data)))
    while len(gmif_data) % 4:
        gmif_data += b"\xff"
    btaf = b"BTAF" + struct.pack("<IHH", 12 + 8 * len(fat), len(fat), 0) + \
        b"".join(struct.pack("<II", s, e) for s, e in fat)
    gmif = b"GMIF" + struct.pack("<I", 8 + len(gmif_data)) + bytes(gmif_data)
    body = btaf + btnf + gmif
    header = b"NARC" + struct.pack("<HHIHH", 0xFFFE, 0x0100, 16 + len(body), 16, 3)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".tmp", "wb") as f:
        f.write(header + body)
    os.replace(path + ".tmp", path)


def gen_town_map_bank(generated, tmp):
    """town_map.json: made by tools/scripts/make_town_map_data.py (res/town_map/meson.build)."""
    env = dict(os.environ, PYTHONPATH=generated, PYTHONUTF8="1", PYTHONWARNINGS="ignore")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "scripts", "make_town_map_data.py"),
                        "--input", os.path.join(ROOT, "res", "town_map", "town_map_data.json"),
                        "--signpost-naix", os.path.join(generated, "res", "graphics", "signposts", "field_board.naix"),
                        "--blocks", os.path.join(tmp, "town_map_blocks.dat"),
                        "--flags", os.path.join(tmp, "town_map_flags.dat"),
                        "--text-bank", os.path.join(tmp, "town_map.json")],
                       env=env, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        sys.exit(f"make_town_map_data.py: {r.stdout}{r.stderr}")


def bank_sources(generated, tmp):
    """The JSON file of each bank, in archive order."""
    gen_town_map_bank(generated, tmp)
    dirs = [os.path.join(ROOT, "res", "text"), tmp] + [os.path.join(generated, d) for d in (
        "res/pokemon", "res/items", "res/trainers", "res/moves", "res/trainers/frontier", "res")]
    out = []
    for line in open(os.path.join(ROOT, "generated", "text_banks.txt"), encoding="utf-8"):
        name = line.strip().replace("TEXT_BANK_", "").lower()
        if not name:
            continue
        for d in dirs:
            p = os.path.join(d, name + ".json")
            if os.path.exists(p):
                out.append(p)
                break
        else:
            sys.exit(f"text bank {name}: no {name}.json in res/text nor in the generated data")
    return out


def encode(msgenc, src, dst):
    charmap = os.path.join(ROOT, "tools", "msgenc", "charmap.txt")
    r = subprocess.run([msgenc, "-e", "--json", "-c", charmap, "-H", dst + ".h", src, dst],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"msgenc {src}: {r.stdout}{r.stderr}")
    return open(dst, "rb").read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--template", required=True, help="data extracted from the player's ROM")
    ap.add_argument("--msgenc", required=True)
    ap.add_argument("--generated", required=True, help="the build's generated_root (banks made by the data tools)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--stamp")
    a = ap.parse_args()
    a.msgenc = os.path.abspath(a.msgenc)

    count, btnf = read_narc(os.path.join(a.template, MSG_NARC))
    with tempfile.TemporaryDirectory() as tmp, concurrent.futures.ThreadPoolExecutor() as pool:
        sources = bank_sources(a.generated, tmp)
        if len(sources) != count:
            sys.exit(f"{len(sources)} text banks in generated/text_banks.txt, {count} in the ROM's {MSG_NARC}")
        jobs = [pool.submit(encode, a.msgenc, src, os.path.join(tmp, "%04d.bin" % i)) for i, src in enumerate(sources)]
        members = [j.result() for j in jobs]
    write_narc(os.path.join(a.out, MSG_NARC), members, btnf)

    # Prebuilt files of the repository (English) that differ from the ROM's.
    localized = 0
    for line in open(os.path.join(ROOT, "platinum.us", "filesys.csv"), encoding="utf-8").read().splitlines()[1:]:
        if "," not in line:
            continue
        src, rom = line.split(",")[0], line.split(",")[1].strip().lstrip("/")
        if not src.startswith("res/prebuilt/") or rom == MSG_NARC.replace(os.sep, "/"):
            continue
        repo, ours = os.path.join(ROOT, src), os.path.join(a.template, rom)
        if os.path.isfile(repo) and (not os.path.isfile(ours) or not filecmp.cmp(repo, ours, shallow=False)):
            dst = os.path.join(a.out, rom)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(repo, dst)
            localized += 1
    print(f"{a.out}: {count} English text banks, {localized} English files from res/prebuilt")
    if a.stamp:
        open(a.stamp, "w").close()


if __name__ == "__main__":
    main()
