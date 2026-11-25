/*
 * os_pc.c - replacement for the NitroSDK OS layer.
 *
 * Milestone 1 provides: timing (OS_GetTick base), a fixed 59.8261 Hz VBlank
 * pacer (what OS_WaitIrq(TRUE, OS_IE_VBLANK) blocks on), and aligned host
 * allocation used underneath OS_AllocFromHeap / NNS_FndCreateExpHeap.
 *
 * Planned (milestone 2): OS_Init/OS_InitArena/OS_InitAlloc arenas carved out
 * of one host block, OS_*Thread on top of SDL threads with a cooperative
 * scheduler (the game only uses a handful of threads: main, sound, card),
 * OS_*Mutex, OS_*Alarm, OS_SetIrqFunction dispatch, OS_ResetSystem via
 * longjmp back to the entry point.
 */
#include "platform_pc.h"

#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#if defined(_WIN32)
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
/* Sleep() / SDL_Delay() granularity is 15.6 ms unless timeBeginPeriod(1)
 * is honoured - and Windows 11 ignores it for background or windowless
 * processes. A high-resolution waitable timer (Windows 10 1803+) gives
 * ~0.5 ms precision regardless. */
static HANDLE s_hires_timer;

static void precise_sleep_us(uint64_t us) {
    if (s_hires_timer != NULL) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(us * 10);   /* relative, 100 ns units */
        if (SetWaitableTimer(s_hires_timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(s_hires_timer, INFINITE);
            return;
        }
    }
    SDL_Delay((Uint32)(us / 1000));
}
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
/* mach_wait_until: the precise sleep of macOS (nanosleep may be coalesced). */
static void precise_sleep_us(uint64_t us) {
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) {
        mach_timebase_info(&tb);
    }
    mach_wait_until(mach_absolute_time() + us * 1000u * tb.denom / tb.numer);
}
#else
static void precise_sleep_us(uint64_t us) {
    SDL_Delay((Uint32)(us / 1000));   /* nanosleep-backed, ~0.1 ms precise */
}
#endif

static uint64_t s_perf_freq;
static uint64_t s_perf_start;
static uint64_t s_next_vblank;      /* in performance-counter units */
static uint64_t s_vblank_count;
static double   s_frame_ticks;      /* perf-counter units per DS frame */
static double   s_frame_accum;      /* fractional part accumulator      */

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

