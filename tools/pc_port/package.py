#!/usr/bin/env python3
"""Builds a release folder and zip of the PC port.

    python tools/pc_port/package.py [--build build-pc] [--out dist]

Contents: the game executable (stripped), README.txt (docs/pc_port/README_PC.md) and
an empty mods/ folder (config.ini is written with defaults on first run). NO game data is ever packaged: on first run the game
extracts the player's own ROM (any *.nds file put next to the executable).

The unstripped executable is kept as dist/symbols/<name> so that crash logs
from a release can still be symbolized (tools/pc_port/symbolize_crash.py).
"""
import argparse
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
FORBIDDEN = (".nds", ".sav", ".narc", ".bin")   # never ship ROM data or saves


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build-pc")
    ap.add_argument("--out", default="dist")
    ap.add_argument("--name", default=None)
    args = ap.parse_args()

    build = ROOT / args.build
    ext = ".exe" if os.name == "nt" else ""
    exe_name = f"pokeplatinum{ext}"
    exe = build / exe_name
    if not exe.is_file():
        sys.exit(f"no {exe_name} in {build}: build the game first (cmake --build {args.build})")
    title = "Platinum"
    mods = "mods"

    system = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}.get(platform.system(), "unknown")
    name = args.name or f"Pokemon{title}-PC-{system}-{platform.machine().lower()}"
    out = ROOT / args.out
    folder = out / name
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    (out / "symbols").mkdir(parents=True, exist_ok=True)

    shutil.copy2(exe, out / "symbols" / exe_name)
    dst = folder / exe_name
    shutil.copy2(exe, dst)
    strip = shutil.which("llvm-strip") or shutil.which("strip")
    if strip:
        # llvm-strip / GNU strip: --strip-debug; Apple's strip only knows -S.
        is_apple = sys.platform == "darwin" and "llvm" not in pathlib.Path(strip).name
        subprocess.run([strip, "-S" if is_apple else "--strip-debug", str(dst)], check=True)
    else:
        print("warning: no strip tool found, the executable keeps its debug info")

    shutil.copy2(ROOT / "docs" / "pc_port" / "README_PC.md", folder / "README.txt")
    (folder / mods).mkdir()
    (folder / mods / "LISEZMOI-README.txt").write_text(
        "Fichiers de mod : meme chemin que dans la ROM (ex. poketool/personal/pl_personal.narc). Voir README.txt.\n"
        "Mod files: same path as inside the ROM (e.g. poketool/personal/pl_personal.narc). See README.txt.\n",
        encoding="utf-8")

    for p in folder.rglob("*"):
        if p.is_file() and p.suffix.lower() in FORBIDDEN:
            sys.exit(f"refusing to package game data: {p}")

    archive = out / f"{name}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(folder.rglob("*")):
            z.write(p, p.relative_to(out))
    size = archive.stat().st_size / (1024 * 1024)
    print(f"{folder}\n{archive} ({size:.1f} MiB)")


if __name__ == "__main__":
    main()
