/*
 * ppu_pc.h - software renderer of the DS 2D engines (see ppu_pc.c).
 */
#ifndef PPL_PPU_PC_H
#define PPL_PPU_PC_H

/* Draws both screens from the current DS video state into the
 * pc_gfx_framebuffer() buffers, running H-Blank DMA and the H-Blank
 * interrupt between scanlines. */
void pc_ppu_render_frame(void);

#endif
