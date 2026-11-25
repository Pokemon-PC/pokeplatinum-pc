#!/usr/bin/env python3
"""Assembles a data-only GNU assembler file of the game (macros + .long, no
ARM instructions) for the PC host, the way tools/scripts/make_script_bin.sh
does it for the DS: C preprocessor, then tools/enumproc (enum names ->
numbers), then the assembler, here Clang's for the host target.

    python tools/pc_port/asm_data.py --cc clang --enumproc enumproc
        -I include -I asm -I build/generated_root SOURCE.s OUTPUT.obj

Used for src/battle/trainer_ai/script.s (gTrainerAITable).
"""
import argparse
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", required=True)
    ap.add_argument("--enumproc", required=True)
    ap.add_argument("-I", dest="incs", action="append", default=[])
    ap.add_argument("source")
    ap.add_argument("output")
    a = ap.parse_args()
    inc = ["-I" + d for d in a.incs]
    pre = subprocess.run([a.cc, "-E", "-P", "-x", "assembler-with-cpp"] + inc + [a.source],
                         capture_output=True)
    if pre.returncode != 0:
        sys.stderr.write(pre.stderr.decode(errors="replace"))
        return 1
    enums = subprocess.run([a.enumproc], input=pre.stdout, capture_output=True)
    if enums.returncode != 0:
        sys.stderr.write(enums.stderr.decode(errors="replace"))
        return 1
    asm = subprocess.run([a.cc, "-c", "-x", "assembler-with-cpp", "-o", a.output, "-"] + inc,
                         input=enums.stdout, capture_output=True)
    if asm.returncode != 0:
        sys.stderr.write(asm.stderr.decode(errors="replace"))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
