/*
 * sdk_asm_pc.c - portable C versions of the NitroSDK routines that the
 * SDK keeps as hand-written ARM assembly (Metrowerks `asm` functions). Each one was translated
 * from the original instruction sequence (see the file named above it), so
 * results - including element order for MATH_QSort - match the DS.
 */
#include "nitro_pc.h"
#include "gpu3d_pc.h"

#include <nitro/fx/fx.h>
#include <nitro/math/qsort.h>
#include <string.h>

/* ------------------------- NitroSDK libraries/fx/src/fx_mtx{22,33,43,44}.c */
/*
 * The Metrowerks `asm` functions of these files, in C. The rest of each file
 * is compiled from the copy in src/platform_pc/sdk_src (made by
 * tools/pc_port/make_sdk_override.py). Matrices are row-major arrays
 * (m->a[row * columns + column]), as the ldmia/stmia sequences store them.
 * Sources and destinations may be the same matrix: inputs are read first.
 */

#define ONE FX32_ONE

/* 2x2 */
void MTX_Identity22_(MtxFx22 *m) {
    m->a[0] = ONE; m->a[1] = 0;
    m->a[2] = 0;   m->a[3] = ONE;
}

void MTX_Transpose22_(const MtxFx22 *src, MtxFx22 *dst) {
    fx32 b = src->a[1], c = src->a[2];
    dst->a[0] = src->a[0]; dst->a[1] = c;
    dst->a[2] = b;         dst->a[3] = src->a[3];
}

void MTX_Scale22_(MtxFx22 *m, fx32 x, fx32 y) {
    m->a[0] = x; m->a[1] = 0;
    m->a[2] = 0; m->a[3] = y;
}

void MTX_Rot22_(MtxFx22 *m, fx32 s, fx32 c) {
    m->a[0] = c;  m->a[1] = s;
    m->a[2] = -s; m->a[3] = c;
}

/* 3x3 */
void MTX_Identity33_(MtxFx33 *m) {
    static const fx32 id[9] = { ONE, 0, 0, 0, ONE, 0, 0, 0, ONE };
    memcpy(m->a, id, sizeof id);
}

void MTX_Copy33To43_(const MtxFx33 *src, MtxFx43 *dst) {
    fx32 t[9];
    memcpy(t, src->a, sizeof t);
    memcpy(dst->a, t, sizeof t);
    dst->a[9] = dst->a[10] = dst->a[11] = 0;
}

void MTX_Copy33To44_(const MtxFx33 *src, MtxFx44 *dst) {
    fx32 t[9];
    memcpy(t, src->a, sizeof t);
    for (int r = 0; r < 3; r++) {
        dst->a[r * 4 + 0] = t[r * 3 + 0];
        dst->a[r * 4 + 1] = t[r * 3 + 1];
        dst->a[r * 4 + 2] = t[r * 3 + 2];
        dst->a[r * 4 + 3] = 0;
    }
    dst->a[12] = dst->a[13] = dst->a[14] = 0;
    dst->a[15] = ONE;
}

void MTX_Transpose33_(const MtxFx33 *src, MtxFx33 *dst) {
    fx32 t[9];
    memcpy(t, src->a, sizeof t);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            dst->a[r * 3 + c] = t[c * 3 + r];
        }
    }
}

void MTX_Scale33_(MtxFx33 *m, fx32 x, fx32 y, fx32 z) {
    memset(m->a, 0, sizeof m->a);
    m->a[0] = x; m->a[4] = y; m->a[8] = z;
}

/* [1 0 0; 0 cos sin; 0 -sin cos] */
void MTX_RotX33_(MtxFx33 *m, fx32 s, fx32 c) {
    m->a[0] = ONE; m->a[1] = 0;  m->a[2] = 0;
    m->a[3] = 0;   m->a[4] = c;  m->a[5] = s;
    m->a[6] = 0;   m->a[7] = -s; m->a[8] = c;
}

/* [cos 0 -sin; 0 1 0; sin 0 cos] */
void MTX_RotY33_(MtxFx33 *m, fx32 s, fx32 c) {
    m->a[0] = c; m->a[1] = 0;   m->a[2] = -s;
    m->a[3] = 0; m->a[4] = ONE; m->a[5] = 0;
    m->a[6] = s; m->a[7] = 0;   m->a[8] = c;
}

