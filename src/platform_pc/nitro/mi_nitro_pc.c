/*
 * mi_nitro_pc.c - portable C replacement for lib/NitroSDK/src/mi/mi_memory.c
 * (ARM assembly). Semantics follow the SDK: sizes are in bytes, 16/32-bit
 * variants copy whole units, Fast variants are 32-bit block copies.
 * Sources and destinations may overlap only where the SDK allowed it
 * (memmove covers that).
 */
#include "nitro_pc.h"
#include "gpu3d_pc.h"

#include <string.h>

void MIi_CpuClear16(u16 data, void *dest, u32 size) {
    u16 *destp = dest;
    for (u32 i = 0; i < size / 2; i++) {
        destp[i] = data;
    }
}

void MIi_CpuCopy16(const void *srcp, void *destp, u32 size) {
    memmove(destp, srcp, size & ~1u);
}

void MIi_CpuClear32(u32 data, void *dest, u32 size) {
    u32 *destp = dest;
    for (u32 i = 0; i < size / 4; i++) {
        destp[i] = data;
    }
}

void MIi_CpuCopy32(const void *srcp, void *destp, u32 size) {
    memmove(destp, srcp, size & ~3u);
}

void MIi_CpuSend32(const void *srcp, volatile void *destp, u32 size) {
    /* Writes every word to the same (FIFO) address. */
    const u32 *s = srcp;
    volatile u32 *d = destp;
    if (pc_g3_is_port(destp)) {
        pc_g3_cancel(destp);
        for (u32 i = 0; i < size / 4; i++) {
            pc_g3_write((u32)(uintptr_t)destp, s[i]);
        }
        return;
    }
    for (u32 i = 0; i < size / 4; i++) {
        *d = s[i];
    }
}

void MIi_CpuClearFast(u32 data, void *destp, u32 size) {
    MIi_CpuClear32(data, destp, size);
}

void MIi_CpuCopyFast(const void *srcp, void *destp, u32 size) {
    MIi_CpuCopy32(srcp, destp, size);
}

/* Copies to the geometry FIFO / command ports feed the 3D engine; copies
 * from the 3D result registers (0x04000600+) first complete pending commands. */
static void copy_words(const void *pSrc, void *pDest, u32 size) {
    if (pc_g3_is_port(pDest)) {
        pc_g3_cancel(pDest);
        const u32 *s = pSrc;
        for (u32 i = 0; i < size / 4; i++) {
            pc_g3_write((u32)(uintptr_t)pDest + i * 4, s[i]);
        }
        return;
    }
    uintptr_t src = (uintptr_t)pSrc;
    if (src >= 0x04000600 && src < 0x04000700) {
        pc_g3_flush();
    }
    memcpy(pDest, pSrc, size);
}

void MI_Copy32B(const void *pSrc, void *pDest) { copy_words(pSrc, pDest, 32); }
void MI_Copy36B(const void *pSrc, void *pDest) { copy_words(pSrc, pDest, 36); }
void MI_Copy48B(const void *pSrc, void *pDest) { copy_words(pSrc, pDest, 48); }
void MI_Copy64B(const void *pSrc, void *pDest) { copy_words(pSrc, pDest, 64); }
void MI_Copy16B(const void *pSrc, void *pDest) { copy_words(pSrc, pDest, 16); }
void MI_Copy128B(const void *pSrc, void *pDest) { copy_words(pSrc, pDest, 128); }
void MI_Zero32B(void *pDest) { memset(pDest, 0, 32); }
void MI_Zero36B(void *pDest) { memset(pDest, 0, 36); }
void MI_Zero48B(void *pDest) { memset(pDest, 0, 48); }
void MI_Zero64B(void *pDest) { memset(pDest, 0, 64); }

/* Writes every halfword to the same (I/O) address. */
void MIi_CpuSend16(const void *srcp, volatile void *destp, u32 size) {
    const u16 *s = srcp;
    volatile u16 *d = destp;
    for (u32 i = 0; i < size / 2; i++) {
        *d = s[i];
    }
}

void MI_CpuFill8(void *dstp, u8 data, u32 size) {
    memset(dstp, data, size);
}

void MI_CpuCopy8(const void *srcp, void *dstp, u32 size) {
    memmove(dstp, srcp, size);
}
