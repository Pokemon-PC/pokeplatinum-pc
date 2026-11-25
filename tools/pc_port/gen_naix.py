#!/usr/bin/env python3
"""Generate NARC index headers (.naix) for the PC port without building data.

The DS build gets each X.naix from `nitroarc -cf X.narc --index-namespace X`,
which needs every member file to be built first (png -> NCGR, .s -> .bin ...).
The index only depends on member *names and order*, so this script predicts
the directory listing nitroarc will see after the build and writes the very
same header (same guard, same macro names, same values), reproducing
nitroarc's rules (tools/nitroarc/src/opmode_create.c):

  * members listed in <dir>/.narcorder come first, in that order;
  * every other file under <dir> (recursively) is added, sorted by strcmp,
    unless it matches one of the fnmatch patterns in <dir>/.narcignore
    (plus the built-in '.*ignore', '.*keep', '.*order');
  * names become identifiers: non [A-Za-z0-9_] chars -> '_', duplicates get
    a _N suffix.

Files that only exist after the build are taken from the directory's .mk
rules (the member lists they declare as prerequisites).

When a ROM file system is given (--rom-dir), each generated index is checked
against the member count of the corresponding NARC in the ROM.

Usage:
    python tools/pc_port/gen_naix.py OUT_DIR [--rom-dir pc_data/fr] NAIX...
    (NAIX = path relative to files/, e.g. msgdata/msg.naix)
"""
import argparse
import fnmatch
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
FILES = ROOT / "files"

MEMBER_EXT = re.compile(
    r"\.(NCGR|NCLR|NCER|NANR|NSCR|NMCR|NMAR|NSBMD|NSBCA|NSBTA|NSBTP|NSBMA|NSBVA|NSBTX|NFTR|"
    r"bin|dat|lz|narc|bbin|spa|sbnk|swar|sseq|sdat|txt)$", re.IGNORECASE)


SHORTNAME = "gold"   # config.mk: HEARTGOLD -> gold, SOULSILVER -> silver


def strip_map():
    """files/<source>.narc -> ROM path, from filesystem.mk arc_strip_name.
    Version-specific names (_$(shortname)) are also registered without the
    suffix, matching the version-agnostic .naix the Makefile derives."""
    m = {}
    text = (ROOT / "filesystem.mk").read_text(encoding="utf-8").replace("$(shortname)", SHORTNAME)
    for src, dst in re.findall(r"arc_strip_name,files/([^,]+),files/([^)\s]+)\)", text):
        m[src] = dst
        m.setdefault(src.replace("_" + SHORTNAME + ".narc", ".narc"), dst)
    return m


def narc_count(path):
    data = path.read_bytes()
    if data[:4] != b"NARC":
        return None
    btaf = struct.unpack_from("<H", data, 12)[0]
    return struct.unpack_from("<H", data, btaf + 8)[0]


def mk_listed_members(narc_dir: pathlib.Path, source_exts=()):
    """File names under narc_dir that the directory's .mk declares as built."""
    mk = narc_dir.with_suffix(".mk")
    if not mk.is_file():
        return set()
    text = mk.read_text(encoding="utf-8", errors="replace")
    text = text.replace("\\\n", " ")
    rel = narc_dir.relative_to(ROOT).as_posix()
    out = set()
    for tok in re.split(r"\s+", text):
        tok = tok.strip()
        if not tok or "%" in tok or "*" in tok:
            continue
        # "$(SOMETHING_DIR)/name" or "files/x/y/name" or bare "name"
        tok = re.sub(r"^\$\([A-Z0-9_]+_DIR\)/", "", tok)
        if tok.startswith(rel + "/"):
            tok = tok[len(rel) + 1:]
        if "/" in tok or "$" in tok or ":" in tok or "=" in tok:
            continue
        if MEMBER_EXT.search(tok) or any(tok.endswith("." + e) for e in source_exts):
            out.add(tok)
    return out


SUBST = re.compile(r"(?:\$\([A-Za-z0-9_]+:%\.(\w+(?:\.\w+)?)=%\.(\w+(?:\.\w+)?)\)"
                   r"|patsubst\s+%\.(\w+(?:\.\w+)?),\s*%\.(\w+(?:\.\w+)?))")


