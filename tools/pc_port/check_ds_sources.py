#!/usr/bin/env python3
"""Check that the PC port leaves the DS sources unchanged (matching ROM).

Without the proprietary DS toolchain (mwccarm, Nitro SDK) the ROM cannot be
rebuilt here; this checks the next best thing: every C/asm source and header,
with the PLATFORM_PC-only code removed and comments/whitespace ignored, must
equal the original decompilation - apart from macros that expand to the
original tokens on the DS (SCRIPT_REL(x) -> x, PC_PTR32 -> nothing...).

Usage: python tools/pc_port/check_ds_sources.py ORIGINAL_DIR [REPO_DIR]
  ORIGINAL_DIR: an untouched copy of the decomp (e.g. the downloaded archive).
Prints the files that still differ; exit status 1 if any.
"""
import pathlib
import re
import sys

def strip_pc(text):
    out, stack = [], []   # stack of (kind, keep_this_branch, parent_keep)
    keep = True
    for line in text.split('\n'):
        s = line.strip()
        m = re.match(r'#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)', s)
        if m:
            d, rest = m.group(1), re.sub(r'/\*.*?\*/|//.*', '', m.group(2)).strip()
            if d in ('ifdef', 'ifndef', 'if'):
                pc = None
                if d == 'ifdef' and rest == 'PLATFORM_PC': pc = False
                elif d == 'ifndef' and rest == 'PLATFORM_PC': pc = True
                elif d == 'if' and re.fullmatch(r'!\s*defined\s*\(?\s*PLATFORM_PC\s*\)?', rest): pc = True
                elif d == 'if' and re.fullmatch(r'defined\s*\(?\s*PLATFORM_PC\s*\)?', rest): pc = False
                if pc is None:
                    stack.append(('other', keep)); 
                    if keep: out.append(line)
                else:
                    stack.append(('pc', keep, pc)); keep = keep and pc
                continue
            if d in ('else', 'elif'):
                top = stack[-1]
                if top[0] == 'pc':
                    if d == 'elif':
                        # #ifdef PLATFORM_PC ... #elif COND ...: becomes '#if COND' for the DS
                        keep = top[1] and not top[2]
                        stack[-1] = ('other', top[1] and not top[2])
                        if keep: out.append('#if ' + rest)
                        continue
                    keep = top[1] and not top[2]
                    stack[-1] = ('pc', top[1], not top[2])
                    continue
                if keep: out.append(line)
                continue
            if d == 'endif':
                top = stack.pop()
                if top[0] == 'pc':
                    keep = top[1]; continue
                if top[1]: out.append(line)
                continue
        if keep:
            out.append(line)
    return '\n'.join(out)

def norm(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = re.sub(r'//[^\n]*', ' ', text)
    return re.sub(r'\s+', ' ', text).strip()



MACRO_DEFS = re.compile(r'#\s*define\s+(SCRIPT_REL|PC_PTR32|DGT_ULONG|PC_LNK_VRAM_BLOCK_SIZE|ANMOBJ_HEADER_SIZE|TRAINER_HOUSE_DEFAULT_LANGUAGE|MAIN_MENU_UTIL_WORK_AFTER_CRYPTO_HEAP_ID|PCBOX_TAIL_STATE|PCBOX_TAIL_RETURN|PCBOX_TAIL_DONE|PC_HEAP_SIZE)\b[^\n]*')
def unwrap(text, name):
    out = ''; i = 0
    while True:
        j = text.find(name + '(', i)
        if j < 0: return out + text[i:]
        out += text[i:j]; k = j + len(name) + 1; depth = 1; start = k
        while depth: 
            depth += {'(':1, ')':-1}.get(text[k], 0); k += 1
        out += text[start:k-1]; i = k
def ds(text):
    text = MACRO_DEFS.sub('', text)
    text = re.sub(r'#\s*ifndef PC_PTR32\s*#\s*endif', '', text)
    for n in ('SCRIPT_REL', 'PC_HEAP_SIZE'): text = unwrap(text, n)
    rep = {'PC_PTR32': '', 'DGT_ULONG': 'unsigned long', 'PC_LNK_VRAM_BLOCK_SIZE': '16', 'ANMOBJ_HEADER_SIZE': '0x1C',
           'TRAINER_HOUSE_DEFAULT_LANGUAGE': 'GAME_LANGUAGE', 'MAIN_MENU_UTIL_WORK_AFTER_CRYPTO_HEAP_ID': '(MainMenuUtilWork *)(&gMainMenuUtilCryptoHeapId + 1)',
           'PCBOX_TAIL_STATE': 'void', 'PCBOX_TAIL_RETURN': '', 'PCBOX_TAIL_DONE': 'return', 'PartyMenuActionWord': 'u32', 'FOOTPRINT_SPRITE_STRIDE': '4', 'RECORD_MIXING_PTR_STRIDE': '4',
           'LANGUAGE_ENGLISH': '2', 'GAME_LANGUAGE': '2'}
    for k, v in rep.items(): text = re.sub(r'\b' + k + r'\b', v, text)
    text = re.sub(r'typedef u32 u32;', '', text)
    # definitions of the macros expanded above (their names are already replaced)
    text = re.sub(r'#\s*define (4|16|0x1C|unsigned long|void|return|2|u32)\b[^\n]*', '', text)
    text = re.sub(r'#\s*if defined\((\w+)\)', r'#ifdef \1', text)
    # implicit int spelled out
    text = re.sub(r'static const int (sSnow)', r'static const \1', text)
    text = re.sub(r'\bint (NNS_G2dInitCellTransferStateManager)', r'\1', text)
    text = re.sub(r'static int (PokedexApp_MainSeq_62\(PokedexAppData \*pokedexApp\) \{)', r'\1', text)
    text = re.sub(r'\(\s*(\w+)\s*\)', r'\1', text)  # (x) -> x for plain identifiers
    return re.sub(r'\s*\*\s*', '*', norm(text))   # 'void* p' == 'void *p'


def main():
    orig = pathlib.Path(sys.argv[1])
    cur = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else ".")
    bad = []
    for root in ("src", "include", "lib", "asm", "sub"):
        for p in (cur / root).rglob("*"):
            if not p.is_file() or p.suffix not in (".c", ".h", ".s", ".inc", ".cpp"):
                continue
            rel = p.relative_to(cur).as_posix()
            if rel.startswith("src/platform_pc"):
                continue
            o = orig / rel
            if not o.exists():
                bad.append(("new file", rel))
                continue
            a = p.read_text(encoding="utf-8", errors="replace")
            b = o.read_text(encoding="utf-8", errors="replace")
            if a == b:
                continue
            if ds(strip_pc(a)) != ds(b):
                bad.append(("differs", rel))
    for kind, rel in bad:
        print(kind, rel)
    print(f"{len(bad)} files differ from the original on the DS", file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
