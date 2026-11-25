#!/usr/bin/env python3
"""Find DS memory-layout assumptions that break on the 64-bit PC build.

Two bug families found while playing the PC port:

  read-past   a constant index or a cast reads beyond the end of a global
              object. On the DS the linker places .rodata objects back to back
              and the code relies on reaching the neighbour; on PC objects are
              aligned and ordered differently. Examples: Pokeathlon picture
              ids, the Battle Castle menu template.
                  ((const u32 *)&gX)[8]      *(T *)&gX[15]      gX[6] (gX has 2)

  raw-offset  a constant byte offset into a struct that holds pointers: the
              offset was computed with 4-byte pointers. Example: the Battle
              Tower reading PartyMenuArgs through `u8 *unk; unk[0x30]`.
                  ((u8 *)p)[0x26]      *(T *)((u8 *)p + 0x30)

Only code compiled for the game is scanned (not src/platform_pc), with the
build's own flags (compile_commands.json, PLATFORM_PC defined), so code
already fixed under #ifdef PLATFORM_PC is not reported.

Usage (libclang, e.g. build-pc/venv):
    python tools/pc_port/find_ds_offsets.py build-clang/compile_commands.json [-j N] [paths...]
"""
import argparse
import json
import multiprocessing
import os
import pathlib
import shlex
import sys

import clang.cindex as ci

ROOT = pathlib.Path(__file__).resolve().parents[2]


def has_pointer(t, depth=0):
    t = t.get_canonical()
    if t.kind in (ci.TypeKind.POINTER, ci.TypeKind.BLOCKPOINTER):
        return True
    if depth > 6:
        return False
    if t.kind in (ci.TypeKind.CONSTANTARRAY, ci.TypeKind.INCOMPLETEARRAY):
        return has_pointer(t.element_type, depth + 1)
    if t.kind == ci.TypeKind.RECORD:
        decl = t.get_declaration()
        return any(has_pointer(f.type, depth + 1) for f in decl.get_children() if f.kind == ci.CursorKind.FIELD_DECL)
    return False


