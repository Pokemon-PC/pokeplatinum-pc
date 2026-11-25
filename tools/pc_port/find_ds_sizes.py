#!/usr/bin/env python3
"""Find heap allocations whose size was written for the DS (32-bit pointers).

Decompiled code often allocates with a literal size (`objectMax * 0x5C`,
`n * 4`) instead of sizeof(). That is right on the DS but too small on the
64-bit PC build whenever the allocated type contains pointers, and the
overflow corrupts the heap later, far from the cause.

For every call to an allocator whose size argument contains an integer
literal and no sizeof, this reports the call when the result is stored in a
`T *` and T holds pointers (or is a pointer), with sizeof(T) on PC.

Usage (libclang, e.g. build-pc/venv):
    python tools/pc_port/find_ds_sizes.py build-clang/compile_commands.json [paths...]
"""
import json
import pathlib
import shlex
import sys

import clang.cindex as ci

ALLOCATORS = {"Heap_Alloc", "Heap_AllocAtEnd", "AllocFromHeap", "AllocFromHeapAtEnd",
              "NNS_FndAllocFromExpHeapEx", "NNS_FndAllocFromAllocator"}


def has_pointer(t, depth=0):
    t = t.get_canonical()
    if t.kind in (ci.TypeKind.POINTER, ci.TypeKind.BLOCKPOINTER):
        return True
    if t.kind == ci.TypeKind.CONSTANTARRAY:
        return has_pointer(t.element_type, depth + 1)
    if t.kind == ci.TypeKind.RECORD and depth < 8:
        decl = t.get_declaration()
        return any(has_pointer(f.type, depth + 1) for f in decl.get_children()
                   if f.kind == ci.CursorKind.FIELD_DECL)
    return False


def size_arg_info(arg):
    """(has a literal, has sizeof) inside the size expression."""
    lit = so = False
    for c in arg.walk_preorder():
        if c.kind == ci.CursorKind.INTEGER_LITERAL:
            lit = True
        elif c.kind == ci.CursorKind.CXX_UNARY_EXPR or (
                c.kind == ci.CursorKind.UNEXPOSED_EXPR and "sizeof" in " ".join(t.spelling for t in c.get_tokens())):
            so = True
        if c.kind == ci.CursorKind.UNARY_OPERATOR or c.kind == ci.CursorKind.CXX_UNARY_EXPR:
            toks = [t.spelling for t in c.get_tokens()]
            if toks and toks[0] in ("sizeof", "_Alignof"):
                so = True
    return lit, so


def target_type(call, parents):
    """Type the allocation result is converted to, if any."""
    p = parents.get(call.hash)
    while p is not None and p.kind in (ci.CursorKind.UNEXPOSED_EXPR, ci.CursorKind.PAREN_EXPR,
                                       ci.CursorKind.CSTYLE_CAST_EXPR):
        if p.kind == ci.CursorKind.CSTYLE_CAST_EXPR:
            return p.type
        if p.type.kind == ci.TypeKind.POINTER and p.type.get_pointee().kind != ci.TypeKind.VOID:
            return p.type
        p = parents.get(p.hash)
    if p is None:
        return None
    if p.kind == ci.CursorKind.VAR_DECL:
        return p.type
    if p.kind == ci.CursorKind.BINARY_OPERATOR:
        lhs = next(p.get_children())
        return lhs.type
    return None


def scan(tu, root, out):
    parents = {}

    def walk(c):
        for ch in c.get_children():
            parents[ch.hash] = c
            walk(ch)
    walk(tu.cursor)
    for c in tu.cursor.walk_preorder():
        if c.kind != ci.CursorKind.CALL_EXPR or c.spelling not in ALLOCATORS:
            continue
        if c.location.file is None or not pathlib.Path(c.location.file.name).resolve().is_relative_to(root):
            continue
        args = list(c.get_arguments())
        if len(args) < 2:
            continue
        size = args[1]
        lit, so = size_arg_info(size)
        if not lit or so:
            continue
        t = target_type(c, parents)
        if t is None or t.get_canonical().kind != ci.TypeKind.POINTER:
            continue
        pointee = t.get_canonical().get_pointee()
        if pointee.kind == ci.TypeKind.VOID or not has_pointer(pointee):
            continue
        text = " ".join(tok.spelling for tok in size.get_tokens())
        loc = f"{pathlib.Path(c.location.file.name).resolve().relative_to(root)}:{c.location.line}"
        out.add((loc, f"{c.spelling}(.., {text}) -> {t.spelling} (sizeof {pointee.get_size()} on PC)"))


def main():
    db = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
    root = pathlib.Path(__file__).resolve().parents[2]
    filters = [str((root / f).resolve()) for f in sys.argv[2:]]
    index = ci.Index.create()
    found = set()
    for entry in db:
        src = str(pathlib.Path(entry["directory"], entry["file"]).resolve())
        if "platform_pc" in src or not src.endswith(".c"):
            continue
        if filters and not any(src.startswith(f) for f in filters):
            continue
        args = shlex.split(entry["command"], posix=False) if "command" in entry else entry["arguments"]
        args = [a.strip('"') for a in args[1:]]
        cleaned, skip = [], False
        for a in args:
            if skip:
                skip = False
                continue
            if a in ("-o", "-c", "-MF", "-MT", "-MQ"):
                skip = a != "-c"
                continue
            if a == src or a.endswith(entry["file"]) or a.startswith("-W") or a == "-w":
                continue
            cleaned.append(a)
        try:
            tu = index.parse(src, args=cleaned + ["-w"])
        except ci.TranslationUnitLoadError:
            print("parse failed:", src, file=sys.stderr)
            continue
        scan(tu, root, found)
    for loc, msg in sorted(found):
        print(f"{loc}: {msg}")
    print(f"{len(found)} suspicious allocations", file=sys.stderr)


if __name__ == "__main__":
    main()
