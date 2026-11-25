/*
 * os_nitro_pc.c - NitroSDK OS layer on the PC HAL.
 *
 * Implemented: arenas (main RAM at its DS address), interrupt enable state,
 * IRQ handler table + VBlank dispatch, OS_WaitIrq / OS_WaitVBlankIntr paced
 * at 59.83 Hz, tick, reset parameter, owner info, entropy, misc queries.
 *
 * Not yet (milestone 3, needed once the whole game links): threads, mutexes,
 * alarms, message queues - the game runs single-threaded until then.
 */
#include "nitro_pc.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------- arenas */

/* Kept in host memory: on 64-bit OSArenaInfo (2 x 9 pointers) would no
 * longer fit at HW_ARENA_INFO_BUF without overlapping HW_ROM_HEADER_BUF. */
static void *s_arena_lo[OS_ARENA_MAX];
static void *s_arena_hi[OS_ARENA_MAX];
static void *s_arena_init_lo[OS_ARENA_MAX];
static void *s_arena_init_hi[OS_ARENA_MAX];

static void set_initial_arena(OSArenaId id, u32 lo, u32 hi) {
    s_arena_init_lo[id] = s_arena_lo[id] = (void *)(uintptr_t)lo;
    s_arena_init_hi[id] = s_arena_hi[id] = (void *)(uintptr_t)hi;
}

void OS_InitArena(void) {
    static BOOL initialized;
    if (initialized) {
        return;
    }
    initialized = TRUE;
    /* Retail 4 MiB unit. Code and static data are host-side on PC, so the
     * whole main RAM is available to the arenas (the DS leaves ~1.3 MiB). */
    set_initial_arena(OS_ARENA_MAIN, PC_HW_MAIN_MEM, PC_HW_ARENA_END);
    set_initial_arena(OS_ARENA_MAIN_SUBPRIV, PC_HW_ARENA_END, PC_HW_ARENA_END);
    set_initial_arena(OS_ARENA_MAINEX, PC_HW_ARENA_END, PC_HW_ARENA_END);      /* no 8 MiB expansion */
    set_initial_arena(OS_ARENA_ITCM, HW_ITCM, HW_ITCM_END);
    set_initial_arena(OS_ARENA_DTCM, PC_HW_DTCM, PC_HW_DTCM + 0x3000);       /* top: IRQ/SVC stacks */
    set_initial_arena(OS_ARENA_SHARED, HW_SHARED_ARENA_LO_DEFAULT, HW_SHARED_ARENA_HI_DEFAULT);
    set_initial_arena(OS_ARENA_WRAM_MAIN, HW_WRAM, HW_WRAM);                  /* WRAM belongs to ARM7 */
    set_initial_arena(OS_ARENA_WRAM_SUB, HW_WRAM, HW_WRAM);
    set_initial_arena(OS_ARENA_WRAM_SUBPRIV, HW_WRAM, HW_WRAM);
}

void OS_InitArenaEx(void) {
}

void *OS_GetArenaHi(OSArenaId id) { return s_arena_hi[id]; }
void *OS_GetArenaLo(OSArenaId id) { return s_arena_lo[id]; }
void *OS_GetInitArenaHi(OSArenaId id) { return s_arena_init_hi[id]; }
void *OS_GetInitArenaLo(OSArenaId id) { return s_arena_init_lo[id]; }
void OS_SetArenaHi(OSArenaId id, void *newHi) { s_arena_hi[id] = newHi; }
void OS_SetArenaLo(OSArenaId id, void *newLo) { s_arena_lo[id] = newLo; }

#define ROUND_UP(p, a)   (((uintptr_t)(p) + ((a) - 1)) & ~(uintptr_t)((a) - 1))
#define ROUND_DOWN(p, a) ((uintptr_t)(p) & ~(uintptr_t)((a) - 1))