def first_pointer_offset(t, base=0, depth=0):
    """Byte offset (PC) of the first pointer inside t, or None."""
    t = t.get_canonical()
    if t.kind in (ci.TypeKind.POINTER, ci.TypeKind.BLOCKPOINTER):
        return base
    if depth > 6:
        return None
    if t.kind == ci.TypeKind.CONSTANTARRAY:
        return first_pointer_offset(t.element_type, base, depth + 1)
    if t.kind == ci.TypeKind.RECORD:
        for f in t.get_declaration().get_children():
            if f.kind == ci.CursorKind.FIELD_DECL:
                off = f.get_field_offsetof()
                if off is None or off < 0:
                    continue
                r = first_pointer_offset(f.type, base + off // 8, depth + 1)
                if r is not None:
                    return r
    return None


def int_value(c):
    """Value of an integer constant expression made of literals, or None."""
    c = strip(c)
    if c.kind == ci.CursorKind.INTEGER_LITERAL:
        toks = [t.spelling for t in c.get_tokens()]
        if toks:
            try:
                return int(toks[0].rstrip("uUlL"), 0)
            except ValueError:
                return None
    if c.kind == ci.CursorKind.BINARY_OPERATOR:
        kids = list(c.get_children())
        toks = [t.spelling for t in c.get_tokens()]
        if len(kids) == 2:
            a, b = int_value(kids[0]), int_value(kids[1])
            if a is None or b is None:
                return None
            op_tokens = [t for t in toks if t in ("+", "-", "*", "<<")]
            if len(op_tokens) == 1:
                return {"+": a + b, "-": a - b, "*": a * b, "<<": a << b}[op_tokens[0]]
    return None


def strip(c):
    while c.kind in (ci.CursorKind.PAREN_EXPR, ci.CursorKind.UNEXPOSED_EXPR):
        kids = list(c.get_children())
        if len(kids) != 1:
            break
        c = kids[0]
    return c


def is_u8_ptr(t):
    t = t.get_canonical()
    return t.kind == ci.TypeKind.POINTER and t.get_pointee().get_canonical().kind in (
        ci.TypeKind.UCHAR, ci.TypeKind.CHAR_S, ci.TypeKind.SCHAR, ci.TypeKind.CHAR_U)


def under_root(cursor):
    f = cursor.location.file
    if f is None:
        return None
    p = pathlib.Path(f.name).resolve()
    try:
        rel = p.relative_to(ROOT).as_posix()
    except ValueError:
        return None
    if rel.startswith(("src/platform_pc", "build")):
        return None
    return rel


EXTRA_ARGS = []


def set_extra_args(extra):
    EXTRA_ARGS[:] = extra


def scan_tu(entry):
    args = shlex.split(entry["command"], posix=True)[1:] if "command" in entry else entry["arguments"][1:]
    keep, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-o", "-c", "-MF", "-MT", "-MQ"):
            skip = a != "-c"
            continue
        if a == entry["file"] or a.endswith((".c", ".obj", ".o")):
            continue
        keep.append(a)
    os.chdir(entry["directory"])
    index = ci.Index.create()
    try:
        tu = index.parse(entry["file"], args=keep + ["-w", "-ferror-limit=0"] + EXTRA_ARGS)
    except ci.TranslationUnitLoadError:
        return [], {}
    hits = []
    globals_ = {}
    u8_origin = {}   # u8* local -> pointee type of its initializer (record) or "void"   # name -> (size or None, type spelling, file) for complete definitions here

    def visit(c, parent_chain):
        k = c.kind
        if k == ci.CursorKind.VAR_DECL and c.semantic_parent is not None and \
                c.semantic_parent.kind == ci.CursorKind.TRANSLATION_UNIT:
            t = c.type.get_canonical()
            if c.is_definition() and t.kind != ci.TypeKind.INCOMPLETEARRAY:
                sz = t.get_size()
                if sz > 0:
                    globals_[c.spelling] = sz
        if k == ci.CursorKind.VAR_DECL and is_u8_ptr(c.type):
            for init in c.get_children():
                if init.kind in (ci.CursorKind.TYPE_REF,):
                    continue
                it = strip(init)
                if it.kind == ci.CursorKind.CSTYLE_CAST_EXPR:
                    it = strip(list(it.get_children())[-1])
                t = it.type.get_canonical()
                if t.kind == ci.TypeKind.POINTER:
                    p = t.get_pointee().get_canonical()
                    if p.kind == ci.TypeKind.VOID:
                        u8_origin[c.hash] = None
                    elif has_pointer(p):
                        u8_origin[c.hash] = p
        if k == ci.CursorKind.ARRAY_SUBSCRIPT_EXPR:
            rel = under_root(c)
            if rel:
                kids = list(c.get_children())
                if len(kids) == 2:
                    base, idx = strip(kids[0]), kids[1]
                    n = int_value(idx)
                    if n is not None:
                        elem = c.type.get_canonical().get_size()
                        # gX[N] on a global (sizes checked across TUs later)
                        if base.kind == ci.CursorKind.DECL_REF_EXPR and base.referenced is not None and \
                                base.referenced.kind == ci.CursorKind.VAR_DECL and \
                                base.referenced.semantic_parent.kind == ci.CursorKind.TRANSLATION_UNIT:
                            hits.append(("index", rel, c.location.line, base.spelling, (n + 1) * elem,
                                         base.referenced.type.get_canonical().kind == ci.TypeKind.INCOMPLETEARRAY))
                        # (&gX)[N]
                        if base.kind == ci.CursorKind.UNARY_OPERATOR and                                 [t.spelling for t in base.get_tokens()][:1] == ["&"]:
                            tgt = strip(list(base.get_children())[0])
                            if tgt.kind == ci.CursorKind.DECL_REF_EXPR and tgt.referenced is not None and                                     tgt.referenced.kind == ci.CursorKind.VAR_DECL:
                                hits.append(("index", rel, c.location.line, tgt.spelling, (n + 1) * elem, False))
                        # u8 *unk = <struct or void pointer>; unk[N]
                        if base.kind == ci.CursorKind.DECL_REF_EXPR and base.referenced is not None and                                 base.referenced.hash in u8_origin:
                            p = u8_origin[base.referenced.hash]
                            if p is None:
                                if n >= 0x10:
                                    hits.append(("raw", rel, c.location.line, "void * (" + base.spelling + ")", n, None))
                            else:
                                fpo = first_pointer_offset(p)
                                if fpo is not None and n >= fpo:
                                    hits.append(("raw", rel, c.location.line, p.spelling, n, None))
                        # ((T *)&gX)[N]  /  ((u8 *)p)[N]
                        cast = base
                        if cast.kind == ci.CursorKind.CSTYLE_CAST_EXPR:
                            inner = strip(list(cast.get_children())[-1])
                            if inner.kind == ci.CursorKind.UNARY_OPERATOR:
                                tgt = strip(list(inner.get_children())[0])
                                if tgt.kind == ci.CursorKind.DECL_REF_EXPR and tgt.referenced is not None and \
                                        tgt.referenced.kind == ci.CursorKind.VAR_DECL:
                                    hits.append(("index", rel, c.location.line, tgt.spelling, (n + 1) * elem, False))
                            elif is_u8_ptr(cast.type):
                                src_t = inner.type.get_canonical()
                                if src_t.kind == ci.TypeKind.POINTER and has_pointer(src_t.get_pointee()):
                                    fpo = first_pointer_offset(src_t.get_pointee())
                                    if fpo is not None and n >= fpo:
                                        hits.append(("raw", rel, c.location.line, src_t.get_pointee().spelling, n, None))
        if k == ci.CursorKind.BINARY_OPERATOR:
            # (u8 *)p + CONST where p points to a struct holding pointers
            kids = list(c.get_children())
            toks = [t.spelling for t in c.get_tokens()]
            if len(kids) == 2 and "+" in toks:
                lhs, rhs = strip(kids[0]), kids[1]
                n = int_value(rhs)
                # &gX + N  (pointer arithmetic from a global's address)
                if n is not None and n > 0 and lhs.kind == ci.CursorKind.UNARY_OPERATOR and                         [t.spelling for t in lhs.get_tokens()][:1] == ["&"]:
                    tgt = strip(list(lhs.get_children())[0])
                    pt = lhs.type.get_canonical()
                    rel = under_root(c)
                    if rel and tgt.kind == ci.CursorKind.DECL_REF_EXPR and tgt.referenced is not None and                             tgt.referenced.kind == ci.CursorKind.VAR_DECL and pt.kind == ci.TypeKind.POINTER:
                        esz = pt.get_pointee().get_size()
                        if esz > 0:
                            hits.append(("index", rel, c.location.line, tgt.spelling, (n + 1) * esz, False))
                if n is not None and n > 0 and lhs.kind == ci.CursorKind.CSTYLE_CAST_EXPR and is_u8_ptr(lhs.type):
                    inner = strip(list(lhs.get_children())[-1])
                    src_t = inner.type.get_canonical()
                    if src_t.kind == ci.TypeKind.POINTER and has_pointer(src_t.get_pointee()):
                        fpo = first_pointer_offset(src_t.get_pointee())
                        rel = under_root(c)
                        if rel and fpo is not None and n >= fpo:
                            hits.append(("raw", rel, c.location.line, src_t.get_pointee().spelling, n, None))
        for ch in c.get_children():
            visit(ch, parent_chain)

    visit(tu.cursor, [])
    return hits, globals_


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("compile_commands")
    ap.add_argument("paths", nargs="*")
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("--as-ds", action="store_true",
                    help="parse with PLATFORM_PC undefined (shows what the PC fixes replaced; for testing the scanner)")
    args = ap.parse_args()
    if args.as_ds:
        EXTRA_ARGS.append("-UPLATFORM_PC")
    entries = json.loads(pathlib.Path(args.compile_commands).read_text())
    norm = lambda e: e["file"].replace("\\", "/")
    data_entries = [e for e in entries if "/generated/asm_data/" in norm(e)]
    entries = [e for e in entries if "platform_pc" not in norm(e) and "_deps" not in e["file"] and e["file"].endswith(".c")]
    if args.paths:
        entries = [e for e in entries if any(p in norm(e) for p in args.paths)]
    entries = entries + data_entries   # their hits are outside the scanned tree (under_root filters them)
    with multiprocessing.Pool(args.j, initializer=set_extra_args, initargs=(list(EXTRA_ARGS),)) as pool:
        results = pool.map(scan_tu, entries, chunksize=4)
    sizes = {}
    hits = set()
    for h, g in results:
        hits |= set(h)
        for name, sz in g.items():
            sizes[name] = max(sizes.get(name, 0), sz)
    # The generated asm data defines byte arrays: their sizes are the DS sizes.
    reports = []
    for kind, rel, line, name, need, incomplete in sorted(hits):
        if kind == "index":
            have = sizes.get(name)
            if have is None or need <= have:
                continue
            reports.append(f"read-past  {rel}:{line}: {name} has {have} bytes, the code reads up to byte {need}")
        else:
            reports.append(f"raw-offset {rel}:{line}: byte offset {name and ''}{need} (0x{need:X}) into {name}")
    print("\n".join(reports))
    print(f"{len(reports)} findings in {len(entries)} files", file=sys.stderr)


if __name__ == "__main__":
    main()
