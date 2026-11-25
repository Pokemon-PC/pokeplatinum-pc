/*
 * gpu3d_pc.h - software DS 3D engine (see gpu3d_pc.c).
 */
#ifndef PPL_GPU3D_PC_H
#define PPL_GPU3D_PC_H

#include <nitro/types.h>

/* Geometry command input. */
void pc_g3_write(u32 addr, u32 value);              /* FIFO / command port write */
void pc_g3_flush(void);                             /* complete a pending port write */
BOOL pc_g3_is_port(const volatile void *p);         /* 0x04000400-0x040005FF */
void pc_g3_cancel(const volatile void *p);          /* address taken, no store */

/* Power-on state (soft reset). */
void pc_gpu3d_reset(void);

/* Rasterises the last swapped polygon list (once per displayed frame). */
void pc_gpu3d_render(void);
const u16 *pc_gpu3d_line_color(int y);   /* 15-bit colours */
const u8 *pc_gpu3d_line_alpha(int y);    /* 0..31, 0 = no 3D pixel */

#endif