bool pc_os_init(void) {
#if defined(__linux__) && defined(__GLIBC__)
    /* Keep host allocations in the brk heap that follows the executable
     * (below 4 GiB): large blocks would otherwise be mmap'ed high, and some
     * game code stores addresses in 32-bit words. */
    mallopt(M_MMAP_THRESHOLD, 512 * 1024 * 1024);
#endif
    SDL_SetMainReady();   /* main() may live in a file that cannot include SDL */
    if (SDL_InitSubSystem(SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        pc_warn("SDL timer/events init failed: %s", SDL_GetError());
        return false;
    }
#if defined(__APPLE__)
    /* Default QoS lets macOS coalesce timers (sleeps of 15 ms took ~40 ms):
     * the game thread paces frames, mark it latency-critical. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
#if defined(_WIN32)
    if (s_hires_timer == NULL) {
        s_hires_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
#endif
    s_perf_freq   = SDL_GetPerformanceFrequency();
    s_perf_start  = SDL_GetPerformanceCounter();
    s_frame_ticks = (double)s_perf_freq / PC_DS_FRAME_RATE;
    s_frame_accum = 0.0;
    s_next_vblank = s_perf_start + (uint64_t)s_frame_ticks;
    s_vblank_count = 0;
    return true;
}

void pc_os_shutdown(void) {
#if defined(_WIN32)
    if (s_hires_timer != NULL) {
        CloseHandle(s_hires_timer);
        s_hires_timer = NULL;
    }
#endif
    SDL_QuitSubSystem(SDL_INIT_TIMER | SDL_INIT_EVENTS);
}

uint64_t pc_os_ticks(void) {
    /* OS_GetTick() runs at (bus clock / 64). */
    uint64_t elapsed = SDL_GetPerformanceCounter() - s_perf_start;
    const uint64_t tick_hz = PC_DS_CPU_HZ / 64u;
    return (elapsed / s_perf_freq) * tick_hz + ((elapsed % s_perf_freq) * tick_hz) / s_perf_freq;
}

uint64_t pc_os_ms(void) {
    uint64_t elapsed = SDL_GetPerformanceCounter() - s_perf_start;
    return elapsed * 1000u / s_perf_freq;
}

/* 1 = normal, N = N times faster (fast-forward), 0 = no pacing at all.
 * PPL_TURBO=1 (tests) starts at 0. */
static int s_speed = -1;

int pc_os_speed(void) {
    if (s_speed < 0) {
        const char *t = getenv("PPL_TURBO");
        s_speed = (t != NULL && t[0] != '\0' && t[0] != '0') ? 0 : 1;
    }
    return s_speed;
}

void pc_os_set_speed(int speed) {
    s_speed = speed < 0 ? 1 : speed;
}

static void advance_deadline(void) {
    int speed = pc_os_speed();
    s_frame_accum += s_frame_ticks / (double)(speed > 1 ? speed : 1);
    uint64_t whole = (uint64_t)s_frame_accum;
    s_frame_accum -= (double)whole;
    s_next_vblank += whole;
}

uint32_t pc_os_wait_vblank(void) {
    uint64_t now = SDL_GetPerformanceCounter();

    /* Speed 0: no frame pacing, the game runs as fast as it can. Game time
     * still advances one frame per V-Blank. */
    if (pc_os_speed() == 0) {
        s_vblank_count++;
        s_next_vblank = now + (uint64_t)s_frame_ticks;
        return 1;
    }

    /* Sleep until shortly before the deadline, then spin for accuracy. The
     * margin adapts to how late sleeps wake up on this host (~0.1 ms on a
     * normal machine, 20 ms+ on some VMs with coalesced timers). */
    static uint64_t s_oversleep_us = 0;
    while (now < s_next_vblank) {
        uint64_t remaining_us = (s_next_vblank - now) * 1000000u / s_perf_freq;
        uint64_t margin_us = 1000 + s_oversleep_us;
        if (remaining_us > margin_us + 500) {
            uint64_t want = remaining_us - margin_us;
            uint64_t t0 = SDL_GetPerformanceCounter();
            precise_sleep_us(want);
            uint64_t slept = (SDL_GetPerformanceCounter() - t0) * 1000000u / s_perf_freq;
            uint64_t late = slept > want ? slept - want : 0;
            /* fast attack, slow decay */
            s_oversleep_us = late > s_oversleep_us ? late : (s_oversleep_us * 15 + late) / 16;
        }
        now = SDL_GetPerformanceCounter();
    }

    uint32_t elapsed = 0;
    while (now >= s_next_vblank) {
        advance_deadline();
        elapsed++;
        /* If the host stalled for a long time (debugger, window drag),
         * resynchronise instead of fast-forwarding hundreds of frames. */
        if (elapsed > 8) {
            s_next_vblank = now + (uint64_t)s_frame_ticks;
            s_frame_accum = 0.0;
            break;
        }
    }
    s_vblank_count += elapsed;
    return elapsed;
}

uint64_t pc_os_vblank_count(void) {
    return s_vblank_count;
}

/* Aligned allocation that works the same on MSVC, MinGW and POSIX.
 * The original pointer is stored just before the returned block. */
void *pc_os_alloc(size_t size, size_t align) {
    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }
    uint8_t *raw = malloc(size + align + sizeof(void *));
    if (raw == NULL) {
        return NULL;
    }
    uintptr_t p = (uintptr_t)(raw + sizeof(void *));
    p = (p + (align - 1)) & ~(uintptr_t)(align - 1);
    ((void **)p)[-1] = raw;
    return (void *)p;
}

void pc_os_free(void *ptr) {
    if (ptr != NULL) {
        free(((void **)ptr)[-1]);
    }
}
