#!/usr/bin/env python3
"""Convert the DS data-only assembly files (asm/*.s, lib/asm/*.s, ...) into C
for the 64-bit PC build.

Why: the .s files hold tables in DS memory layout (4-byte pointers). Data
that contains no pointer has the same layout on PC and is emitted as bytes;
data containing pointers is emitted as a typed C initializer so the host
compiler lays it out natively (8-byte pointers). Types come from the game's
own C declarations, read with libclang for the DS target (armv5te), so field
offsets match the bytes in the .s exactly.

Usage (needs the libclang Python package, e.g. in build-pc/venv):
    python tools/pc_port/asm_data_to_c.py OUT_DIR [--gen-dir build-clang/generated] FILE.s...

Writes, per input <name>.s:
    OUT_DIR/<name>_raw.c    pointer-free symbols as aligned byte arrays
    OUT_DIR/<name>_typed.c  pointer-carrying symbols as typed initializers
and OUT_DIR/asm_data_report.txt listing anything that needs a human.
"""
import argparse
import ast
import collections
import os
import pathlib
import re
import subprocess
import sys

import clang.cindex as ci

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFINES = ["HEARTGOLD", "GAME_REMASTER=0", "ENGLISH", "PM_KEEP_ASSERTS",
           "SDK_ARM9", "SDK_CODE_ARM", "SDK_FINALROM"]
C_INCLUDES = ["src", "include", "include/library", "files", "lib/include", "lib/NitroSDK/include",
              "lib/NitroSDK/include-mw", "lib/NitroSDK/include-pokediamond"]
ASM_INCLUDES = [".", "include", "asm/include", "files", "lib/asm/include", "lib/NitroDWC/asm/include",
                "lib/MSL_C/asm/include", "lib/NitroSDK/asm/include", "lib/syscall/asm/include", "asm",
                "files/msgdata", "lib/include"]

REPORT = []


def note(msg):
    REPORT.append(msg)


# --------------------------------------------------------------------------
# 1. Assembly reader
# --------------------------------------------------------------------------

class Symbol:
    def __init__(self, name, section, align):
        self.name = name
        self.section = section          # rodata / data / bss / text
        self.public = False
        self.align = align
        self.data = bytearray()
        self.relocs = []                # (offset, target, addend)
        self.ctype = None               # resolved TypeInfo
        self.decl = None                # declaration spelling source (for typed emission)
        self.elem_type = None           # set when typed as "array of <elem_type>" by propagation
        self.alias_of = None            # (owner Symbol, DS offset) for labels inside another object
        self.inner = []                 # labels merged into this object


class Reloc(Exception):
    pass


def strip_comment(line):
    out, q = [], None
    i = 0
    while i < len(line):
        c = line[i]
        if q:
            out.append(c)
            if c == "\\" and i + 1 < len(line):
                out.append(line[i + 1])
                i += 2
                continue
            if c == q:
                q = None
        elif c in "\"'":
            q = c
            out.append(c)
        elif line.startswith("//", i) or c in ";@" and (i == 0 or line[i - 1] in " \t"):
            break
        else:
            out.append(c)
        i += 1
    return "".join(out).strip()


def split_operands(s):
    ops, cur, depth, q = [], [], 0, None
    for c in s:
        if q:
            cur.append(c)
            if c == q:
                q = None
            continue
        if c in "\"'":
            q = c
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == "," and depth == 0:
            ops.append("".join(cur).strip())
            cur = []
            continue
        cur.append(c)
    if cur and "".join(cur).strip():
        ops.append("".join(cur).strip())
    return ops