/* Same semantics as lib/NitroSDK/src/os/os_arena.c. */
void *OS_AllocFromArenaLo(OSArenaId id, u32 size, u32 align) {
    if (s_arena_lo[id] == NULL) {
        return NULL;
    }
    uintptr_t ptr = ROUND_UP(s_arena_lo[id], align);
    uintptr_t lo = ROUND_UP(ptr + size, align);
    if (lo > (uintptr_t)s_arena_hi[id]) {
        return NULL;
    }
    s_arena_lo[id] = (void *)lo;
    return (void *)ptr;
}

void *OS_AllocFromArenaHi(OSArenaId id, u32 size, u32 align) {
    if (s_arena_hi[id] == NULL) {
        return NULL;
    }
    uintptr_t hi = ROUND_DOWN(s_arena_hi[id], align);
    hi = ROUND_DOWN(hi - size, align);
    if (hi < (uintptr_t)s_arena_lo[id]) {
        return NULL;
    }
    s_arena_hi[id] = (void *)hi;
    return (void *)hi;
}

/* --------------------------------------------------------- interrupts */

/* The game runs on one host thread: "disabling interrupts" only has to
 * defer IRQ handlers, which we only ever run from OS_WaitIrq / VBlank. */
#ifndef OS_INTRMODE_ENABLE   /* CPSR I bit, as returned by OS_DisableInterrupts */
#define OS_INTRMODE_ENABLE      0x00u
#define OS_INTRMODE_DISABLE_IRQ 0x80u
#endif
static OSIntrMode s_intr_mode = OS_INTRMODE_ENABLE;

OSIntrMode OS_DisableInterrupts(void) {
    OSIntrMode prev = s_intr_mode;
    s_intr_mode = OS_INTRMODE_DISABLE_IRQ;
    return prev;
}

OSIntrMode OS_EnableInterrupts(void) {
    OSIntrMode prev = s_intr_mode;
    s_intr_mode = OS_INTRMODE_ENABLE;
    return prev;
}

OSIntrMode OS_RestoreInterrupts(OSIntrMode state) {
    OSIntrMode prev = s_intr_mode;
    s_intr_mode = state;
    return prev;
}

OSProcMode OS_GetProcMode(void) {
    return OS_PROCMODE_SYS;
}

/* ------------------------------------------------------------- IRQs */

#define IRQ_COUNT 25
static OSIrqFunction s_irq_table[IRQ_COUNT];

static int irq_index(OSIrqMask bit) {
    for (int i = 0; i < IRQ_COUNT; i++) {
        if (bit & (1u << i)) {
            return i;
        }
    }
    return -1;
}

void OS_SetIrqFunction(OSIrqMask intrBit, OSIrqFunction function) {
    for (int i = 0; i < IRQ_COUNT; i++) {
        if (intrBit & (1u << i)) {
            s_irq_table[i] = function;
        }
    }
}

OSIrqFunction OS_GetIrqFunction(OSIrqMask intrBit) {
    int i = irq_index(intrBit);
    return i < 0 ? NULL : s_irq_table[i];
}

OSIrqMask OS_SetIrqMask(OSIrqMask mask) {
    OSIrqMask prev = reg_OS_IE;
    reg_OS_IE = mask;
    return prev;
}

OSIrqMask OS_EnableIrqMask(OSIrqMask mask) {
    OSIrqMask prev = reg_OS_IE;
    reg_OS_IE = prev | mask;
    return prev;
}

OSIrqMask OS_DisableIrqMask(OSIrqMask mask) {
    OSIrqMask prev = reg_OS_IE;
    reg_OS_IE = prev & ~mask;
    return prev;
}

OSIrqMask OS_ResetRequestIrqMask(OSIrqMask mask) {
    OSIrqMask prev = reg_OS_IF;
    reg_OS_IF = prev & ~mask;
    return prev;
}

/* Runs the handler for `irq` the way the ARM9 IRQ vector would, honouring
 * IME and IE, then flags it in the IRQ check buffer for OS_WaitIrq users. */
void pc_nitro_os_dispatch_irq(OSIrqMask irq) {
    reg_OS_IF |= irq;
    if ((s_intr_mode & OS_INTRMODE_DISABLE_IRQ) || !(reg_OS_IME & 1) || !(reg_OS_IE & irq)) {
        return;   /* stays pending in IF */
    }
    int i = irq_index(irq);
    reg_OS_IF &= ~irq;
    if (i >= 0 && s_irq_table[i] != NULL) {
        s_irq_table[i]();
    }
}

