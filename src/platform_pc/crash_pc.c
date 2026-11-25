/*
 * crash_pc.c - report crashes with a raw backtrace.
 *
 * The executable is linked at a fixed base (0x10000000, no ASLR), so the
 * printed addresses are stable and can be turned into file:line with
 *     llvm-addr2line -f -e pokeplatinum.exe 0x...
 * (tools/pc_port/symbolize_crash.py does that for a whole log).
 */
#include "platform_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

static void print_backtrace(CONTEXT c, const char *tag);

/*
 * Debug aid for memory corruption: PPL_WATCH=0xADDR arms a hardware write
 * watchpoint (8 bytes) on the game thread. Every write that leaves a value
 * that is not 0 and not a DS address (0x01FF0000-0x03010000) - e.g. junk in a
 * heap link - is reported with a backtrace; execution then continues.
 */
static DWORD64 s_watch_addr;

static BOOL watch_value_suspicious(DWORD64 v) {
    return v != 0 && (v < 0x01FF0000u || v >= 0x03010000u);
}

static LONG WINAPI on_exception(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && s_watch_addr != 0 && (ep->ContextRecord->Dr6 & 1)) {
        DWORD64 v = *(volatile DWORD64 *)(uintptr_t)s_watch_addr;
        ep->ContextRecord->Dr6 = 0;
        if (watch_value_suspicious(v)) {
            fprintf(stderr, "\n[ppl:watch] 0x%llX <- 0x%016llX\n", (unsigned long long)s_watch_addr,
                    (unsigned long long)v);
            print_backtrace(*ep->ContextRecord, "watch");
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED && code != EXCEPTION_PRIV_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT *ctx = ep->ContextRecord;
    fprintf(stderr, "\n[ppl:crash] exception 0x%08lX at pc=0x%016llX", (unsigned long)code,
            (unsigned long long)ctx->Rip);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        fprintf(stderr, " (%s 0x%016llX)",
                ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write to" :
                ep->ExceptionRecord->ExceptionInformation[0] == 8 ? "execute" : "read from",
                (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    fprintf(stderr, "\n[ppl:crash] rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX rbp=%llX rsp=%llX\n"
                    "[ppl:crash] r8=%llX r9=%llX r10=%llX r11=%llX r12=%llX r13=%llX r14=%llX r15=%llX\n",
            (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx, (unsigned long long)ctx->Rcx,
            (unsigned long long)ctx->Rdx, (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi,
            (unsigned long long)ctx->Rbp, (unsigned long long)ctx->Rsp, (unsigned long long)ctx->R8,
            (unsigned long long)ctx->R9, (unsigned long long)ctx->R10, (unsigned long long)ctx->R11,
            (unsigned long long)ctx->R12, (unsigned long long)ctx->R13, (unsigned long long)ctx->R14,
            (unsigned long long)ctx->R15);
    fprintf(stderr, "[ppl:crash] backtrace:\n");

    /* Walk the faulting thread's stack with the x64 unwind tables. */
    CONTEXT c = *ctx;
    for (int depth = 0; depth < 48 && c.Rip != 0; depth++) {
        fprintf(stderr, "[ppl:crash]   #%02d 0x%016llX\n", depth, (unsigned long long)c.Rip);
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, NULL);
        if (fn == NULL) {
            /* leaf function: return address is at [rsp] */
            c.Rip = *(DWORD64 *)(uintptr_t)c.Rsp;
            c.Rsp += 8;
            continue;
        }
        PVOID handler_data;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &establisher, NULL);
    }
    fflush(stderr);
    pc_save_flush();
    ExitProcess(3);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void print_backtrace(CONTEXT c, const char *tag) {
    for (int depth = 0; depth < 48 && c.Rip != 0; depth++) {
        fprintf(stderr, "[ppl:%s]   #%02d 0x%016llX\n", tag, depth, (unsigned long long)c.Rip);
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, NULL);
        if (fn == NULL) {
            c.Rip = *(DWORD64 *)(uintptr_t)c.Rsp;
            c.Rsp += 8;
            continue;
        }
        PVOID handler_data;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &establisher, NULL);
    }
    fflush(stderr);
}

void pc_log_backtrace(const char *tag) {
    CONTEXT c;
    RtlCaptureContext(&c);
    fprintf(stderr, "[ppl:%s] game check failed; backtrace:\n", tag);
    print_backtrace(c, tag);
}

/* --- hang watchdog: if no frame is produced for a while, print where the
 * game thread is (same stable addresses as crash reports). */
static HANDLE s_game_thread;
static volatile LONG s_frames_seen;

void pc_watchdog_kick(void) {
    InterlockedIncrement(&s_frames_seen);
}

static DWORD WINAPI watchdog_main(LPVOID arg) {
    (void)arg;
    LONG last = -1;
    int stalled = 0, reported = 0;
    for (;;) {
        Sleep(1000);
        LONG now = s_frames_seen;
        stalled = (now == last) ? stalled + 1 : 0;
        last = now;
        if (stalled == 5 && reported < 3) {
            reported++;
            if (SuspendThread(s_game_thread) != (DWORD)-1) {
                CONTEXT c;
                memset(&c, 0, sizeof c);
                c.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(s_game_thread, &c)) {
                    fprintf(stderr, "\n[ppl:hang] no frame for 5 s (after %ld frames); game thread at:\n", (long)now);
                    print_backtrace(c, "hang");
                }
                ResumeThread(s_game_thread);
            }
        }
    }
    return 0;
}

/* Debug registers of a running thread can only be set while it is
 * suspended: done from a helper thread. */
static DWORD WINAPI arm_watch_main(LPVOID arg) {
    (void)arg;
    if (SuspendThread(s_game_thread) == (DWORD)-1) {
        return 1;
    }
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(s_game_thread, &c)) {
        c.Dr0 = s_watch_addr & ~(DWORD64)7;
        /* L0 enabled, break on write, 8-byte length */
        c.Dr7 = (c.Dr7 & ~(DWORD64)0xF0003) | 1 | (1u << 16) | (2u << 18);
        SetThreadContext(s_game_thread, &c);
    }
    ResumeThread(s_game_thread);
    return 0;
}