def mk_transforms(narc_dir: pathlib.Path):
    """Extension rewrites declared by the .mk: $(V:%.json=%.bin), patsubst %.gmm,%.bin ..."""
    mk = narc_dir.with_suffix(".mk")
    if not mk.is_file():
        return []
    text = mk.read_text(encoding="utf-8", errors="replace")
    pairs = []
    for m in SUBST.finditer(text):
        src, dst = (m.group(1), m.group(2)) if m.group(1) else (m.group(3), m.group(4))
        if (src, dst) not in pairs:
            pairs.append((src, dst))
    # VAR := $(OTHER:%.a=%.b) ... $(VAR:%=%.c)  =>  b -> b.c
    var_ext = {}
    for m in re.finditer(r"^(\w+)\s*:?=\s*\$\(\w+:%\.\w+=%\.(\w+)\)", text, re.M):
        var_ext[m.group(1)] = m.group(2)
    for m in re.finditer(r"\$\((\w+):%=%\.(\w+)\)", text):
        b = var_ext.get(m.group(1))
        if b and (b, b + "." + m.group(2)) not in pairs:
            pairs.append((b, b + "." + m.group(2)))
    return pairs


def apply_transforms(names, transforms):
    """Adds the outputs of the .mk rewrite rules, transitively."""
    names = set(names)
    changed = True
    while changed:
        changed = False
        for src, dst in transforms:
            for n in list(names):
                if n.endswith("." + src):
                    out = n[: -len(src)] + dst
                    if out not in names:
                        names.add(out)
                        changed = True
    return names


def predict_members(narc_dir: pathlib.Path, extra=()):
    order_file = narc_dir / ".narcorder"
    ignore_file = narc_dir / ".narcignore"
    excls = [".*ignore", ".*keep", ".*order"]
    if ignore_file.is_file():
        excls += [l.strip() for l in ignore_file.read_text(encoding="utf-8").splitlines() if l.strip()]

    explicit = []
    if order_file.is_file():
        explicit = [l.strip() for l in order_file.read_text(encoding="utf-8").splitlines() if l.strip()]

    # Rule-level tweaks in the .mk: `nitroarc --exclude="pat"` and
    # `ln -sf src $(X_DIR)/name` (a member created as a symlink at build time).
    mk = narc_dir.with_suffix(".mk")
    mk_extra = set()
    if mk.is_file():
        mk_text = mk.read_text(encoding="utf-8", errors="replace").replace("$(shortname)", SHORTNAME)
        excls += re.findall(r'--exclude="([^"]+)"', mk_text)
        mk_extra |= set(re.findall(r"ln -sf \S+ \$\([A-Z0-9_]+_DIR\)/(\S+)", mk_text))

    names = set(mk_extra)
    for p in narc_dir.rglob("*"):
        if p.is_file():
            names.add(p.relative_to(narc_dir).as_posix())
    transforms = mk_transforms(narc_dir)
    names |= mk_listed_members(narc_dir, {t[0] for t in transforms})
    names |= set(extra)
    names = apply_transforms(names, transforms)

    implicit = sorted(n for n in names
                      if n not in explicit and not any(fnmatch.fnmatchcase(n, e) for e in excls))
    return explicit + implicit


def narc_sizes(path):
    data = path.read_bytes()
    btaf = struct.unpack_from("<H", data, 12)[0]
    n = struct.unpack_from("<H", data, btaf + 8)[0]
    return [struct.unpack_from("<2I", data, btaf + 12 + 8 * i) for i in range(n)]


def s_blocks(s_text, defines=()):
    """(label, byte size) for each top-level label of a jsonproc .s output,
    honouring #ifdef/#ifndef/#else/#endif on the given defines."""
    blocks, stack = [], []
    for line in s_text.splitlines():
        t = line.split("//")[0].strip()
        m = re.match(r"#\s*(ifdef|ifndef)\s+(\w+)", t)
        if m:
            on = (m.group(2) in defines) == (m.group(1) == "ifdef")
            stack.append(on)
            continue
        if t.startswith("#else"):
            stack[-1] = not stack[-1]
            continue
        if t.startswith("#endif"):
            stack.pop()
            continue
        if not all(stack):
            continue
        m = re.match(r"^([A-Za-z_]\w*):$", t)
        if m:
            blocks.append([m.group(1), 0])
            continue
        for d, n in ((".word", 4), (".short", 2), (".hword", 2), (".byte", 1)):
            if t.startswith(d + " ") or t.startswith(d + "\t"):
                blocks[-1][1] += n * len(t[len(d):].split(","))
    return blocks


