#!/usr/bin/env python3
"""Compile the game's text (files/msgdata/msg/*.gmm) with the repository's
own msgenc, exactly like files/msgdata/msg.mk does:

  * msg_XXXX.h  (message IDs)  -> <hdr_dir>/msgdata/msg/   (needed to compile)
  * msg_XXXX.bin (encoded text) -> <bin_dir>/               (English text data)

msg_0729.gmm (trainer names) is first generated from
files/poketool/trainer/trainers.json with jsonproc, as in msg.mk.

Usage:
    python tools/pc_port/gen_msg.py HDR_DIR BIN_DIR [--hosttools DIR] [--jobs N]
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
MSG_DIR = ROOT / "files" / "msgdata" / "msg"


def keys_from_mk():
    text = (ROOT / "files" / "msgdata" / "msg.mk").read_text(encoding="utf-8")
    return {m.group(1): m.group(2)
            for m in re.finditer(r"files/msgdata/msg/(msg_\w+)\.bin: MSGENCFLAGS \+= -k (0x[0-9A-Fa-f]+)", text)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hdr_dir")
    ap.add_argument("bin_dir")
    ap.add_argument("--hosttools", default=str(ROOT / "build-pc" / "hosttools"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    tools = pathlib.Path(args.hosttools)
    hdr_dir = pathlib.Path(args.hdr_dir) / "msgdata" / "msg"
    bin_dir = pathlib.Path(args.bin_dir)
    hdr_dir.mkdir(parents=True, exist_ok=True)
    bin_dir.mkdir(parents=True, exist_ok=True)

    # msg_0729.gmm: trainer names, generated from JSON (+ '&' escaping, as msg.mk's sed does).
    built = bin_dir / "msg_0729.gmm"
    subprocess.run([str(tools / "jsonproc"), "files/poketool/trainer/trainers.json",
                    "files/poketool/trainer/trname.json.txt", str(built)], cwd=ROOT, check=True)
    built.write_text(built.read_text(encoding="utf-8").replace("&", "&amp;"), encoding="utf-8", newline="\n")

    gmms = sorted(MSG_DIR.glob("*.gmm"))
    if not any(g.stem == "msg_0729" for g in gmms):
        gmms.append(built)
    gmms.sort(key=lambda p: p.stem)
    keys = keys_from_mk()

    def tool_path(p):
        # msgenc derives the include guard from the -H path: keep it relative
        # and '/'-separated, or a '\' ends the macro name and every message
        # header after the first one is silently skipped.
        p = pathlib.Path(p).resolve()
        try:
            return p.relative_to(ROOT).as_posix()
        except ValueError:
            return p.as_posix()

    def encode(gmm):
        cmd = [str(tools / "msgenc"), "-e", "-c", "charmap.txt", "--gmm",
               "-H", tool_path(hdr_dir / (gmm.stem + ".h")), tool_path(gmm),
               tool_path(bin_dir / (gmm.stem + ".bin"))]
        if gmm.stem in keys:
            cmd += ["-k", keys[gmm.stem]]
        outs = [hdr_dir / (gmm.stem + ".h"), bin_dir / (gmm.stem + ".bin")]
        for attempt in range(2):
            p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            # msgenc has been seen to exit 0 without writing its outputs (file
            # briefly locked, e.g. by an antivirus scan): verify, retry once.
            if p.returncode == 0 and all(o.is_file() and o.stat().st_size > 0 for o in outs):
                return gmm.stem, 0, ""
        return gmm.stem, p.returncode or 1, (p.stderr or "outputs missing after msgenc")[-500:]

    failed = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for stem, rc, err in ex.map(encode, gmms):
            if rc != 0:
                failed.append((stem, err))
    print(f"msgenc: {len(gmms) - len(failed)}/{len(gmms)} message files compiled "
          f"({len(keys)} keys from msg.mk)")
    for stem, err in failed[:10]:
        print(f"  FAILED {stem}: {err.strip()}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
