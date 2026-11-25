/*
 * vram_pc.h - VRAM bank mapping (VRAMCNT) and the slots the CPU cannot see.
 * See vram_pc.c.
 */
#ifndef PPL_VRAM_PC_H
#define PPL_VRAM_PC_H

#include <nitro/types.h>

/* Applies VRAMCNT changes (declared for the SDK in platform_pc_prefix.h). */
void pc_vram_sync(void);
/* Power-on state: all banks disabled and cleared (soft reset). */
void pc_vram_reset(void);

/* Snapshots of the hidden slot spaces, for the renderers (engine 0 = A). */
void pc_vram_bg_ext_palettes(int engine, u16 out[0x4000]);    /* 4 slots x 16 x 256 colours */
void pc_vram_obj_ext_palettes(int engine, u16 out[0x1000]);   /* 16 x 256 colours */
void pc_vram_texture_image(u8 out[0x80000]);
void pc_vram_texture_palette(u8 out[0x18000]);
/* Contents of bank 0..8 (A..I) whatever its mapping (display mode 2). */
const u8 *pc_vram_lcdc_bank(int bank);

#endif