/* [cos sin 0; -sin cos 0; 0 0 1] */
void MTX_RotZ33_(MtxFx33 *m, fx32 s, fx32 c) {
    m->a[0] = c;  m->a[1] = s; m->a[2] = 0;
    m->a[3] = -s; m->a[4] = c; m->a[5] = 0;
    m->a[6] = 0;  m->a[7] = 0; m->a[8] = ONE;
}

/* 4x3 */
void MTX_Identity43_(MtxFx43 *m) {
    static const fx32 id[12] = { ONE, 0, 0, 0, ONE, 0, 0, 0, ONE, 0, 0, 0 };
    memcpy(m->a, id, sizeof id);
}

/* rows of the 4x3 become the first three columns; 4th column = 0,0,0,1 */
void MTX_Copy43To44_(const MtxFx43 *src, MtxFx44 *dst) {
    fx32 t[12];
    memcpy(t, src->a, sizeof t);
    for (int r = 0; r < 4; r++) {
        dst->a[r * 4 + 0] = t[r * 3 + 0];
        dst->a[r * 4 + 1] = t[r * 3 + 1];
        dst->a[r * 4 + 2] = t[r * 3 + 2];
        dst->a[r * 4 + 3] = r == 3 ? ONE : 0;
    }
}

/* the 3x3 part transposed, translation cleared */
void MTX_Transpose43_(const MtxFx43 *src, MtxFx43 *dst) {
    fx32 t[9];
    memcpy(t, src->a, sizeof t);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            dst->a[r * 3 + c] = t[c * 3 + r];
        }
    }
    dst->a[9] = dst->a[10] = dst->a[11] = 0;
}

void MTX_Scale43_(MtxFx43 *m, fx32 x, fx32 y, fx32 z) {
    memset(m->a, 0, sizeof m->a);
    m->a[0] = x; m->a[4] = y; m->a[8] = z;
}

/* [1 0 0; 0 cos sin; 0 -sin cos; 0 0 0] */
void MTX_RotX43_(MtxFx43 *m, fx32 s, fx32 c) {
    MTX_RotX33_((MtxFx33 *)m, s, c);
    m->a[9] = m->a[10] = m->a[11] = 0;
}

/* [cos 0 -sin; 0 1 0; sin 0 cos; 0 0 0] */
void MTX_RotY43_(MtxFx43 *m, fx32 s, fx32 c) {
    MTX_RotY33_((MtxFx33 *)m, s, c);
    m->a[9] = m->a[10] = m->a[11] = 0;
}

/* [cos sin 0; -sin cos 0; 0 0 1; 0 0 0] */
void MTX_RotZ43_(MtxFx43 *m, fx32 s, fx32 c) {
    MTX_RotZ33_((MtxFx33 *)m, s, c);
    m->a[9] = m->a[10] = m->a[11] = 0;
}

/* 4x4 */
void MTX_Identity44_(MtxFx44 *m) {
    memset(m->a, 0, sizeof m->a);
    m->a[0] = m->a[5] = m->a[10] = m->a[15] = ONE;
}

void MTX_Copy44To33_(const MtxFx44 *src, MtxFx33 *dst) {
    fx32 t[16];
    memcpy(t, src->a, sizeof t);
    for (int r = 0; r < 3; r++) {
        dst->a[r * 3 + 0] = t[r * 4 + 0];
        dst->a[r * 3 + 1] = t[r * 4 + 1];
        dst->a[r * 3 + 2] = t[r * 4 + 2];
    }
}

void MTX_Copy44To43_(const MtxFx44 *src, MtxFx43 *dst) {
    fx32 t[16];
    memcpy(t, src->a, sizeof t);
    for (int r = 0; r < 4; r++) {
        dst->a[r * 3 + 0] = t[r * 4 + 0];
        dst->a[r * 3 + 1] = t[r * 4 + 1];
        dst->a[r * 3 + 2] = t[r * 4 + 2];
    }
}

void MTX_Transpose44_(const MtxFx44 *src, MtxFx44 *dst) {
    fx32 t[16];
    memcpy(t, src->a, sizeof t);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            dst->a[r * 4 + c] = t[c * 4 + r];
        }
    }
}

void MTX_Scale44_(MtxFx44 *m, fx32 x, fx32 y, fx32 z) {
    memset(m->a, 0, sizeof m->a);
    m->a[0] = x; m->a[5] = y; m->a[10] = z; m->a[15] = ONE;
}