def render_o2narc(naix_rel, names):
    """Same text as tools/o2narc (WriteNaix)."""
    stem = pathlib.PurePosixPath(naix_rel).stem
    up = stem.upper()
    lines = ["/*", " * THIS FILE WAS AUTOMATICALLY", " *  GENERATED BY tools/o2narc", " *      DO NOT MODIFY!!!", " */",
             "", f"#ifndef NARC_{up}_NAIX_", f"#define NARC_{up}_NAIX_", "", "enum {"]
    for i, n in enumerate(names):
        lines.append(f"    NARC_{stem}_{n} = {i},")
    lines += ["};", "", f"#endif //NARC_{up}_NAIX_", ""]
    return "\n".join(lines)


def tool_generated(naix, out_root, rom_dir, smap, hosttools):
    """Indices produced by o2narc / csv2bin rather than nitroarc."""
    import subprocess
    stem = pathlib.PurePosixPath(naix).stem
    narc_rel = naix[:-len(".naix")] + ".narc"
    rom_file = pathlib.Path(rom_dir) / smap.get(narc_rel, narc_rel) if rom_dir else None
    dst = out_root / naix
    dst.parent.mkdir(parents=True, exist_ok=True)

    if naix == "itemtool/itemdata/item_data.naix":
        # csv2bin writes the index (and the NARC) itself.
        subprocess.run([str(hosttools / "csv2bin"), "compile", "files/itemtool/itemdata/item_data.csv",
                        str(out_root / narc_rel), "files/itemtool/itemdata/item_data.txt",
                        "-i", "include", "--naix", "--pad", "0xFF"], cwd=ROOT, check=True)
        n = len(re.findall(r"^\s+NARC_\w+ = \d+,", dst.read_text(), re.M))
        return n, narc_count(rom_file) if rom_file and rom_file.is_file() else None

    if naix == "application/zukanlist/zkn_data/zukan_data.naix":
        # jsonproc -> .s; o2narc -N names members after the data labels.
        tmp = out_root / (naix[:-len(".naix")] + ".s")
        subprocess.run([str(hosttools / "jsonproc"), "files/application/zukanlist/zkn_data/zukan_data.json",
                        "files/application/zukanlist/zkn_data/zukan_data.json.txt", str(tmp)], cwd=ROOT, check=True)
        blocks = s_blocks(tmp.read_text(encoding="utf-8"))
        dst.write_text(render_o2narc(naix, [b[0] for b in blocks]), encoding="utf-8", newline="\n")
        if rom_file and rom_file.is_file():
            rom = narc_sizes(rom_file)
            # The leading per-species tables (stats) are language-neutral and
            # must match exactly; the alphabetical sort lists that follow
            # legitimately differ between ROM languages (French names sort
            # differently), so only the count is checked for them.
            first_sort = next((i for i, b in enumerate(blocks) if b[0].startswith("sort_order_letters")), len(blocks))
            neutral_ok = all(e - s == b[1] for (s, e), b in zip(rom[:first_sort], blocks[:first_sort]))
            return len(blocks), len(rom) if neutral_ok else -1
        return len(blocks), None

    if naix in ("fielddata/encountdata/g_enc_data.naix", "fielddata/encountdata/s_enc_data.naix",
                "arc/ppark.naix", "application/zukanlist/zkn_data/zukan_enc.naix"):
        # o2narc -n: numbered names; only the member count matters.
        # Member counts of the HeartGold ROM (same in every language), used when
        # no ROM file system is available (e.g. CI builds without game data).
        known = {"fielddata/encountdata/g_enc_data.naix": 142, "fielddata/encountdata/s_enc_data.naix": 142,
                 "arc/ppark.naix": 1, "application/zukanlist/zkn_data/zukan_enc.naix": 3962}
        if rom_file and rom_file.is_file():
            count = narc_count(rom_file)
        else:
            count = known[naix]
        names = [f"{stem}_{i:08d}" for i in range(count)]
        dst.write_text(render_o2narc(naix, names), encoding="utf-8", newline="\n")
        return count, count
    return None


