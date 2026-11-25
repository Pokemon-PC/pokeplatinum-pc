/*
 * hw_pc.h - the Nintendo DS (ARM9) memory map, reproduced at its real
 * addresses inside the host process.
 *
 * Game and NitroSDK code access hardware through fixed addresses
 * (reg_GX_DISPCNT is *(vu32 *)0x04000000, HW_BG_VRAM is 0x06000000...) and
 * frequently keep such addresses - and pointers into main RAM - in 32-bit
 * integers. Instead of rewriting hundreds of those sites, pc_hw_init()
 * reserves the very same address ranges in the low 4 GiB of the 64-bit host
 * address space, so that:
 *   - every register / VRAM / palette / OAM access works unmodified,
 *   - every pointer into emulated memory fits in a u32.
 *
 * Writes to this memory have no side effect by themselves: the HAL reads the
 * state it needs (pc_hw_* helpers, the 2D renderer at VBlank, ...) and the
 * few registers with immediate effects (DMA, divider, 3D FIFO) are handled by
 * their Nitro API wrappers.
 */
#ifndef PPL_HW_PC_H
#define PPL_HW_PC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Address ranges (64 KiB aligned, as required by Windows reservations). */
#define PC_HW_MAIN_BASE   0x01FF0000u   /* ITCM 0x01FF8000 + main RAM (+mirror/system area) */
#define PC_HW_MAIN_END    0x03010000u   /* ... + shared WRAM 0x03000000                     */
#define PC_HW_IO_BASE     0x04000000u   /* I/O registers (0x04000000) + 0x04100000 block    */
#define PC_HW_IO_END      0x04200000u
#define PC_HW_PLTT_BASE   0x05000000u   /* palettes, engine A + B                          */
#define PC_HW_PLTT_END    0x05010000u
#define PC_HW_VRAM_BASE   0x06000000u   /* BG/OBJ VRAM A+B, LCDC 0x06800000-0x068A4000     */
#define PC_HW_VRAM_END    0x06900000u
#define PC_HW_OAM_BASE    0x07000000u   /* OAM, engine A + B                               */
#define PC_HW_OAM_END     0x07010000u

/* Main memory available to the OS arenas (the DS has 4 MiB; code and static
 * data do not live there on PC, so nearly all of it is free). */
#define PC_HW_MAIN_MEM        0x02000000u
#define PC_HW_MAIN_MEM_END    0x023E0000u  /* HW_MAIN_MEM_MAIN_END on retail 4 MiB units */
/* The PC build gives the OS main arena everything up to the DS system area
 * (DTCM at 0x027E0000, boot/ROM-header buffers above): game structures holding
 * pointers are larger on a 64-bit host, so heaps are scaled up (src/heap.c). */
#define PC_HW_ARENA_END       0x027C0000u
#define PC_HW_DTCM            0x027E0000u
#define PC_HW_DTCM_SIZE       0x00004000u

/* Reserves and zero-fills the whole map, then sets power-on register values.
 * Must run before any game / Nitro code. Returns false (and logs why) if the
 * host refused one of the fixed addresses. */
bool pc_hw_init(void);
void pc_hw_shutdown(void);
/* Power-on state again: all DS memory and registers cleared (soft reset). */
void pc_hw_reset(void);
bool pc_hw_is_mapped(uint32_t address, uint32_t size);

static inline volatile uint16_t *pc_hw_reg16(uint32_t address) { return (volatile uint16_t *)(uintptr_t)address; }
static inline volatile uint32_t *pc_hw_reg32(uint32_t address) { return (volatile uint32_t *)(uintptr_t)address; }

/* Inputs, published where PAD_Read()/PAD_DetectFold() expect them:
 * KEYINPUT (0x04000130, active-low A..L) and the ARM7-shared word at
 * HW_BUTTON_XY_BUF (X, Y, debug active-low; bit 15 = lid folded). */
void pc_hw_set_pad(uint16_t buttons_active_high, bool lid_closed);

/* VCOUNT / DISPSTAT (VBlank flag) as seen by code polling the registers. */
void pc_hw_set_vcount(uint16_t line);

#ifdef __cplusplus
}
#endif

#endif /* PPL_HW_PC_H */
