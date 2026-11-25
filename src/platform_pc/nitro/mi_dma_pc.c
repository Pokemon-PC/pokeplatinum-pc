/*
 * mi_dma_pc.c - DMA (lib/NitroSDK/src/mi/mi_dma*.c, mi_init.c) on PC.
 *
 * The DS DMA engine is programmed through SAD/DAD/CNT registers. Here every
 * request is decoded from the same control word and carried out at once:
 *   - immediate transfers are performed synchronously (async callbacks are
 *     invoked right away, as if the transfer had completed);
 *   - H-Blank transfers are recorded (pc_dma_hblank_channel) for the 2D
 *     renderer, which replays them line by line (milestone 4);
 *   - geometry-FIFO transfers are forwarded to pc_gx_fifo_submit (3D,
 *     milestone 5).
 */
#include "nitro_pc.h"
#include "gpu3d_pc.h"

#include <string.h>

#define DMA_CHANNELS 4

/* Control word fields (lib/include/nitro/mi/dma_shared.h, ARM9 timings). */
#define CNT_COUNT(c)     ((c) & 0x1FFFFFu)
#define CNT_DEST_CTRL(c) (((c) >> 21) & 3u)   /* 0 inc, 1 dec, 2 fixed, 3 inc+reload */
#define CNT_SRC_CTRL(c)  (((c) >> 23) & 3u)   /* 0 inc, 1 dec, 2 fixed */
#define CNT_32BIT(c)     (((c) >> 26) & 1u)
#define CNT_TIMING(c)    (((c) >> 27) & 7u)   /* 0 immediate, 1 VBlank, 2 HBlank, 3 display,
                                                  4 main-memory display, 5 card, 6 cartridge, 7 GX FIFO */

static PcDmaHBlank s_hblank[DMA_CHANNELS];

const PcDmaHBlank *pc_dma_hblank_channel(u32 dmaNo) {
    return dmaNo < DMA_CHANNELS && s_hblank[dmaNo].active ? &s_hblank[dmaNo] : NULL;
}

static void transfer(u32 src, u32 dest, u32 ctrl) {
    u32 count = CNT_COUNT(ctrl);
    if (count == 0) {
        count = 0x200000;
    }
    int unit = CNT_32BIT(ctrl) ? 4 : 2;
    int sstep = CNT_SRC_CTRL(ctrl) == 0 ? unit : CNT_SRC_CTRL(ctrl) == 1 ? -unit : 0;
    int dstep = (CNT_DEST_CTRL(ctrl) == 0 || CNT_DEST_CTRL(ctrl) == 3) ? unit : CNT_DEST_CTRL(ctrl) == 1 ? -unit : 0;
    uintptr_t s = src, d = dest;
    if (pc_g3_is_port((const volatile void *)dest)) {   /* DMA to the geometry FIFO */
        for (u32 i = 0; i < count; i++, s += (intptr_t)sstep, d += (intptr_t)dstep) {
            pc_g3_write((u32)d, unit == 4 ? *(const volatile u32 *)s : *(const volatile u16 *)s);
        }
        return;
    }
    for (u32 i = 0; i < count; i++, s += (intptr_t)sstep, d += (intptr_t)dstep) {
        if (unit == 4) {
            *(volatile u32 *)d = *(const volatile u32 *)s;
        } else {
            *(volatile u16 *)d = *(const volatile u16 *)s;
        }
    }
}

static void execute(u32 dmaNo, u32 src, u32 dest, u32 ctrl) {
    switch (CNT_TIMING(ctrl)) {
    case 0: /* immediate */
        transfer(src, dest, ctrl);
        break;
    case 2: /* H-Blank: replayed per scanline by the 2D renderer */
        if (dmaNo < DMA_CHANNELS) {
            s_hblank[dmaNo] = (PcDmaHBlank){ src, dest, ctrl, TRUE };
        }
        break;
    case 7: /* geometry FIFO */
        pc_gx_fifo_submit((const u32 *)(uintptr_t)src, CNT_COUNT(ctrl));
        break;
    default: /* V-Blank, display, card...: not used with DMA by the game */
        transfer(src, dest, ctrl);
        break;
    }
}

