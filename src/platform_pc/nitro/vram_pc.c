/*
 * vram_pc.c - the nine DS VRAM banks (A-I) and their VRAMCNT mapping.
 *
 * On the DS a bank is physical memory that VRAMCNT maps to a CPU address
 * range (BG/OBJ of engine A or B, LCDC) or to a slot the CPU cannot see
 * (texture image/palette, extended palettes, ARM7). On PC the CPU address
 * ranges are plain memory at their native addresses (hw_pc.c), so a bank's
 * contents live:
 *   - at its CPU address while it is mapped to one,
 *   - in s_store otherwise.
 * pc_vram_sync() (called by every lib/NitroSDK/src/gx/gx_vramcnt.c function
 * that writes VRAMCNT) moves banks whose mapping changed: their old CPU
 * range is saved to the store and cleared, then the store is copied to the
 * new CPU range. Remaps happen on scene changes only, so the copies are cheap.
 *
 * The renderers read hidden slots (extended palettes, textures) here.
 */
#include "nitro_pc.h"
#include "vram_pc.h"

#include <string.h>

enum { BANK_A, BANK_B, BANK_C, BANK_D, BANK_E, BANK_F, BANK_G, BANK_H, BANK_I, BANK_COUNT };

typedef struct {
    u32 size;
    u32 lcdc;      /* LCDC (MST 0) address */
    u32 cnt_reg;   /* VRAMCNT_x register */
} BankInfo;

static const BankInfo s_banks[BANK_COUNT] = {
    { 0x20000, 0x06800000, 0x04000240 },
    { 0x20000, 0x06820000, 0x04000241 },
    { 0x20000, 0x06840000, 0x04000242 },
    { 0x20000, 0x06860000, 0x04000243 },
    { 0x10000, 0x06880000, 0x04000244 },
    { 0x04000, 0x06890000, 0x04000245 },
    { 0x04000, 0x06894000, 0x04000246 },
    { 0x08000, 0x06898000, 0x04000248 },
    { 0x04000, 0x068A0000, 0x04000249 },
};

typedef enum {
    MAP_NONE,
    MAP_CPU,         /* addr = CPU address */
    MAP_TEX,         /* addr = offset in the 512 KiB texture image space */
    MAP_TEXPLTT,     /* addr = offset in the texture palette space */
    MAP_BGEXT_A,     /* addr = offset in engine A's 32 KiB BG extended palettes */
    MAP_OBJEXT_A,
    MAP_BGEXT_B,
    MAP_OBJEXT_B,
    MAP_ARM7,
} MapKind;

typedef struct {
    MapKind kind;
    u32     addr;
} Mapping;

static u8  s_store_A[0x20000], s_store_B[0x20000], s_store_C[0x20000], s_store_D[0x20000];
static u8  s_store_E[0x10000], s_store_F[0x4000], s_store_G[0x4000], s_store_H[0x8000], s_store_I[0x4000];
static u8 *const s_store[BANK_COUNT] = {
    s_store_A, s_store_B, s_store_C, s_store_D, s_store_E, s_store_F, s_store_G, s_store_H, s_store_I,
};
static u8 s_cur[BANK_COUNT];   /* VRAMCNT values currently applied */

/* GBATEK "DS Memory Control - VRAM". */
static Mapping decode(int bank, u8 cnt) {
    Mapping m = { MAP_NONE, 0 };
    if (!(cnt & 0x80)) {
        return m;
    }
    u32 mst = cnt & 7, ofs = (cnt >> 3) & 3;
    if (bank <= BANK_B) {
        mst &= 3;
    }
    if (mst == 0) {
        m.kind = MAP_CPU;
        m.addr = s_banks[bank].lcdc;
        return m;
    }
    switch (bank) {
    case BANK_A:
    case BANK_B:
    case BANK_C:
    case BANK_D:
        if (mst == 1) {
            m = (Mapping){ MAP_CPU, 0x06000000 + 0x20000 * ofs };
        } else if (mst == 2) {
            m = bank <= BANK_B ? (Mapping){ MAP_CPU, 0x06400000 + 0x20000 * (ofs & 1) } : (Mapping){ MAP_ARM7, 0x20000 * (ofs & 1) };
        } else if (mst == 3) {
            m = (Mapping){ MAP_TEX, 0x20000 * ofs };
        } else if (mst == 4 && bank == BANK_C) {
            m = (Mapping){ MAP_CPU, 0x06200000 };
        } else if (mst == 4 && bank == BANK_D) {
            m = (Mapping){ MAP_CPU, 0x06600000 };
        }
        break;
    case BANK_E:
        switch (mst) {
        case 1: m = (Mapping){ MAP_CPU, 0x06000000 }; break;
        case 2: m = (Mapping){ MAP_CPU, 0x06400000 }; break;
        case 3: m = (Mapping){ MAP_TEXPLTT, 0 }; break;
        case 4: m = (Mapping){ MAP_BGEXT_A, 0 }; break;
        }
        break;
    case BANK_F:
    case BANK_G: {
        u32 off = 0x4000 * (ofs & 1) + 0x10000 * (ofs >> 1);
        switch (mst) {
        case 1: m = (Mapping){ MAP_CPU, 0x06000000 + off }; break;
        case 2: m = (Mapping){ MAP_CPU, 0x06400000 + off }; break;
        case 3: m = (Mapping){ MAP_TEXPLTT, off }; break;
        case 4: m = (Mapping){ MAP_BGEXT_A, 0x4000 * (ofs & 1) }; break;
        case 5: m = (Mapping){ MAP_OBJEXT_A, 0 }; break;
        }
        break;
    }
    case BANK_H:
        switch (mst) {
        case 1: m = (Mapping){ MAP_CPU, 0x06200000 }; break;
        case 2: m = (Mapping){ MAP_BGEXT_B, 0 }; break;
        }
        break;
    case BANK_I:
        switch (mst) {
        case 1: m = (Mapping){ MAP_CPU, 0x06208000 }; break;
        case 2: m = (Mapping){ MAP_CPU, 0x06600000 }; break;
        case 3: m = (Mapping){ MAP_OBJEXT_B, 0 }; break;
        }
        break;
    }
    return m;
}