class Expr:
    """Evaluates an operand: an int, or (symbol, addend) for a relocation."""

    def __init__(self, consts):
        self.consts = consts

    def eval(self, text):
        text = text.strip()
        if re.fullmatch(r"'(\\.|[^'])'", text):
            return ord(ast.literal_eval(text))
        py = text.replace("~", " ~")
        try:
            node = ast.parse(py, mode="eval").body
        except SyntaxError:
            raise ValueError(f"cannot parse expression {text!r}")
        return self._eval(node)

    def _eval(self, n):
        if isinstance(n, ast.Constant) and isinstance(n.value, int):
            return n.value
        if (isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id == "FS_OVERLAY_ID"
                and len(n.args) == 1 and isinstance(n.args[0], ast.Name)):
            # Overlay table index = order of `Overlay` sections in main.lsf.
            ids = overlay_ids()
            if n.args[0].id not in ids:
                raise ValueError(f"unknown overlay {n.args[0].id}")
            return ids[n.args[0].id]
        if isinstance(n, ast.Name):
            if n.id in self.consts:
                return self.consts[n.id]
            return (n.id, 0)
        if isinstance(n, ast.UnaryOp):
            v = self._eval(n.operand)
            if isinstance(v, tuple):
                raise ValueError("unary op on relocation")
            return {ast.USub: -v, ast.UAdd: v, ast.Invert: ~v}[type(n.op)]
        if isinstance(n, ast.BinOp):
            a, b = self._eval(n.left), self._eval(n.right)
            if isinstance(a, tuple) or isinstance(b, tuple):
                if isinstance(n.op, ast.Add) and isinstance(a, tuple) and isinstance(b, int):
                    return (a[0], a[1] + b)
                if isinstance(n.op, ast.Add) and isinstance(b, tuple) and isinstance(a, int):
                    return (b[0], b[1] + a)
                if isinstance(n.op, ast.Sub) and isinstance(a, tuple) and isinstance(b, int):
                    return (a[0], a[1] - b)
                raise ValueError("unsupported relocation expression")
            ops = {ast.Add: a + b, ast.Sub: a - b, ast.Mult: a * b, ast.LShift: a << b if b >= 0 else 0,
                   ast.RShift: a >> b if b >= 0 else 0, ast.BitOr: a | b, ast.BitAnd: a & b, ast.BitXor: a ^ b}
            if isinstance(n.op, (ast.Div, ast.FloorDiv)):
                return int(a / b) if b else 0
            return ops[type(n.op)]
        raise ValueError(f"unsupported expression node {ast.dump(n)}")


_OVERLAY_IDS = None


def overlay_ids():
    global _OVERLAY_IDS
    if _OVERLAY_IDS is None:
        names = re.findall(r"^Overlay\s+(\w+)\s*$", (ROOT / "main.lsf").read_text(encoding="utf-8"), re.M)
        _OVERLAY_IDS = {n: i for i, n in enumerate(names)}
    return _OVERLAY_IDS


def find_include(name, here):
    for d in [here] + [ROOT / d for d in ASM_INCLUDES]:
        p = (d / name)
        if p.is_file():
            return p
    return None