void MIi_DmaSetParams(u32 dmaNo, u32 src, u32 dest, u32 ctrl) { execute(dmaNo, src, dest, ctrl); }
void MIi_DmaSetParams_wait(u32 dmaNo, u32 src, u32 dest, u32 ctrl) { execute(dmaNo, src, dest, ctrl); }
void MIi_DmaSetParams_noInt(u32 dmaNo, u32 src, u32 dest, u32 ctrl) { execute(dmaNo, src, dest, ctrl); }
void MIi_DmaSetParams_wait_noInt(u32 dmaNo, u32 src, u32 dest, u32 ctrl) { execute(dmaNo, src, dest, ctrl); }

void MI_DmaFill32(u32 dmaNo, void *dest, u32 data, u32 size) {
    (void)dmaNo;
    u32 *d = dest;
    for (u32 i = 0; i < size / 4; i++) {
        d[i] = data;
    }
}

void MI_DmaCopy32(u32 dmaNo, const void *src, void *dest, u32 size) {
    (void)dmaNo;
    memmove(dest, src, size & ~3u);
}

void MI_DmaCopy16(u32 dmaNo, const void *src, void *dest, u32 size) {
    (void)dmaNo;
    memmove(dest, src, size & ~1u);
}

void MI_DmaFill32Async(u32 dmaNo, void *dest, u32 data, u32 size, MIDmaCallback callback, void *arg) {
    MI_DmaFill32(dmaNo, dest, data, size);
    if (callback != NULL) {
        callback(arg);
    }
}

void MI_DmaCopy32Async(u32 dmaNo, const void *src, void *dest, u32 size, MIDmaCallback callback, void *arg) {
    MI_DmaCopy32(dmaNo, src, dest, size);
    if (callback != NULL) {
        callback(arg);
    }
}

void MI_WaitDma(u32 dmaNo) {
    (void)dmaNo;   /* transfers complete synchronously */
}

void MI_StopDma(u32 dmaNo) {
    if (dmaNo < DMA_CHANNELS) {
        s_hblank[dmaNo].active = FALSE;
    }
}

void MIi_CheckAnotherAutoDMA(u32 dmaNo, u32 dmaType) { (void)dmaNo; (void)dmaType; }
void MIi_CheckDma0SourceAddress(u32 dmaNo, u32 src, u32 size, u32 dir) { (void)dmaNo; (void)src; (void)size; (void)dir; }

void MIi_CardDmaCopy32(u32 dmaNo, const void *src, void *dest, u32 size) {
    MI_DmaCopy32(dmaNo, src, dest, size);
}

void MI_HBlankDmaCopy32(u32 dmaNo, const void *src, void *dest, u32 size) {
    if (dmaNo < DMA_CHANNELS) {
        s_hblank[dmaNo] = (PcDmaHBlank){ PC_ADDR32(src), PC_ADDR32(dest),
                                         (1u << 26) | (3u << 21) | (size / 4), TRUE };
    }
}

void MI_HBlankDmaCopy16(u32 dmaNo, const void *src, void *dest, u32 size) {
    if (dmaNo < DMA_CHANNELS) {
        s_hblank[dmaNo] = (PcDmaHBlank){ PC_ADDR32(src), PC_ADDR32(dest), (3u << 21) | (size / 2), TRUE };
    }
}

/* ---------------------------------------------------- geometry FIFO */

void MI_SendGXCommand(u32 dmaNo, const void *src, u32 commandLength) {
    (void)dmaNo;
    pc_gx_fifo_submit(src, commandLength / 4);
}

void MI_SendGXCommandFast(u32 dmaNo, const void *src, u32 commandLength) {
    MI_SendGXCommand(dmaNo, src, commandLength);
}

void MI_SendGXCommandAsync(u32 dmaNo, const void *src, u32 commandLength, MIDmaCallback callback, void *arg) {
    MI_SendGXCommand(dmaNo, src, commandLength);
    if (callback != NULL) {
        callback(arg);
    }
}

void MI_SendGXCommandAsyncFast(u32 dmaNo, const void *src, u32 commandLength, MIDmaCallback callback, void *arg) {
    MI_SendGXCommandAsync(dmaNo, src, commandLength, callback, arg);
}

/* ------------------------------------------------------------ mi_init */

void MI_Init(void) {
    memset(s_hblank, 0, sizeof s_hblank);
}