void pc_crash_install(void) {
    AddVectoredExceptionHandler(1, on_exception);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_game_thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                    FALSE, 0);
    CreateThread(NULL, 64 * 1024, watchdog_main, NULL, 0, NULL);
    const char *watch = getenv("PPL_WATCH");
    if (watch != NULL) {
        s_watch_addr = (DWORD64)strtoull(watch, NULL, 0);
        HANDLE t = CreateThread(NULL, 64 * 1024, arm_watch_main, NULL, 0, NULL);
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        fprintf(stderr, "[ppl:watch] hardware write watchpoint on 0x%llX\n", (unsigned long long)s_watch_addr);
    }
}
#else
#include <signal.h>
#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#define PPL_HAVE_EXECINFO 1
#endif

/* Stack addresses are stable (non-PIE executable at 0x10000000): they can
 * be symbolized offline (tools/pc_port/symbolize_crash.py, llvm-symbolizer). */
static void print_frames(const char *tag) {
#ifdef PPL_HAVE_EXECINFO
    void *frames[32];
    int n = backtrace(frames, 32);
    fprintf(stderr, "[ppl:%s] backtrace:\n", tag);
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "[ppl:%s]   #%02d %p\n", tag, i, frames[i]);
    }
#else
    (void)tag;
#endif
}

static void on_signal(int sig, siginfo_t *info, void *uctx) {
    (void)uctx;
    fprintf(stderr, "\n[ppl:crash] signal %d (address %p)\n", sig, info != NULL ? info->si_addr : NULL);
    print_frames("crash");
    fflush(stderr);
    _Exit(3);
}

void pc_watchdog_kick(void) {
}

void pc_log_backtrace(const char *tag) {
    fprintf(stderr, "[ppl:%s] game check failed\n", tag);
    print_frames(tag);
}

void pc_crash_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}
#endif