/* [1 0 0 0; 0 cos sin 0; 0 -sin cos 0; 0 0 0 1] */
void MTX_RotX44_(MtxFx44 *m, fx32 s, fx32 c) {
    MTX_Identity44_(m);
    m->a[5] = c;  m->a[6] = s;
    m->a[9] = -s; m->a[10] = c;
}

/* [cos 0 -sin 0; 0 1 0 0; sin 0 cos 0; 0 0 0 1] */
void MTX_RotY44_(MtxFx44 *m, fx32 s, fx32 c) {
    MTX_Identity44_(m);
    m->a[0] = c; m->a[2] = -s;
    m->a[8] = s; m->a[10] = c;
}

/* [cos sin 0 0; -sin cos 0 0; 0 0 1 0; 0 0 0 1] */
void MTX_RotZ44_(MtxFx44 *m, fx32 s, fx32 c) {
    MTX_Identity44_(m);
    m->a[0] = c;  m->a[1] = s;
    m->a[4] = -s; m->a[5] = c;
}

/* --------------------------------------- lib/NitroSDK/src/gx/gxasm.c etc. */

/* Words are written one after another to the same (FIFO) register. */
void GX_SendFifo48B(const void *pSrc, void *pDest) {
    const u32 *s = pSrc;
    volatile u32 *d = pDest;
    if (pc_g3_is_port(pDest)) {
        pc_g3_cancel(pDest);
        for (int i = 0; i < 12; i++) {
            pc_g3_write((u32)(uintptr_t)pDest, s[i]);
        }
        return;
    }
    for (int i = 0; i < 12; i++) {
        *d = s[i];
    }
}

void GX_SendFifo64B(const void *pSrc, void *pDest) {
    const u32 *s = pSrc;
    volatile u32 *d = pDest;
    if (pc_g3_is_port(pDest)) {
        pc_g3_cancel(pDest);
        for (int i = 0; i < 16; i++) {
            pc_g3_write((u32)(uintptr_t)pDest, s[i]);
        }
        return;
    }
    for (int i = 0; i < 16; i++) {
        *d = s[i];
    }
}

void GXi_NopClearFifo128_(void *reg) {
    volatile u32 *d = reg;
    if (pc_g3_is_port(reg)) {
        pc_g3_cancel(reg);
        for (int i = 0; i < 128; i++) {
            pc_g3_write((u32)(uintptr_t)reg, 0);
        }
        return;
    }
    for (int i = 0; i < 128; i++) {
        *d = 0;
    }
}

/* ------------------------------------------ lib/NitroSDK/src/mi/mi_swap.c */

u32 MI_SwapWord(u32 setData, vu32 *destp) {
    u32 old = *destp;   /* single-threaded game code: no atomicity needed */
    *destp = setData;
    return old;
}

/* ------------------------------------ lib/NitroSDK/src/mi/mi_uncompress.c */

/*
 * LZ77 with the "LZ11" extension, selected by a non-zero low nibble in the
 * header (type 0x11): match lengths may use 1, 2 or 3 bytes.
 */
void MI_UncompressLZ8(const void *srcp, void *destp) {
    const u8 *src = srcp;
    u8 *dst = destp;
    u32 header = (u32)src[0] | ((u32)src[1] << 8) | ((u32)src[2] << 16) | ((u32)src[3] << 24);
    s32 remaining = (s32)(header >> 8);
    int extended = (header & 0xF) != 0;
    src += 4;

    while (remaining > 0) {
        u8 flags = *src++;
        for (int bit = 0; bit < 8 && remaining > 0; bit++, flags <<= 1) {
            if (!(flags & 0x80)) {
                *dst++ = *src++;
                remaining--;
                continue;
            }
            u32 b = src[0];
            u32 length;
            if (!extended) {
                length = 3 + (b >> 4);
            } else if (b & 0xE0) {
                length = 1 + (b >> 4);                        /* 3..16 */
            } else {
                src++;
                u32 base = (b & 0xF) << 4;
                if (b & 0x10) {                               /* 3-byte form */
                    base = (base << 8) + ((u32)*src++ << 4) + 0x100;
                }
                base += 0x11;
                b = src[0];
                length = base + (b >> 4);
            }
            src++;
            u32 disp = (((b & 0xF) << 8) | *src++) + 1;
            remaining -= (s32)length;
            for (u32 i = 0; i < length; i++, dst++) {
                *dst = *(dst - disp);
            }
        }
    }
}