def preprocess(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    if not re.search(r"^\s*#\s*(include|define|if|ifdef|ifndef)\b", text, re.M):
        return text
    cmd = ["clang", "-E", "-P", "-x", "assembler-with-cpp", "-DPM_ASM", "-DSDK_ASM"]
    cmd += [f"-D{d}" for d in DEFINES]
    cmd += [f"-I{ROOT / d}" for d in ["build-clang/generated"] + C_INCLUDES + ["asm/include"]]
    cmd.append(str(path))
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if p.returncode != 0:
        note(f"{path.name}: C preprocessor failed: {p.stderr.strip()[:300]}")
        return text
    return p.stdout


def read_asm(path):
    """Returns (ordered list of Symbol, constants)."""
    consts, macros = {}, {}
    symbols, cur = [], None
    section = "text"
    pending_align = 4
    expr = Expr(consts)

    def lines_of(text, here, depth=0):
        it = iter(text.splitlines())
        for raw in it:
            line = strip_comment(raw)
            if not line or line.startswith("#"):
                continue
            m = re.match(r"\.macro\s+(\w+)\s*(.*)$", line)
            if m:
                params = [p.strip() for p in re.split(r"[,\s]+", m.group(2)) if p.strip()]
                body = []
                for b in it:
                    bs = strip_comment(b)
                    if re.match(r"\.endm\b", bs):
                        break
                    body.append(bs)
                macros[m.group(1)] = (params, body)
                continue
            m = re.match(r"\.include\s+\"([^\"]+)\"", line)
            if m:
                inc = find_include(m.group(1), here)
                if inc and depth < 8:
                    yield from lines_of(preprocess(inc), inc.parent, depth + 1)
                continue
            yield line

    def emit_line(line):
        nonlocal cur, section, pending_align
        while True:  # label(s) at line start
            m = re.match(r"([A-Za-z_.$][\w.$]*)\s*:\s*(.*)$", line)
            if not m:
                break
            name, line = m.group(1), m.group(2)
            cur = Symbol(name, section, pending_align)
            symbols.append(cur)
            pending_align = 1
            if not line:
                return
        m = re.match(r"(\.?[A-Za-z_][\w.]*)\s*(.*)$", line)
        if not m:
            note(f"unparsed line: {line}")
            return
        op, rest = m.group(1), m.group(2).strip()
        if op in macros:
            params, body = macros[op]
            args = split_operands(rest) if rest else []
            vals = {}
            for i, pdef in enumerate(params):
                pname, _, default = pdef.partition("=")
                vals[pname] = args[i] if i < len(args) else default
            for b in body:
                for k, v in vals.items():
                    b = b.replace("\\" + k, v)
                emit_line(b)
            return
        if op in (".text",):
            section = "text"; cur = None; return
        if op in (".rodata",):
            section = "rodata"; cur = None; return
        if op in (".data",):
            section = "data"; cur = None; return
        if op in (".bss",):
            section = "bss"; cur = None; return
        if op == ".section":
            n = rest.split(",")[0].strip().strip('"')
            section = "bss" if "bss" in n else "rodata" if "rodata" in n else "data" if "data" in n else "text"
            cur = None
            return
        if op in (".public", ".global", ".globl"):
            for n in split_operands(rest):
                pub.add(n.strip())
            return
        if op in (".extern", ".type", ".size", ".hidden", ".syntax", ".arm", ".thumb", ".align_func",
                  "#pragma", ".ident", ".file", ".option", ".force_thumb", ".endm"):
            return
        if op in (".set", ".equ"):
            k, v = [x.strip() for x in rest.split(",", 1)]
            consts[k] = expr.eval(v)
            return
        if op in (".balign", ".align", ".p2align"):
            ops = split_operands(rest)
            n = expr.eval(ops[0]) if ops else 4
            if op in (".align", ".p2align"):
                n = 1 << n
            fill = expr.eval(ops[1]) if len(ops) > 1 and ops[1] else 0
            if cur is None:
                pending_align = max(pending_align, n)
            else:
                while len(cur.data) % n:
                    cur.data.append(fill & 0xFF)
                pending_align = max(pending_align, n)
            return
        if cur is None:
            if op in (".byte", ".short", ".hword", ".word", ".4byte", ".2byte", ".space", ".skip",
                      ".ascii", ".asciz", ".string", ".fill", ".zero"):
                note(f"{path.name}: data before any label in section {section}: {line[:60]}")
            return
        if op in (".byte", ".short", ".hword", ".2byte", ".word", ".4byte"):
            size = {".byte": 1, ".short": 2, ".hword": 2, ".2byte": 2, ".word": 4, ".4byte": 4}[op]
            for o in split_operands(rest):
                v = expr.eval(o)
                if isinstance(v, tuple):
                    if size != 4:
                        note(f"{cur.name}: {size}-byte relocation to {v[0]}")
                    cur.relocs.append((len(cur.data), v[0], v[1]))
                    v = 0
                cur.data += (v & ((1 << (8 * size)) - 1)).to_bytes(size, "little")
            return
        if op in (".space", ".skip", ".zero"):
            ops = split_operands(rest)
            n = expr.eval(ops[0])
            fill = expr.eval(ops[1]) if len(ops) > 1 else 0
            cur.data += bytes([fill & 0xFF]) * n
            return
        if op == ".fill":
            ops = split_operands(rest)
            n, sz = expr.eval(ops[0]), expr.eval(ops[1]) if len(ops) > 1 else 1
            v = expr.eval(ops[2]) if len(ops) > 2 else 0
            for _ in range(n):
                cur.data += (v & ((1 << (8 * sz)) - 1)).to_bytes(sz, "little")
            return
        if op in (".ascii", ".asciz", ".string"):
            for o in split_operands(rest):
                s = ast.literal_eval(o)
                b = s.encode("latin-1") if isinstance(s, str) else s
                cur.data += b + (b"\0" if op != ".ascii" else b"")
            return
        note(f"{path.name}: unknown directive {op}")

    pub = set()
    for line in lines_of(preprocess(path), path.parent):
        try:
            emit_line(line)
        except (ValueError, SyntaxError) as e:
            note(f"{path.name}: {e}: {line[:80]}")
    for s in symbols:
        s.public = s.name in pub
    return [s for s in symbols if s.section != "text" or s.data]


# --------------------------------------------------------------------------
# 2. C types (DS layout) via libclang
# --------------------------------------------------------------------------

CLANG_ARGS = ["--target=armv5te-none-eabi", "-std=gnu99", "-fms-extensions", "-fsigned-char",
              "-ffreestanding", "-nostdinc", "-Wno-everything"] + [f"-D{d}" for d in DEFINES]


class Types:
    def __init__(self, gen_dir):
        self.args = CLANG_ARGS + [f"-I{gen_dir}"] + [f"-I{ROOT / d}" for d in C_INCLUDES]
        self.index = ci.Index.create()
        self.tus = {}
        self.decls = {}       # name -> (cursor type, declaring file)
        self.ident_files = None

    def _ident_index(self):
        idx = collections.defaultdict(set)
        for pattern in ("src/**/*.c", "include/**/*.h", "lib/include/**/*.h", "lib/**/src/*.c"):
            for f in ROOT.glob(pattern):
                if "platform_pc" in f.parts:
                    continue
                for w in set(re.findall(r"[A-Za-z_]\w+", f.read_text(encoding="utf-8", errors="replace"))):
                    idx[w].add(f)
        return idx

    def _parse(self, f):
        if f in self.tus:
            return self.tus[f]
        if f.suffix == ".h":
            tu = self.index.parse("probe.c", args=self.args,
                                  unsaved_files=[("probe.c", f'#include "global.h"\n#include "{f.as_posix()}"\n')])
        else:
            tu = self.index.parse(str(f), args=self.args)
        self.tus[f] = tu
        for c in tu.cursor.get_children():
            if c.kind == ci.CursorKind.VAR_DECL:
                prev = self.decls.get(c.spelling)
                t = c.type
                # Prefer a complete type (arrays with a size) over `T x[]`.
                if prev is None or (prev[0].kind == ci.TypeKind.INCOMPLETEARRAY and
                                    t.kind != ci.TypeKind.INCOMPLETEARRAY):
                    self.decls[c.spelling] = (t, c.location.file.name if c.location.file else str(f))
        return tu

    def lookup(self, name):
        if name in self.decls:
            return self.decls[name]
        if self.ident_files is None:
            self.ident_files = self._ident_index()
        for f in sorted(self.ident_files.get(name, ()), key=lambda p: (p.suffix != ".h", str(p))):
            self._parse(f)
            if name in self.decls:
                return self.decls[name]
        return None


def has_pointer(t, seen=None):
    t = t.get_canonical()
    k = t.kind
    if k in (ci.TypeKind.POINTER, ci.TypeKind.BLOCKPOINTER, ci.TypeKind.MEMBERPOINTER):
        return True
    if k in (ci.TypeKind.CONSTANTARRAY, ci.TypeKind.INCOMPLETEARRAY):
        return has_pointer(t.element_type)
    if k == ci.TypeKind.RECORD:
        return any(has_pointer(f.type) for f in t.get_fields())
    return False


# --------------------------------------------------------------------------
# 3. Typed initializer emission
# --------------------------------------------------------------------------

def read_int(data, off, size, signed):
    return int.from_bytes(bytes(data[off:off + size]).ljust(size, b"\0"), "little", signed=signed)


def field_name(f, index):
    # libclang spells anonymous struct/union members "struct X::(anonymous at ...)".
    if not f.spelling or "(anonymous" in f.spelling or f.is_anonymous():
        return f"_anon{index}"
    return f.spelling


class Mirror:
    """Re-declares DS C types for the PC build without including game headers:
    same members, same order, fixed-width integers, `const void *` for every
    pointer. The host compiler then lays them out exactly like the original
    types (natural alignment on both sides)."""

    def __init__(self):
        self.defs = []
        self.names = {}

    def scalar(self, t):
        t = t.get_canonical()
        k = t.kind
        if k in (ci.TypeKind.POINTER, ci.TypeKind.BLOCKPOINTER):
            return "const void *"
        if k == ci.TypeKind.FLOAT:
            return "float"
        if k == ci.TypeKind.DOUBLE:
            return "double"
        size = t.get_size()
        signed = k in (ci.TypeKind.CHAR_S, ci.TypeKind.SCHAR, ci.TypeKind.SHORT, ci.TypeKind.INT,
                       ci.TypeKind.LONG, ci.TypeKind.LONGLONG, ci.TypeKind.ENUM, ci.TypeKind.WCHAR)
        return f"{'int' if signed else 'uint'}{size * 8}_t"

    def type_name(self, t):
        t = t.get_canonical()
        if t.kind != ci.TypeKind.RECORD:
            return self.scalar(t)
        key = t.spelling
        if key in self.names:
            return self.names[key]
        base = re.sub(r"\W+", "_", t.get_declaration().spelling or "anon").strip("_") or "anon"
        name = f"pcm_{base}_{len(self.names)}"
        self.names[key] = name
        kw = "union" if t.get_declaration().kind == ci.CursorKind.UNION_DECL else "struct"
        members = []
        for i, f in enumerate(t.get_fields()):
            n = field_name(f, i)
            if f.is_bitfield():
                members.append(f"    {self.scalar(f.type)} {n} : {f.get_bitfield_width()};")
            else:
                members.append(f"    {self.declarator(f.type, n)};")
        self.defs.append(f"typedef {kw} {{\n" + "\n".join(members) + f"\n}} {name};\n")
        return name

    def declarator(self, t, name, count=None):
        t = t.get_canonical()
        dims = []
        while t.kind in (ci.TypeKind.CONSTANTARRAY, ci.TypeKind.INCOMPLETEARRAY):
            if t.kind == ci.TypeKind.CONSTANTARRAY:
                dims.append(str(t.element_count))
            else:
                dims.append(str(count) if count is not None else "")
            t = t.element_type.get_canonical()
        return f"{self.type_name(t)} {name}" + "".join(f"[{d}]" for d in dims)


class Emitter:
    def __init__(self, sym, symtab):
        self.sym = sym
        self.symtab = symtab
        self.relocs = {off: (t, a) for off, t, a in sym.relocs}
        self.used = set()
        self.referenced = set()

    def target_expr(self, target, addend):
        tgt = self.symtab.get(target)
        typed_here = (tgt is not None and tgt.ctype is not None and has_pointer(tgt.ctype)
                      and tgt.file == self.sym.file)
        if typed_here:
            ref = target                      # mirror-typed symbol defined in this file
        else:
            self.referenced.add(target)
            ref = "pcref_" + target           # linker alias, see PCREF_DEF
        if addend == 0:
            return f"(const void *)&{ref}"
        if tgt is not None and tgt.ctype is not None and has_pointer(tgt.ctype):
            path = member_path(tgt.ctype, addend)
            if path is not None and typed_here:
                return f"(const void *)&{target}{path}"
            note(f"{self.sym.name}: pointer into the middle of typed {target}+{addend:#x}")
        return f"(const void *)((const char *)&{ref} + {addend})"

    def value(self, t, off, indent):
        t = t.get_canonical()
        k = t.kind
        pad = "    " * indent
        if k == ci.TypeKind.RECORD:
            decl = t.get_declaration()
            is_union = decl.kind == ci.CursorKind.UNION_DECL
            fields = list(t.get_fields())
            if is_union:
                if has_pointer(t) and len(fields) > 1:
                    note(f"{self.sym.name}: union with pointer members at +{off:#x} (first member used)")
                fields = fields[:1]
            parts = []
            for i, f in enumerate(fields):
                foff_bits = f.get_field_offsetof()
                name = field_name(f, i)
                if f.is_bitfield():
                    width = f.get_bitfield_width()
                    unit = (foff_bits // 8) & ~3
                    word = read_int(self.sym.data, off + unit, 4, False)
                    v = (word >> (foff_bits - unit * 8)) & ((1 << width) - 1)
                    parts.append(f"{pad}    .{name} = {v},")
                    continue
                parts.append(f"{pad}    .{name} = {self.value(f.type, off + foff_bits // 8, indent + 1).lstrip()},")
            return "{\n" + "\n".join(parts) + f"\n{pad}}}"
        if k in (ci.TypeKind.CONSTANTARRAY, ci.TypeKind.INCOMPLETEARRAY):
            et = t.element_type
            esz = et.get_canonical().get_size()
            n = t.element_count if k == ci.TypeKind.CONSTANTARRAY else (len(self.sym.data) - off) // max(esz, 1)
            if et.get_canonical().kind in (ci.TypeKind.RECORD, ci.TypeKind.CONSTANTARRAY):
                items = [f"{pad}    {self.value(et, off + i * esz, indent + 1).lstrip()}," for i in range(n)]
                return "{\n" + "\n".join(items) + f"\n{pad}}}"
            items = [self.value(et, off + i * esz, indent + 1).strip() for i in range(n)]
            rows = [", ".join(items[i:i + 8]) for i in range(0, len(items), 8)]
            return "{ " + (",\n" + pad + "  ").join(rows) + " }"
        if k == ci.TypeKind.POINTER:
            if off in self.relocs:
                self.used.add(off)
                tgt, add = self.relocs[off]
                return self.target_expr(tgt, add)
            v = read_int(self.sym.data, off, 4, False)
            return "NULL" if v == 0 else f"(const void *)(uintptr_t){v:#x}"
        size = t.get_size()
        signed = k in (ci.TypeKind.CHAR_S, ci.TypeKind.SCHAR, ci.TypeKind.SHORT, ci.TypeKind.INT,
                       ci.TypeKind.LONG, ci.TypeKind.LONGLONG, ci.TypeKind.ENUM)
        if off in self.relocs and size == 4:
            self.used.add(off)
            tgt, add = self.relocs[off]
            note(f"{self.sym.name}: address of {tgt} stored in integer field (+{off:#x}); "
                 f"not representable on 64-bit")
            return "0"
        if k in (ci.TypeKind.FLOAT, ci.TypeKind.DOUBLE):
            import struct
            return repr(struct.unpack("<f" if size == 4 else "<d", bytes(self.sym.data[off:off + size]))[0])
        v = read_int(self.sym.data, off, size, signed)
        return str(v) if -10 <= v <= 255 or signed else f"{v:#x}"


def member_path(t, off):
    """C designator (e.g. "[3].field") of the member starting at DS byte offset `off`."""
    t = t.get_canonical()
    path = ""
    while off > 0 or t.kind in (ci.TypeKind.CONSTANTARRAY, ci.TypeKind.INCOMPLETEARRAY):
        if t.kind in (ci.TypeKind.CONSTANTARRAY, ci.TypeKind.INCOMPLETEARRAY):
            esz = t.element_type.get_canonical().get_size()
            path += f"[{off // esz}]"
            off %= esz
            t = t.element_type.get_canonical()
            if off == 0:
                return path
        elif t.kind == ci.TypeKind.RECORD:
            best = None
            for i, f in enumerate(t.get_fields()):
                fo = f.get_field_offsetof() // 8
                if 0 <= fo <= off and (best is None or fo >= best[0]):
                    best = (fo, f, i)
            if best is None:
                return None
            path += "." + field_name(best[1], best[2])
            off -= best[0]
            t = best[1].type.get_canonical()
        else:
            return None
    return path


# --------------------------------------------------------------------------
# 4. Driver
# --------------------------------------------------------------------------

def c_bytes(data):
    rows = []
    for i in range(0, len(data), 16):
        rows.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    return "\n".join(rows)


def convert(files, out_dir, types):
    all_syms = {}
    per_file = []
    for f in files:
        syms = read_asm(f)
        for sym in syms:
            sym.file = f
        per_file.append((f, syms))
        for s in syms:
            if s.name in all_syms:
                note(f"{s.name}: defined in {all_syms[s.name][0].name} and {f.name}")
            all_syms[s.name] = (f, s)
    symtab = {n: s for n, (f, s) in all_syms.items()}

    # Types from C declarations.
    for name, s in symtab.items():
        found = types.lookup(name)
        if found:
            s.ctype, s.decl = found

    # Labels that fall inside a typed object are aliases of one of its members
    # (DS layout). Merge their bytes into the enclosing object, which is then
    # emitted whole; the inner labels become linker aliases (see emit_aliases).
    for f, syms in per_file:
        for i, s in enumerate(syms):
            if (s.alias_of is not None or s.ctype is None
                    or s.ctype.get_canonical().kind == ci.TypeKind.INCOMPLETEARRAY):
                continue
            ds_size = s.ctype.get_canonical().get_size()
            j = i + 1
            while ds_size > len(s.data) and j < len(syms) and syms[j].section == s.section:
                inner = syms[j]
                start = len(s.data)
                inner.alias_of = (s, start)
                s.data += inner.data
                s.relocs += [(off + start, t, a) for off, t, a in inner.relocs]
                s.inner.append(inner)
                j += 1
            if s.inner:
                note(f"{f.name}: aliases inside {s.name}: "
                     + ", ".join(f"{x.name}@+{x.alias_of[1]:#x}" for x in s.inner))

    # Propagate pointee types to undeclared targets of typed pointers.
    changed = True
    while changed:
        changed = False
        for s in symtab.values():
            if s.ctype is None or not s.relocs:
                continue
            for off, tgt, add in s.relocs:
                ts = symtab.get(tgt)
                if ts is None or ts.ctype is not None or add != 0:
                    continue
                ft = field_type_at(s.ctype if s.elem_type is None else s.elem_type,
                                   off if s.elem_type is None else off % max(s.elem_type.get_size(), 1))
                if ft is not None and ft.kind == ci.TypeKind.POINTER:
                    pt = ft.get_pointee().get_canonical()
                    # The target is an array of the pointee type (one element or
                    # more, as many as its size allows).
                    if pt.get_size() > 0 and has_pointer(pt):
                        ts.ctype = pt
                        ts.elem_type = pt
                        ts.decl = None
                        changed = True

    out_dir.mkdir(parents=True, exist_ok=True)
    stats = collections.Counter()
    for f, syms in per_file:
        raw, typed, externs, forward = [], [], set(), []
        mirror = Mirror()
        for s in syms:
            if s.alias_of is not None:
                continue                      # emitted as an alias of its enclosing object
            is_const = s.section == "rodata"
            size = len(s.data)
            if s.ctype is None or not has_pointer(s.ctype):
                if s.relocs:
                    # Unknown type but holds addresses. Only a table made purely of
                    # addresses (or NULL / -1 sentinels) can be re-laid out safely;
                    # anything mixing plain integers needs its real type.
                    words = [read_int(s.data, i, 4, False) for i in range(0, size - size % 4, 4)]
                    reloc_offs = {o for o, _, _ in s.relocs}
                    pure = (size % 4 == 0 and all(o % 4 == 0 for o in reloc_offs) and
                            all(i * 4 in reloc_offs or w in (0, 0xFFFFFFFF) for i, w in enumerate(words)))
                    if pure:
                        items = []
                        for i in range(0, size, 4):
                            r = next(((t, a) for o, t, a in s.relocs if o == i), None)
                            if r:
                                externs.add(r[0])
                                items.append(f"(const void *)&pcref_{r[0]}" if r[1] == 0 else
                                             f"(const void *)((const char *)&pcref_{r[0]} + {r[1]})")
                            else:
                                v = read_int(s.data, i, 4, False)
                                items.append("NULL" if v == 0 else f"(const void *)(uintptr_t){v:#x}")
                        note(f"{f.name}: {s.name}: no C type, emitted as an array of pointers")
                        typed.append(f"{'const void *const' if is_const else 'const void *'} {s.name}[{size // 4}] = {{\n    "
                                     + ",\n    ".join(items) + ",\n};\n")
                        stats["pointer-table"] += 1
                        continue
                    note(f"{f.name}: {s.name}: no C type and mixes integers with addresses (skipped; "
                         f"needs a declaration)")
                    stats["skipped"] += 1
                    continue
                align = max(s.align, 8)
                q = "const " if is_const else ""
                if s.section == "bss" and not any(s.data):
                    raw.append(f"__attribute__((aligned({align}))) unsigned char {s.name}[{max(size, 1)}];\n")
                else:
                    raw.append(f"__attribute__((aligned({align}))) {q}unsigned char {s.name}[{max(size, 1)}] = {{\n"
                               f"{c_bytes(s.data)}\n}};\n")
                # Pointer-free: the PC layout is the DS layout, offsets carry over.
                for inner in s.inner:
                    raw.append(f'__asm__(".globl " PCSYM({inner.name}) "\\n.set " PCSYM({inner.name}) ", " '
                               f'PCSYM({s.name}) "+{inner.alias_of[1]}");\n')
                stats["raw"] += 1
                continue
            # Typed symbol (contains pointers): mirror type + initializer.
            em = Emitter(s, symtab)
            decl_type = s.ctype.get_canonical()
            count = None
            if s.elem_type is not None:
                esz = max(s.elem_type.get_size(), 1)
                count = max(size // esz, 1)
                if size % esz:
                    note(f"{f.name}: {s.name}: {size % esz} trailing byte(s) beyond {count} element(s) dropped")
                spelled = ("const " if is_const else "") + mirror.declarator(s.elem_type, s.name) + f"[{count}]"
            else:
                if decl_type.kind == ci.TypeKind.INCOMPLETEARRAY:
                    count = size // max(decl_type.element_type.get_canonical().get_size(), 1)
                spelled = ("const " if is_const else "") + mirror.declarator(decl_type, s.name, count)
            forward.append(f"extern {spelled};")
            if s.section == "bss" and not any(s.data):
                typed.append(f"{spelled};\n")
            else:
                if s.elem_type is not None:
                    esz = s.elem_type.get_size()
                    items = [em.value(s.elem_type, i * esz, 1).strip() for i in range(count)]
                    body = "{\n    " + ",\n    ".join(items) + ",\n}"
                else:
                    body = em.value(decl_type, 0, 0)
                typed.append(f"{spelled} = {body};\n")
                missing = [o for o, _, _ in s.relocs if o not in em.used]
                if missing:
                    note(f"{f.name}: {s.name}: relocations at {missing} not matched to pointer fields")
                externs |= em.referenced
            # Inner labels: alias at the PC offset of the member they designate.
            for inner in s.inner:
                expr = pc_offset_expr(decl_type, inner.alias_of[1], mirror)
                if expr is None:
                    note(f"{f.name}: {inner.name}: alias at DS +{inner.alias_of[1]:#x} of {s.name} "
                         f"is not at a member boundary (not emitted)")
                    continue
                typed.append(f"__attribute__((used)) static void pc_alias_{inner.name}(void) {{\n"
                             f'    __asm__ volatile(".globl " PCSYM({inner.name}) "\\n.set " PCSYM({inner.name}) ", " '
                             f'PCSYM({s.name}) "+%c0" :: "i"({expr}));\n}}\n')
            stats["typed"] += 1
        stem = f.stem
        header = f"/* Generated from {f.relative_to(ROOT).as_posix()} by tools/pc_port/asm_data_to_c.py - do not edit. */\n"
        (out_dir / f"{stem}_raw.c").write_text(header + PCREF_DEF + "\n" + "\n".join(raw), encoding="utf-8", newline="\n")
        ext = "".join(f"PCREF({e});\n" for e in sorted(externs))
        types_txt = ("\n/* DS types re-declared with the same members (layout identical to the\n"
                     " * game's own declarations once compiled for the host) */\n" + "\n".join(mirror.defs)
                     if mirror.defs else "")
        (out_dir / f"{stem}_typed.c").write_text(
            header + "\n#include <stddef.h>\n#include <stdint.h>\n" + PCREF_DEF +
            ("\n/* referenced symbols: linker aliases, only their address is used */\n" + ext if ext else "") +
            types_txt + ("\n" + "\n".join(forward) + "\n" if forward else "") +
            "\n" + "\n".join(typed), encoding="utf-8", newline="\n")
    return stats


PCREF_DEF = r"""
/* pcref_X is a distinct C name bound to the linker symbol X, so referencing
 * it never conflicts with however a header declares X. */
#define PCREF_STR2(x) #x
#define PCREF_STR(x) PCREF_STR2(x)
#define PCREF(sym) extern char pcref_##sym[] __asm__(PCREF_STR(__USER_LABEL_PREFIX__) #sym)
#define PCSYM(sym) PCREF_STR(__USER_LABEL_PREFIX__) #sym
"""


def pc_offset_expr(t, ds_off, mirror):
    """C constant expression giving the PC offset of the member found at DS
    offset `ds_off` in type `t` (mirror types), or None."""
    path = member_path(t, ds_off) if ds_off else ""
    if path is None:
        return None
    t = t.get_canonical()
    if not path:
        return "0"
    if t.kind in (ci.TypeKind.CONSTANTARRAY, ci.TypeKind.INCOMPLETEARRAY):
        m = re.match(r"\[(\d+)\](.*)$", path)
        elem = t.element_type.get_canonical()
        ename = mirror.type_name(elem) if elem.kind == ci.TypeKind.RECORD else mirror.declarator(elem, "").strip()
        base = f"{m.group(1)} * sizeof({ename})"
        rest = m.group(2)
        if not rest:
            return base
        if elem.kind != ci.TypeKind.RECORD:
            return None
        return f"{base} + __builtin_offsetof({ename}, {rest.lstrip('.')})"
    if t.kind == ci.TypeKind.RECORD:
        return f"__builtin_offsetof({mirror.type_name(t)}, {path.lstrip('.')})"
    return None


def rel_include(h):
    hp = pathlib.Path(h).resolve()
    for d in C_INCLUDES:
        try:
            return hp.relative_to((ROOT / d).resolve()).as_posix()
        except ValueError:
            pass
    return hp.as_posix()


def field_type_at(t, off):
    t = t.get_canonical()
    while True:
        if t.kind in (ci.TypeKind.CONSTANTARRAY, ci.TypeKind.INCOMPLETEARRAY):
            esz = t.element_type.get_canonical().get_size()
            off %= max(esz, 1)
            t = t.element_type.get_canonical()
        elif t.kind == ci.TypeKind.RECORD:
            best = None
            for f in t.get_fields():
                fo = f.get_field_offsetof() // 8
                if 0 <= fo <= off and (best is None or fo >= best[0]):
                    best = (fo, f)
            if best is None:
                return None
            off -= best[0]
            t = best[1].type.get_canonical()
        else:
            return t if off == 0 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir")
    ap.add_argument("files", nargs="+")
    ap.add_argument("--gen-dir", default=str(ROOT / "build-clang" / "generated"))
    args = ap.parse_args()
    types = Types(args.gen_dir)
    stats = convert([pathlib.Path(f).resolve() for f in args.files], pathlib.Path(args.out_dir), types)
    rep = pathlib.Path(args.out_dir) / "asm_data_report.txt"
    rep.write_text("\n".join(REPORT) + "\n", encoding="utf-8")
    print(dict(stats), f"- {len(REPORT)} notes in {rep}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