void OS_WaitIrq(BOOL clear, OSIrqMask irqFlags) {
    if (clear) {
        OS_ClearIrqCheckFlag(irqFlags);
    }
    /* VBlank is the only interrupt source emulated so far; every other wait
     * also returns at the next frame so callers cannot spin forever. */
    while (!(OS_GetIrqCheckFlag() & irqFlags)) {
        pc_os_wait_vblank();
        pc_nitro_vblank();
        if (!(irqFlags & OS_IE_V_BLANK)) {
            break;
        }
    }
    OS_ClearIrqCheckFlag(irqFlags);
}

void OS_WaitVBlankIntr(void) {
    OS_WaitIrq(TRUE, OS_IE_V_BLANK);
}

/* ------------------------------------------------------------- ticks */

static BOOL s_tick_ready;

void OS_InitTick(void) {
    s_tick_ready = TRUE;
}

BOOL OS_IsTickAvailable(void) {
    return s_tick_ready;
}

OSTick OS_GetTick(void) {
    return (OSTick)pc_os_ticks();
}

void OS_Sleep(u32 msec) {
    uint64_t end = pc_os_ms() + msec;
    while (pc_os_ms() < end) {
        pc_os_wait_vblank();
        pc_nitro_vblank();
    }
}

/* ------------------------------------------------------------ system */

void OS_Init(void) {
    OS_InitArena();
    OS_InitTick();
    reg_OS_IME = 1;
}

void OS_Terminate(void) {
    pc_fatal("OS_Terminate() called by the game");
}

void OS_ResetSystem(u32 parameter) {
    pc_nitro_soft_reset(parameter);   /* does not return */
}

u32 OS_GetConsoleType(void) {
    return OS_CONSOLE_NITRO | OS_CONSOLE_SIZE_4MB;
}

void OS_GetLowEntropyData(u32 buffer[OS_LOW_ENTROPY_DATA_SIZE / sizeof(u32)]) {
    uint64_t t = pc_os_ticks() ^ ((uint64_t)time(NULL) << 20);
    for (u32 i = 0; i < OS_LOW_ENTROPY_DATA_SIZE / sizeof(u32); i++) {
        t ^= t << 13;
        t ^= t >> 7;
        t ^= t << 17;
        buffer[i] = (u32)t;
    }
}

void OS_GetMacAddress(u8 *macAddr) {
    /* Locally administered, stable address (used for trainer ID seeds). */
    static const u8 mac[6] = { 0x02, 0x00, 0x50, 0x48, 0x47, 0x01 };
    memcpy(macAddr, mac, sizeof mac);
}

void OS_GetOwnerInfo(OSOwnerInfo *info) {
    static const char name[] = "PLAYER";
    memset(info, 0, sizeof *info);
    info->language = 1;   /* English */
    info->favoriteColor = 0;
    info->birthday.month = 1;
    info->birthday.day = 1;
    for (u32 i = 0; i < sizeof name - 1; i++) {
        info->nickName[i] = (u16)name[i];
    }
    info->nickNameLength = sizeof name - 1;
}

s64 OS_GetOwnerRtcOffset(void) {
    return 0;
}

/* ------------------------------------------------------------ helpers */

uintptr_t pc_nitro_check_ptr32(const void *p, const char *file, int line) {
    uintptr_t v = (uintptr_t)p;
    if (v > 0xFFFFFFFFu) {
        pc_fatal("%s:%d: pointer %p does not fit the DS 32-bit address space "
                 "(host stack/heap pointer passed to the SDK)", file, line, p);
    }
    return v;
}

/* The V-Blank interrupt the DS would deliver in the middle of long work
 * (src/sys_task.c: V-Blank task list full during a map load). Runs the
 * game's V-Blank handler now, as the hardware would, without drawing. */
void pc_nitro_vblank_irq_now(void) {
    pc_nitro_os_dispatch_irq(OS_IE_V_BLANK);
}