/* ----------------------------------------- lib/NitroSDK/src/math/math_qsort.c */

static void swap_bytes(u8 *a, u8 *b, u32 width) {
    for (u32 i = 0; i < width; i++) {
        u8 t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

/* Same algorithm as the DS: explicit stack, pivot at
 * lo + ((hi - lo) >> bitlen(width)) * width moved to lo, Hoare partition,
 * larger half pushed first, 2-element ranges handled directly. The `work`
 * buffer of the DS version only holds that stack; a local one is used. */
void MATH_QSort(void *base, u32 count, u32 width, MATHCompareFunc compare, void *work) {
    (void)work;
    if (count <= 1) {
        return;
    }
    u32 shift = 32 - (u32)__builtin_clz(width);
    u8 *stack[2 * 64];
    int top = 0;
    stack[top++] = base;
    stack[top++] = (u8 *)base + (count - 1) * width;

    while (top > 0) {
        u8 *hi = stack[--top];
        u8 *lo = stack[--top];
        if ((u32)(hi - lo) == width) {
            if (compare(lo, hi) > 0) {
                swap_bytes(lo, hi, width);
            }
            continue;
        }
        u8 *mid = lo + (((u32)(hi - lo) >> shift) * width);
        swap_bytes(mid, lo, width);

        u8 *i = lo + width;
        u8 *j = hi;
        for (;;) {
            while (i < hi && compare(i, lo) < 0) {
                i += width;
            }
            while (compare(j, lo) > 0) {
                j -= width;
            }
            if (i >= j) {
                break;
            }
            swap_bytes(i, j, width);
            i += width;
            j -= width;
            if (i > j) {
                break;
            }
        }
        swap_bytes(lo, j, width);

        if ((j - lo) > (hi - j)) {
            if (lo < j - width) { stack[top++] = lo; stack[top++] = j - width; }
            if (j + width < hi) { stack[top++] = j + width; stack[top++] = hi; }
        } else {
            if (j + width < hi) { stack[top++] = j + width; stack[top++] = hi; }
            if (lo < j - width) { stack[top++] = lo; stack[top++] = j - width; }
        }
    }
}

/* --------------------------------- lib/NitroSDK/src/dgt_hash2_arm4_small.c */

/* SHA-1 compression over `length` bytes (a multiple of 64). The context
 * starts with the five 32-bit chaining values (struct DGTHash2Context). */
static u32 rol(u32 x, int n) {
    return (x << n) | (x >> (32 - n));
}

void DGTi_hash2_arm4_small(void *context, const void *input, u32 length) {
    u32 *h = context;
    const u8 *p = input;
    for (u32 off = 0; off + 64 <= length; off += 64, p += 64) {
        u32 w[80];
        for (int t = 0; t < 16; t++) {
            w[t] = ((u32)p[4 * t] << 24) | ((u32)p[4 * t + 1] << 16) | ((u32)p[4 * t + 2] << 8) | p[4 * t + 3];
        }
        for (int t = 16; t < 80; t++) {
            w[t] = rol(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int t = 0; t < 80; t++) {
            u32 f, k;
            if (t < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (t < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            u32 tmp = rol(a, 5) + f + e + k + w[t];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
}

/* ------------------------------------------------ LZ77 auto-detection */

/* Nitro resource files start with a 4-character tag (RGCN, RLCN, RCSN,
 * RECN, RNAN, BMD0, BTX0, NARC, SDAT...): letters/digits only. An LZ77
 * stream starts with 0x10 (or 0x11 for LZ11) instead. */
static int looks_like_nitro_tag(const u8 *p) {
    for (int i = 0; i < 4; i++) {
        u8 c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return 0;
        }
    }
    return 1;
}

int pc_lz_autodetect(const void *data, int declared) {
    const u8 *p = data;
    int actual = declared;
    if (p == NULL) {
        return declared;
    }
    if (looks_like_nitro_tag(p)) {
        actual = 0;
    } else if ((p[0] == 0x10 || p[0] == 0x11) && (p[1] | p[2] | p[3]) != 0) {
        actual = 1;
    }
    if (actual != declared) {
        static int reported;
        if (reported++ < 16) {
            pc_log("data: member is %scompressed in this ROM (code expected the opposite); adapted",
                   actual ? "" : "not ");
        }
    }
    return actual;
}
