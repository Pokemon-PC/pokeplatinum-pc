/*
 * platform_pc_prefix.h - force-included (-include) in every game / NitroSDK /
 * NitroSystem file built for the PC host.
 */
#pragma once

#if defined(__MINGW32__)
/* MinGW's _mingw.h defines __ptr32/__ptr64/__uptr/__sptr as empty macros
 * (for GCC). Pull it in first, then drop those macros so Clang's real
 * 32-bit pointer qualifiers are used (see PC_PTR32 below). _mingw.h is
 * include-guarded, so later system headers will not redefine them. */
#include <_mingw.h>
#undef __ptr32
#undef __ptr64
#undef __uptr
#undef __sptr
#endif

/*
 * PC_PTR32: pointer member stored on 32 bits, as on the DS.
 *
 * Structures overlaid on game data files (NitroSystem 2D resources: NCGR,
 * NCER, NANR...) contain pointers that are patched in place when a resource
 * is "unpacked". On a 64-bit host a plain pointer would grow to 8 bytes and
 * shift every following field. With Clang, `T *PC_PTR32 field` keeps the
 * field 4 bytes wide and converts transparently (zero-extension) when used;
 * this is valid because all game memory lives below 4 GiB on PC (hw_pc.h).
 * Limitation: such a field cannot appear in a static initializer.
 */
#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__))
#define PC_PTR32 __ptr32 __uptr
#else
#define PC_PTR32
#endif

/* Returns whether a NARC member really is LZ77-compressed (see
 * src/platform_pc/nitro/sdk_asm_pc.c); `declared` is what the code assumed. */
int pc_lz_autodetect(const void *data, int declared);

/* Applies VRAMCNT bank changes; called by lib/NitroSDK/src/gx/gx_vramcnt.c
 * after each write (src/platform_pc/nitro/vram_pc.c). */
void pc_vram_sync(void);

/* The game's V-Blank work is done: the frame can be drawn (src/main.c;
 * src/platform_pc/nitro/nitro_pc.c). */
void pc_nitro_frame_ready(void);

/* Diagnostics usable from game code (src/platform_pc). */
void pc_warn(const char *fmt, ...);
void pc_log_backtrace(const char *tag);

/* Game language: LANGUAGE_* of the player's ROM or of --lang
 * (GAME_LANGUAGE expands to it on PC; src/platform_pc/nitro/nitro_pc.c). */
extern unsigned char gPcGameLanguage;

/* Count leading zeros: the SDK otherwise uses an ARM `clz` inline-asm block
 * (nitro/math/math.h checks this macro first). */
#define PLATFORM_INTRINSIC_FUNCTION_BIT_CLZ32(x) ((x) ? (unsigned int)__builtin_clz(x) : 32u)

/* Overlay a source belongs to (-1: main program); the per-overlay headers of
 * tools/pc_port/gen_overlay_sections.py redefine it. Used by nitro/sinit.h. */
#ifndef PC_OVERLAY_INDEX
#define PC_OVERLAY_INDEX (-1)
#endif

/* MWCC built-in used by NitroSystem g2d (g2d_CharCanvas.c). */
#define __alloca(size) __builtin_alloca(size)

/* Delivers the V-Blank interrupt now (src/platform_pc/nitro/os_nitro_pc.c). */
void pc_nitro_vblank_irq_now(void);
