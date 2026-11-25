#!/usr/bin/env python3
"""Writes src/platform_pc/sdk_override: copies of the few NitroSDK /
NitroSystem headers (subprojects/) that the PC build needs changed.

The DS build uses the subprojects unchanged. On PC, src/platform_pc/sdk_override
comes first in the include path, so these copies replace the originals. Each
change keeps the original line under #else, so a diff against the subproject
shows exactly what the PC port does and why.

    python tools/pc_port/make_sdk_override.py

Run it again after updating a subproject revision (subprojects/*.wrap).
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SDK = os.path.join(ROOT, "subprojects", "NitroSDK-4.2.30001", "include")
NNS = os.path.join(ROOT, "subprojects", "NitroSystem-071126.1", "include")
OUT = os.path.join(ROOT, "src", "platform_pc", "sdk_override")


def write_if_changed(path, text, newline="\n"):
    """Rewrites a file only when its contents change (keeps the build incremental)."""
    try:
        with open(path, encoding="utf-8", newline="") as f:
            if f.read() == (text if newline == "" else text.replace("\n", newline)):
                return
    except OSError:
        pass
    with open(path, "w", encoding="utf-8", newline=newline) as f:
        f.write(text)


def pc(new, old, why):
    """Replacement text: new code for PC, the original kept for the DS."""
    return "#ifdef PLATFORM_PC\n// PC port: %s\n%s\n#else\n%s\n#endif" % (why, new, old)


# (base, relative path, [(original text, replacement)])
PATCHES = [
    (SDK, "nitro/types.h", [
        ("typedef unsigned long u32;",
         pc("typedef unsigned int u32;", "typedef unsigned long u32;",
            "`long` is 64-bit on Linux/macOS (LP64); keep the DS 32-bit width everywhere.")),
        ("typedef signed long s32;",
         pc("typedef signed int s32;", "typedef signed long s32;", "same as u32.")),
        ("#if defined(SDK_CW) || defined(__MWERKS__)\n    #ifndef ATTRIBUTE_ALIGN",
         "#if defined(SDK_CW) || defined(__MWERKS__) || defined(PLATFORM_PC)\n    #ifndef ATTRIBUTE_ALIGN"),
        ("#if defined(SDK_CW) || defined(__MWERKS__)\n    #define SDK_WEAK_SYMBOL __declspec(weak)",
         "#if defined(PLATFORM_PC)\n    #define SDK_WEAK_SYMBOL __attribute__((weak))\n"
         "#elif defined(SDK_CW) || defined(__MWERKS__)\n    #define SDK_WEAK_SYMBOL __declspec(weak)"),
    ]),
    (SDK, "nitro/platform.h", [
        ("#if defined(SDK_CW) || defined(__MWERKS__)",
         "#if defined(PLATFORM_PC)\n"
         "    /* PC port: as Metrowerks, but the DS structure-size checks (wireless\n"
         "     * file system formats with pointers) do not hold in 64-bit. */\n"
         "    #define PLATFORM_BITFIELDS_ENDIAN    PLATFORM_ENDIAN_LITTLE\n"
         "    #define PLATFORM_COMPILER_ASSERT(expr) struct platform_compiler_assert_unused\n"
         "    #define PLATFORM_STRUCT_PADDING_FOOTER\n"
         "    #define PLATFORM_ATTRIBUTE_INLINE       SDK_INLINE\n"
         "#elif defined(SDK_CW) || defined(__MWERKS__)"),
    ]),
    # Private header of libraries/gx (found through the override folder, which
    # comes first in the include path).
    (os.path.join(ROOT, "subprojects", "NitroSDK-4.2.30001", "libraries", "gx", "include"), "gxasm.h", [
        ("void GX_SendFifo36B(register const void * pSrc, register void * pDest);\n",
         pc("", "void GX_SendFifo36B(register const void * pSrc, register void * pDest);",
            "only the static inline version below (an extern declaration before it\n"
            "// makes Clang emit it in every file).") + "\n"),
    ]),
    (SDK, "nitro/sinit.h", [
        ("#if defined(SDK_CW) || defined(__MWERKS__)",
         "#if defined(PLATFORM_PC)\n"
         "    /* PC port: the DS overlay loader runs the .sinit entries of an overlay\n"
         "     * when it loads it. Here each NitroStaticInit registers itself at\n"
         "     * start-up with the overlay its file belongs to (PC_OVERLAY_INDEX, from\n"
         "     * the per-overlay header of tools/pc_port/gen_overlay_sections.py) and\n"
         "     * FS_LoadOverlay calls it (src/platform_pc/nitro/fs_nitro_pc.c). */\n"
         "    void pc_register_static_init(int overlay, void (*fn)(void));\n"
         "    static void NitroStaticInit(void);\n"
         "    __attribute__((constructor)) static void pc_sinit_register_(void)\n"
         "    {\n"
         "        pc_register_static_init(PC_OVERLAY_INDEX, NitroStaticInit);\n"
         "    }\n"
         "#elif defined(SDK_CW) || defined(__MWERKS__)"),
    ]),
    (SDK, "nitro/hw/ARM9/mmap_global.h", [
        ("extern u32 SDK_AUTOLOAD_DTCM_START[];\n#define HW_DTCM                 ((u32)SDK_AUTOLOAD_DTCM_START)",
         pc("#define HW_DTCM                 0x027E0000",
            "extern u32 SDK_AUTOLOAD_DTCM_START[];\n#define HW_DTCM                 ((u32)SDK_AUTOLOAD_DTCM_START)",
            "no linker-placed DTCM; it lives at its usual DS address inside the\n"
            "// emulated memory map reserved by src/platform_pc/hw_pc.c.")),
    ]),
    (SDK, "nitro/fs/overlay.h", [
        ("#define FS_EXTERN_OVERLAY(name) extern u32 SDK_OVERLAY_ ## name ## _ID[1]\n"
         "#define FS_OVERLAY_ID(name) ((u32) & (SDK_OVERLAY_ ## name ## _ID))",
         pc("#include <pc_overlay_ids.h>\n"
            "#define FS_EXTERN_OVERLAY(name) extern const int pc_overlay_declared_ ## name\n"
            "#define FS_OVERLAY_ID(name) ((u32)PC_OVERLAY_ID_ ## name)",
            "#define FS_EXTERN_OVERLAY(name) extern u32 SDK_OVERLAY_ ## name ## _ID[1]\n"
            "#define FS_OVERLAY_ID(name) ((u32) & (SDK_OVERLAY_ ## name ## _ID))",
            "overlay IDs are real constants (generated from platinum.us/main.lsf.in\n"
            "// in the same order as the DS overlay table), usable in static initializers.")),
    ]),
]


DWC = os.path.join(ROOT, "subprojects", "NitroDWC-2.2.30008", "include")
WIN32_HEADERS = [
    (DWC, "nd/DWCi_Nd.h"),
    (DWC, "nhttp/NHTTP.h"),
    (DWC, "gs/nonport.h"),
    (DWC, "gs/darray.h"),
    (DWC, "gs/hashtable.h"),
    (DWC, "gs/gsiDebug.h"),
    (DWC, "gs/gstats/gstats.h"),
    (NNS, "nnsys/mcs.h"),
    (NNS, "nnsys/mcs/ringBuffer.h"),
]


def ioreg_cp_patches():
    """Divider / square-root results: computed when read (hw_pc.c)."""
    path = os.path.join(SDK, "nitro/hw/ARM9/ioreg_CP.h")
    text = open(path, encoding="utf-8").read()
    out = []
    for line in text.splitlines():
        for reg, fn in (("DIVCNT", "div"), ("DIV_RESULT", "div"), ("DIV_RESULT_L", "div"), ("DIV_RESULT_H", "div"),
                        ("DIVREM_RESULT", "div"), ("DIVREM_RESULT_L", "div"), ("DIVREM_RESULT_H", "div"),
                        ("SQRTCNT", "sqrt"), ("SQRT_RESULT", "sqrt")):
            prefix = "#define reg_CP_%s " % reg
            if line.startswith(prefix):
                addr = "REG_%s_ADDR" % reg
                new = line.replace(" %s)" % addr, " (uintptr_t)pc_cp_%s_update(%s))" % (fn, addr))
                if new != line:
                    out.append((line, pc(new, line, "the result is computed when the register is read (hw_pc.c).")))
    header = ("#include <nitro/hw/ARM9/ioreg_CP.h>", None)
    return out


def main():
    patches = list(PATCHES)
    cp = ioreg_cp_patches()
    decl = ("#ifdef PLATFORM_PC\n// PC port: the divider / square-root units compute when their result\n"
            "// registers are read (src/platform_pc/hw_pc.c); inputs are plain memory.\n#include <stdint.h>\n"
            "unsigned int pc_cp_div_update(unsigned int reg_address);\n"
            "unsigned int pc_cp_sqrt_update(unsigned int reg_address);\n#endif\n")
    patches.append((SDK, "nitro/hw/ARM9/ioreg_CP.h", cp + [("#include <nitro/hw/ARM9/mmap_global.h>\n",
                                                                "#include <nitro/hw/ARM9/mmap_global.h>\n" + decl)]))
    # Headers that test _WIN32/WIN32 to mean "built for a Windows tool": the
    # PC port is not that, even when it runs on Windows.
    for base, rel in WIN32_HEADERS:
        patches.append((base, rel, "win32"))
    # NitroSystem structures laid over file data (NCGR, NCER, NANR, NMCR,
    # NFTR... and the sound archive file table): their pointer fields are
    # offsets "unpacked" in place into 32-bit pointers. They stay 4 bytes
    # wide on PC (PC_PTR32, platform_pc_prefix.h) so the layout matches the file.
    for rel in sorted(os.listdir(os.path.join(NNS, "nnsys", "g2d", "fmt"))):
        patches.append((NNS, "nnsys/g2d/fmt/" + rel, "ptr32"))
    patches.append((NNS, "nnsys/snd/sndarc.h", "ptr32"))
    # Geometry command registers: on the DS a write feeds the geometry engine;
    # on PC every access goes through pc_g3_port() (nitro/gpu3d_pc.c), which
    # hands the previous access to the 3D engine.
    patches.append((SDK, "nitro/hw/ARM9/ioreg_G3.h", "g3port"))
    patches.append((SDK, "nitro/hw/ARM9/ioreg_G3X.h", "g3port"))
    n = 0
    for base, rel, edits in patches:
        src = os.path.join(base, rel)
        text = open(src, encoding="utf-8", newline="").read()
        crlf = "\r\n" in text
        text = text.replace("\r\n", "\n")
        if edits == "win32":
            text = re.sub(r"defined\s*\(\s*(_?WIN32)\s*\)|defined\s+(_?WIN32)\b",
                          lambda m: "(defined(%s) && !defined(PLATFORM_PC))" % (m.group(1) or m.group(2)), text)
            text = re.sub(r"^(\s*#\s*)ifdef\s+(_?WIN32)\b", r"\1if defined(\2) && !defined(PLATFORM_PC)", text,
                          flags=re.M)
            text = re.sub(r"^(\s*#\s*)ifndef\s+(_?WIN32)\b", r"\1if !defined(\2) || defined(PLATFORM_PC)", text,
                          flags=re.M)
            edits = []
        elif edits == "g3port":
            pat = re.compile(r"^(#define (reg_G3_\w+|reg_G3X_GXFIFO)\s+)\(\*\( REGType32v \*\) (REG_\w+_ADDR)\)[ \t]*$",
                             re.M)
            text, count = pat.subn(lambda m: pc("%s(*pc_g3_port(%s))" % (m.group(1), m.group(3)), m.group(0),
                                                "geometry command port (nitro/gpu3d_pc.c)."), text)
            if count == 0:
                print("%s: no geometry register found" % rel, file=sys.stderr)
                return 1
            text = text.replace("#include <nitro/hw/ARM9/mmap_global.h>\n",
                                "#include <nitro/hw/ARM9/mmap_global.h>\n#ifdef PLATFORM_PC\n"
                                "REGType32v *pc_g3_port(u32 addr);\n#endif\n", 1)
            edits = []
        elif edits == "ptr32":
            text, count = re.subn(r"^(\s*(?:const\s+)?\w+(?:\s+const)?\s*)\*(\s*)(\w+\s*;)",
                                  r"\1*PC_PTR32 \3", text, flags=re.M)
            if count == 0:
                continue
            text = text.replace("\n", "\n#ifndef PC_PTR32\n#define PC_PTR32   /* PC port: 32-bit pointer field (platform_pc_prefix.h) */\n#endif\n", 1)
            edits = []
        for old, new in edits:
            if text.count(old) != 1:
                print("%s: patch target found %d times:\n%s" % (rel, text.count(old), old), file=sys.stderr)
                return 1
            text = text.replace(old, new)
        if crlf:
            text = text.replace("\n", "\r\n")
        dst = os.path.join(OUT, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        write_if_changed(dst, text, "")
        n += 1
    print("%d SDK headers written to %s" % (n, os.path.relpath(OUT, ROOT)))
    return strip_asm_sources()


# SDK sources mixing C and Metrowerks `asm` functions (ARM assembly). The PC
# build compiles copies without the asm functions, whose C versions are in
# src/platform_pc/nitro/sdk_asm_pc.c.
SDK_LIB = os.path.join(ROOT, "subprojects", "NitroSDK-4.2.30001", "libraries")
NNS_LIB = os.path.join(ROOT, "subprojects", "NitroSystem-071126.1", "libraries")
ASM_SOURCES = [
    (SDK_LIB, "fx/src/fx_mtx22.c"),
    (SDK_LIB, "fx/src/fx_mtx33.c"),
    (SDK_LIB, "fx/src/fx_mtx43.c"),
    (SDK_LIB, "fx/src/fx_mtx44.c"),
    (SDK_LIB, "mi/src/mi_uncomp_stream.c"),
    (SDK_LIB, "gx/src/g3x.c"),
]
SRC_OUT = os.path.join(ROOT, "src", "platform_pc", "sdk_src")


def lib_name(base):
    return "NitroSDK" if base == SDK_LIB else "NitroSystem"
# Plain C sources with a construct Clang refuses (casts used as lvalues).
SOURCE_PATCHES = [
    (SDK_LIB, "math/src/checksum.c", [
        # *((T *)p)++  ->  *PC_POSTINC(p, T)
        (re.compile(r"\*\(\((u8|u16) \*\)(\w+)\)\+\+"), r"*PC_POSTINC(\2, \1)"),
    ]),
    # VRAM bank mapping: every VRAMCNT write is followed by pc_vram_sync()
    # (nitro/vram_pc.c moves the bank contents to their new CPU address).
    (SDK_LIB, "gx/src/gx_vramcnt.c", [
        (re.compile(r"\b(reg_GX_VRAMCNT_[A-I] = [^;]+|GX_VRAMCNT_SetLCDC_\([^;]*\));"), r"(\1, pc_vram_sync());"),
    ]),
    (SDK_LIB, "gx/src/gxstate.c", [
        ("reg_GX_VRAMCNT = 0;", "reg_GX_VRAMCNT = 0; pc_vram_sync();"),
    ]),
    # Reverb capture passes interval 0: the ARM division routine returns 0,
    # an x86 division by zero traps.
    (NNS_LIB, "snd/src/capture.c", [
        ("cap->blockSize = bufLen / interval;", "cap->blockSize = interval != 0 ? bufLen / interval : 0;"),
    ]),
    (NNS_LIB, "g2d/src/internal/g2di_SplitChar.c", [
        ("((const wchar_t *)*ppChar)++;", "*ppChar = (const u8 *)*ppChar + sizeof(wchar_t);"),
    ]),
    (NNS_LIB, "snd/src/sndarc_stream.c", [
        ("*state = *((AdpcmState *)srcp)++;", "*state = *(const AdpcmState *)srcp; srcp = (const AdpcmState *)srcp + 1;"),
    ]),
]


def strip_asm_sources():
    n = 0
    for base, rel in ASM_SOURCES:
        text = open(os.path.join(base, rel), encoding="utf-8", newline="").read().replace("\r\n", "\n")
        out, pos, removed = [], 0, []
        # prototypes of static asm functions become ordinary declarations
        text = re.sub(r"^static asm (.*\));[ \t]*$", lambda m: m.group(1).replace("register ", "") + ";", text,
                      flags=re.M)
        for m in re.finditer(r"^[ \t]*(?:static\s+)?asm\s+[^\n;]*?(\w+)\s*\([^)]*\)\s*\{", text, re.M):
            if m.start() < pos:
                continue
            depth, i = 0, text.index("{", m.start())
            while True:
                if text[i] == "{":
                    depth += 1
                elif text[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            out.append(text[pos:m.start()])
            out.append("/* PC port: %s (ARM assembly) -> src/platform_pc/nitro/sdk_asm_pc.c */" % m.group(1))
            removed.append(m.group(1))
            pos = i + 1
        out.append(text[pos:])
        text = "".join(out)
        header = ("/* Generated by tools/pc_port/make_sdk_override.py from subprojects/%s/%s:\n"
                  " * the ARM assembly functions (%s) are removed. */\n"
                  % (os.path.basename(os.path.dirname(base)), rel, ", ".join(removed)))
        dst = os.path.join(SRC_OUT, lib_name(base), rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        write_if_changed(dst, header + text)
        n += 1
    for base, rel, edits in SOURCE_PATCHES:
        text = open(os.path.join(base, rel), encoding="utf-8", newline="").read().replace("\r\n", "\n")
        for old, new in edits:
            if isinstance(old, re.Pattern):
                text, count = old.subn(new, text)
                if count == 0:
                    print("%s: no match for %s" % (rel, old.pattern), file=sys.stderr)
                    return 1
                text = ("/* PC port: casts used as lvalues rewritten with PC_POSTINC. */\n"
                        "#define PC_POSTINC(p, T) ((p) = (const T *)(p) + 1, (const T *)(p) - 1)\n") + text
                continue
            if text.count(old) != 1:
                print("%s: patch target found %d times: %s" % (rel, text.count(old), old), file=sys.stderr)
                return 1
            text = text.replace(old, "/* PC port: was `%s` (PC: %s) */ %s" % (old.replace("*/", ""), "see make_sdk_override.py", new))
        dst = os.path.join(SRC_OUT, lib_name(base), rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        write_if_changed(dst, "/* Generated by tools/pc_port/make_sdk_override.py from subprojects/%s/%s. */\n"
                         % (os.path.basename(os.path.dirname(base)), rel) + text)
        n += 1
    print("%d SDK sources written to %s" % (n, os.path.relpath(SRC_OUT, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
