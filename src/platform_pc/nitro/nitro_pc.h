/*
 * nitro_pc.h - glue between the NitroSDK API (as seen by game code) and the
 * PC HAL. Files in src/platform_pc/nitro/ are compiled with the game's
 * include paths and the real Nitro headers, and implement the Nitro
 * functions the DS gets from lib/NitroSDK on top of platform_pc.h.
 */
#ifndef PPL_NITRO_PC_H
#define PPL_NITRO_PC_H

#include <nitro.h>

#include "hw_pc.h"
#include "platform_pc.h"

/* Brings up the Nitro layer: memory map, OS arenas, tick, IRQ table, FS.
 * data_dir forces a single NitroFS root; NULL uses the standard layers for
 * `lang` (mods/<lang>, mods, pc_data/<lang>, files). save_path is the
 * backup memory file. */
BOOL pc_nitro_init(const char *data_dir, const char *lang, const char *save_path);
void pc_nitro_shutdown(void);

/* Runs the game's entry point (NitroMain). A soft reset (OS_ResetSystem)
 * restores every game variable and the DS memory map to their start-up
 * state, then restarts the entry point from here, in the same window. */
void pc_nitro_run(void (*entry)(void));
__attribute__((noreturn)) void pc_nitro_soft_reset(u32 parameter);
void pc_nitro_fs_soft_reset(void);

/* Called once per emulated frame, at VBlank: publishes inputs to the
 * virtual registers, feeds touch auto-sampling, then runs the VBlank IRQ
 * handler registered with OS_SetIrqFunction (if IME/IE allow it). */
void pc_nitro_vblank(void);

/* With a window, each VBlank also pumps host events and presents a frame. */
void pc_nitro_set_window(BOOL has_window);

/* Internal hooks shared between adapter files. */
void pc_nitro_tp_vblank(void);
void pc_nitro_os_dispatch_irq(OSIrqMask irq);
void pc_nitro_os_alarm_tick(void);

/* H-Blank DMA programmed by the game, replayed per scanline by the 2D
 * renderer (milestone 4). */
typedef struct PcDmaHBlank {
    u32  src, dest, ctrl;
    BOOL active;
} PcDmaHBlank;
const PcDmaHBlank *pc_dma_hblank_channel(u32 dmaNo);

/* Geometry-engine FIFO sink (3D backend, milestone 5). */
void pc_gx_fifo_submit(const u32 *words, u32 count);

/* Pointers handed to the SDK as u32 (CARD, DMA...) must lie in the low
 * 4 GiB; anything else means a host stack/heap pointer leaked into game
 * code and would be silently truncated. */
#define PC_ADDR32(p) ((u32)pc_nitro_check_ptr32((const void *)(p), __FILE__, __LINE__))
uintptr_t pc_nitro_check_ptr32(const void *p, const char *file, int line);

/* wm_pc.c: the wireless manager as an empty radio. */
void pc_wm_vblank(void);
void pc_wm_reset(void);

#endif /* PPL_NITRO_PC_H */