def guardify(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


def render(naix_rel, members):
    narc_rel = "files/" + naix_rel[:-len(".naix")] + ".narc"
    iname = narc_rel[:-len(".narc")] + ".naix"
    guard = guardify(iname)
    bname = pathlib.PurePosixPath(narc_rel).stem + "_"
    lines = ["/*", f" * This file was generated by nitroarc for '{narc_rel}'",
             " * DO NOT MODIFY IT; MANUAL EDITS WILL BE LOST", " */", "",
             f"#ifndef {guard}", f"#define {guard}", ""]
    seen = {}
    for i, m in enumerate(members):
        n = seen.get(m, 0)
        seen[m] = n + 1
        defn = guardify(m) + (f"_{n}" if n else "")
        lines.append(f"#define NARC_{bname}{defn} {i}")
    lines += ["", f"#define {guard[:-5]}_COUNT {len(members)}", "", f"#endif /* {guard} */", ""]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir")
    ap.add_argument("naix", nargs="*")
    ap.add_argument("--all", action="store_true",
                    help="generate every .naix #included by src/ and include/")
    ap.add_argument("--stamp", help="file touched on success (for build systems)")
    ap.add_argument("--rom-dir")
    ap.add_argument("--hosttools", default=str(ROOT / "build-pc" / "hosttools"),
                    help="directory holding csv2bin and jsonproc executables")
    args = ap.parse_args()
    if args.all:
        found = set()
        for d in ("src", "include"):
            for f in (ROOT / d).rglob("*"):
                if f.suffix in (".c", ".h") and "platform_pc" not in f.parts:
                    found |= set(re.findall(r'#include\s+"([^"]+\.naix)"',
                                            f.read_text(encoding="utf-8", errors="replace")))
        args.naix = sorted(set(args.naix) | found)
    smap = strip_map()
    out_root = pathlib.Path(args.out_dir)
    bad = 0
    for naix in args.naix:
        narc_dir = FILES / naix[:-len(".naix")]
        map_txt = narc_dir.with_suffix(".txt")
        if naix == "poketool/pokegra/otherpoke.naix" and map_txt.is_file():
            # Members are the converter outputs listed in otherpoke.txt, scanned
            # by nitroarc in build-otherpoke/ (sorted, nothing ignored).
            outs = [l.split()[1] for l in map_txt.read_text(encoding="utf-8").splitlines() if l.strip()]
            members = sorted(pathlib.PurePosixPath(o).name for o in outs)
            narc_dir = None
        elif not narc_dir.is_dir():
            res = tool_generated(naix, out_root, args.rom_dir, smap, pathlib.Path(args.hosttools))
            if res is None:
                print(f"SKIP  {naix}: no source directory and no known generator")
                bad += 1
                continue
            got, expect = res
            status = "ok   " if expect == got else ("?    " if expect is None else "BAD  ")
            bad += status == "BAD  "
            print(f"{status}{naix}: {got} members, ROM: {expect if expect != -1 else 'size mismatch'} (tool-generated)")
            continue
        if narc_dir is not None:
            members = predict_members(narc_dir)
        dst = out_root / naix
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(render(naix, members), encoding="utf-8", newline="\n")
        status = "ok   "
        if args.rom_dir:
            src_narc = naix[:-len(".naix")] + ".narc"
            rom_rel = smap.get(src_narc, src_narc)
            rom_file = pathlib.Path(args.rom_dir) / rom_rel
            expect = narc_count(rom_file) if rom_file.is_file() else None
            if expect is None:
                status = "?    "
            elif expect != len(members):
                status = "BAD  "
                bad += 1
            print(f"{status}{naix}: {len(members)} members, ROM {rom_rel}: {expect}")
        else:
            print(f"{status}{naix}: {len(members)} members")
    if bad:
        return 1
    if args.stamp:
        pathlib.Path(args.stamp).write_text(f"{len(args.naix)} indices\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