static u8 read_cnt(int bank) {
    return *(volatile u8 *)(uintptr_t)s_banks[bank].cnt_reg;
}

static BOOL cpu_overlaps(Mapping a, u32 asize, Mapping b, u32 bsize) {
    return a.kind == MAP_CPU && b.kind == MAP_CPU && a.addr < b.addr + bsize && b.addr < a.addr + asize;
}

void pc_vram_sync(void) {
    u8 next[BANK_COUNT];
    BOOL changed[BANK_COUNT];
    BOOL any = FALSE;
    for (int i = 0; i < BANK_COUNT; i++) {
        next[i] = read_cnt(i);
        changed[i] = next[i] != s_cur[i];
        any |= changed[i];
    }
    if (!any) {
        return;
    }
    /* 1. Save the contents of every bank leaving a CPU range. */
    for (int i = 0; i < BANK_COUNT; i++) {
        Mapping old = decode(i, s_cur[i]);
        if (changed[i] && old.kind == MAP_CPU) {
            memcpy(s_store[i], (void *)(uintptr_t)old.addr, s_banks[i].size);
        }
    }
    /* 2. Clear the ranges they leave (unmapped VRAM reads as 0), unless a
     *    bank that stays mapped shares them. */
    for (int i = 0; i < BANK_COUNT; i++) {
        Mapping old = decode(i, s_cur[i]);
        if (!changed[i] || old.kind != MAP_CPU) {
            continue;
        }
        BOOL shared = FALSE;
        for (int j = 0; j < BANK_COUNT; j++) {
            if (j != i && !changed[j] && cpu_overlaps(old, s_banks[i].size, decode(j, s_cur[j]), s_banks[j].size)) {
                shared = TRUE;
            }
        }
        if (!shared) {
            memset((void *)(uintptr_t)old.addr, 0, s_banks[i].size);
        }
    }
    /* 3. Bring in the banks entering a CPU range. */
    for (int i = 0; i < BANK_COUNT; i++) {
        Mapping now = decode(i, next[i]);
        if (changed[i] && now.kind == MAP_CPU) {
            memcpy((void *)(uintptr_t)now.addr, s_store[i], s_banks[i].size);
        }
        s_cur[i] = next[i];
    }
}

void pc_vram_reset(void) {
    for (int i = 0; i < BANK_COUNT; i++) {
        memset(s_store[i], 0, s_banks[i].size);
        s_cur[i] = 0;
    }
}

/* Contents of a bank whatever its mapping. */
static const u8 *bank_data(int bank) {
    Mapping m = decode(bank, s_cur[bank]);
    return m.kind == MAP_CPU ? (const u8 *)(uintptr_t)m.addr : s_store[bank];
}

/* Assembles a hidden slot space (extended palettes, texture palettes...)
 * from the banks mapped to it; unmapped parts read as 0. */
static void gather(MapKind kind, u8 *out, u32 size) {
    memset(out, 0, size);
    for (int i = 0; i < BANK_COUNT; i++) {
        Mapping m = decode(i, s_cur[i]);
        if (m.kind != kind || m.addr >= size) {
            continue;
        }
        u32 n = s_banks[i].size;
        if (n > size - m.addr) {
            n = size - m.addr;
        }
        memcpy(out + m.addr, bank_data(i), n);
    }
}

void pc_vram_bg_ext_palettes(int engine, u16 out[0x4000]) {
    gather(engine == 0 ? MAP_BGEXT_A : MAP_BGEXT_B, (u8 *)out, 0x8000);
}

void pc_vram_obj_ext_palettes(int engine, u16 out[0x1000]) {
    gather(engine == 0 ? MAP_OBJEXT_A : MAP_OBJEXT_B, (u8 *)out, 0x2000);
}

void pc_vram_texture_image(u8 out[0x80000]) {
    gather(MAP_TEX, out, 0x80000);
}

void pc_vram_texture_palette(u8 out[0x18000]) {
    gather(MAP_TEXPLTT, out, 0x18000);
}

const u8 *pc_vram_lcdc_bank(int bank) {
    return bank >= 0 && bank < BANK_COUNT ? bank_data(bank) : NULL;
}
