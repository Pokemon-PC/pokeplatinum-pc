#include "hw_pc.h"

#include "platform_pc.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <errno.h>
#include <sys/mman.h>
#endif

typedef struct {
    uint32_t    base, end;
    const char *name;
    bool        mapped;
} Region;

static Region s_regions[] = {
    { PC_HW_MAIN_BASE, PC_HW_MAIN_END, "ITCM/main RAM/WRAM", false },
    { PC_HW_IO_BASE,   PC_HW_IO_END,   "I/O registers",      false },
    { PC_HW_PLTT_BASE, PC_HW_PLTT_END, "palettes",           false },
    { PC_HW_VRAM_BASE, PC_HW_VRAM_END, "VRAM",               false },
    { PC_HW_OAM_BASE,  PC_HW_OAM_END,  "OAM",                false },
};
#define REGION_COUNT (sizeof s_regions / sizeof s_regions[0])

#define PPL_HW_CHILD_ENV "PPL_HW_PRERESERVED"

static bool map_fixed(uint32_t base, uint32_t size) {
    void *want = (void *)(uintptr_t)base;
#if defined(_WIN32)
    /* Relaunched child: the parent already committed the region for us. */
    MEMORY_BASIC_INFORMATION mbi;
    if (GetEnvironmentVariableA(PPL_HW_CHILD_ENV, NULL, 0) != 0 &&
        VirtualQuery(want, &mbi, sizeof mbi) != 0 && mbi.State == MEM_COMMIT &&
        mbi.AllocationBase == want && mbi.Type == MEM_PRIVATE && mbi.RegionSize >= size) {
        return true;
    }
    void *got = VirtualAlloc(want, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (got != want) {
        if (got != NULL) {
            VirtualFree(got, 0, MEM_RELEASE);
        }
        return false;
    }
    return true;
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;    /* never clobber an existing mapping */
#endif
    void *got = mmap(want, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (got == MAP_FAILED) {
        return false;
    }
    if (got != want) {               /* kernel ignored the hint */
        munmap(got, size);
        return false;
    }
    return true;
#endif
}

/* Logs what already occupies [base, end) to explain a failed reservation. */
static void report_conflicts(uint32_t base, uint32_t end) {
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = base; a < end; a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (VirtualQuery((void *)a, &mbi, sizeof mbi) == 0) {
            break;
        }
        if (mbi.State != MEM_FREE) {
            char name[MAX_PATH] = "";
            if (mbi.Type == MEM_IMAGE) {
                GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, sizeof name);
            } else if (mbi.Type == MEM_MAPPED) {
                K32GetMappedFileNameA(GetCurrentProcess(), mbi.BaseAddress, name, sizeof name);
            }
            pc_warn("hw:   0x%08llX-0x%08llX in use (%s%s%s)", (unsigned long long)(uintptr_t)mbi.BaseAddress,
                    (unsigned long long)((uintptr_t)mbi.BaseAddress + mbi.RegionSize),
                    mbi.Type == MEM_IMAGE ? "image " : mbi.Type == MEM_MAPPED ? "mapped" : "private",
                    name[0] ? ": " : "", name);
        }
    }
#else
    (void)base;
    (void)end;
#endif
}

static void unmap_fixed(uint32_t base, uint32_t size) {
#if defined(_WIN32)
    (void)size;
    VirtualFree((void *)(uintptr_t)base, 0, MEM_RELEASE);
#else
    munmap((void *)(uintptr_t)base, size);
#endif
}

static int s_reserve_state;   /* 0 = not tried, 1 = ok, -1 = failed */
static int s_failed_region = -1;

/* Reserves every region; silent so it can run before main(). */
static void reserve_all(void) {
    if (s_reserve_state != 0) {
        return;
    }
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        Region *r = &s_regions[i];
        if (!map_fixed(r->base, r->end - r->base)) {
            s_failed_region = (int)i;
            for (unsigned j = 0; j < i; j++) {
                unmap_fixed(s_regions[j].base, s_regions[j].end - s_regions[j].base);
                s_regions[j].mapped = false;
            }
            s_reserve_state = -1;
            return;
        }
        r->mapped = true;
    }
    s_reserve_state = 1;
}

#if defined(_WIN32)
/*
 * On Windows, user32/gdi32 (pulled in by SDL) map shared sections and the
 * loader creates worker-thread stacks while static imports are initialised,
 * i.e. before any code of ours runs - and with a fixed low image base they
 * are placed bottom-up, right where the DS map must go.
 *
 * Fix: re-create this very process suspended. At that point only the
 * executable image and ntdll are mapped, so the parent can commit the DS
 * ranges inside the child (VirtualAllocEx) before resuming it, then relays
 * the child's exit code. Set PPL_NO_RELAUNCH=1 to disable (debugging).
 */
static void relaunch_with_prereserved_map(void) {
    if (GetEnvironmentVariableA(PPL_HW_CHILD_ENV, NULL, 0) != 0 ||
        GetEnvironmentVariableA("PPL_NO_RELAUNCH", NULL, 0) != 0) {
        return;
    }
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0) {
        return;
    }
    SetEnvironmentVariableA(PPL_HW_CHILD_ENV, "1");
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(exe, GetCommandLineW(), NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        SetEnvironmentVariableA(PPL_HW_CHILD_ENV, NULL);
        return;
    }
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        void *want = (void *)(uintptr_t)s_regions[i].base;
        SIZE_T size = s_regions[i].end - s_regions[i].base;
        if (VirtualAllocEx(pi.hProcess, want, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != want) {
            /* Give up on relaunching; the child is discarded and the
             * original process reports the conflict normally. */
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            SetEnvironmentVariableA(PPL_HW_CHILD_ENV, NULL);
            return;
        }
    }
    /* Tie the child's lifetime to ours: if this process is killed (console
     * closed, task manager...), the job closes and the child goes with it. */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job != NULL) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
        ZeroMemory(&lim, sizeof lim);
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(code);
}
#endif

#if defined(__GNUC__) || defined(__clang__)
/* Grab the DS address ranges before anything else (SDL threads, fonts,
 * file mappings...) can land on them: the host allocates bottom-up in the
 * low 4 GiB when the executable is linked at a low, fixed base. */
__attribute__((constructor(101))) static void reserve_before_main(void) {
    reserve_all();
#if defined(_WIN32)
    if (s_reserve_state < 0) {
        relaunch_with_prereserved_map();
    }
#endif
}
#endif

bool pc_hw_init(void) {
    reserve_all();
    if (s_reserve_state < 0) {
        const Region *r = &s_regions[s_failed_region];
        pc_warn("hw: cannot map %s at 0x%08X-0x%08X:", r->name, r->base, r->end);
        report_conflicts(r->base, r->end);
        return false;
    }
    pc_hw_reset();
    pc_log("hw: DS memory map reserved at its native addresses (0x%08X-0x%08X)",
           s_regions[0].base, s_regions[REGION_COUNT - 1].end);
    return true;
}

void pc_hw_reset(void) {
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        memset((void *)(uintptr_t)s_regions[i].base, 0, s_regions[i].end - s_regions[i].base);
    }
    /* Power-on state that the game observes. */
    pc_hw_set_pad(0, false);          /* no button pressed, lid open */
    pc_hw_set_vcount(0);
}

void pc_hw_shutdown(void) {
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        if (s_regions[i].mapped) {
            unmap_fixed(s_regions[i].base, s_regions[i].end - s_regions[i].base);
            s_regions[i].mapped = false;
        }
    }
    s_reserve_state = 0;
}

bool pc_hw_is_mapped(uint32_t address, uint32_t size) {
    for (unsigned i = 0; i < REGION_COUNT; i++) {
        const Region *r = &s_regions[i];
        if (r->mapped && address >= r->base && size <= r->end - address) {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------- math coprocessor (CP) */

#define REG_DIVCNT        0x04000280u
#define REG_DIV_NUMER     0x04000290u
#define REG_DIV_DENOM     0x04000298u
#define REG_DIV_RESULT    0x040002A0u
#define REG_DIVREM_RESULT 0x040002A8u
#define REG_SQRTCNT       0x040002B0u
#define REG_SQRT_RESULT   0x040002B4u
#define REG_SQRT_PARAM    0x040002B8u

static inline volatile int64_t *reg64(uint32_t a) { return (volatile int64_t *)(uintptr_t)a; }

/* Divider, as described by GBATEK: mode 0 = 32/32, 1 = 64/32, 2/3 = 64/64.
 * Division by zero: remainder = numerator, result = -1 (numerator >= 0) or
 * +1, and in 32-bit mode the upper half of the result is inverted. */
uint32_t pc_cp_div_update(uint32_t reg_address) {
    uint16_t cnt = *pc_hw_reg16(REG_DIVCNT);
    int64_t numer = *reg64(REG_DIV_NUMER);
    int64_t denom = *reg64(REG_DIV_DENOM);
    int mode = cnt & 3;
    int64_t n, d, q, r;

    if (mode == 0) {
        n = (int32_t)numer;
        d = (int32_t)denom;
    } else if (mode == 1) {
        n = numer;
        d = (int32_t)denom;
    } else {
        n = numer;
        d = denom;
    }

    if (denom == 0) {
        cnt |= 0x4000;           /* DIV0: the full 64-bit denominator is 0 */
    } else {
        cnt &= (uint16_t)~0x4000;
    }

    if (d == 0) {
        r = n;
        q = n >= 0 ? -1 : 1;
        if (mode == 0) {
            q = (int64_t)((uint64_t)q ^ 0xFFFFFFFF00000000ull);
        }
    } else if (n == INT64_MIN && d == -1) {
        q = INT64_MIN;           /* overflow wraps, as on hardware */
        r = 0;
    } else {
        q = n / d;               /* C division truncates toward zero, like the DS */
        r = n % d;
    }
    *reg64(REG_DIV_RESULT) = q;
    *reg64(REG_DIVREM_RESULT) = r;
    *pc_hw_reg16(REG_DIVCNT) = cnt & (uint16_t)~0x8000;   /* never busy */
    return reg_address;
}

/* Square root unit: SQRTCNT bit 0 selects a 64-bit (1) or 32-bit (0) input. */
uint32_t pc_cp_sqrt_update(uint32_t reg_address) {
    uint16_t cnt = *pc_hw_reg16(REG_SQRTCNT);
    uint64_t v = (cnt & 1) ? (uint64_t)*reg64(REG_SQRT_PARAM) : (uint32_t)*reg64(REG_SQRT_PARAM);
    uint64_t res = 0, bit = 1ull << 62;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0) {           /* exact integer square root */
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    *pc_hw_reg32(REG_SQRT_RESULT) = (uint32_t)res;
    *pc_hw_reg16(REG_SQRTCNT) = cnt & (uint16_t)~0x8000;
    return reg_address;
}

#define REG_KEYINPUT     0x04000130u
#define REG_VCOUNT       0x04000006u
#define REG_DISPSTAT     0x04000004u
#define HW_BUTTON_XY_BUF 0x027FFFA8u

void pc_hw_set_pad(uint16_t buttons, bool lid_closed) {
    /* KEYINPUT: bits 0-9 = A B Select Start Right Left Up Down R L, 0 = pressed. */
    *pc_hw_reg16(REG_KEYINPUT) = (uint16_t)(~buttons & 0x03FF);
    /* ARM7 shared word: bit 10 X, bit 11 Y, bit 13 debug (0 = pressed),
     * bit 15 = lid folded (1 = closed). */
    uint16_t xy = (uint16_t)(~buttons & 0x2C00);
    if (lid_closed) {
        xy |= 0x8000;
    }
    *pc_hw_reg16(HW_BUTTON_XY_BUF) = xy;
}

void pc_hw_set_vcount(uint16_t line) {
    *pc_hw_reg16(REG_VCOUNT) = line;
    uint16_t stat = *pc_hw_reg16(REG_DISPSTAT) & (uint16_t)~0x0001u;
    if (line >= 192 && line < 262) {
        stat |= 0x0001;   /* VBlank flag */
    }
    *pc_hw_reg16(REG_DISPSTAT) = stat;
}

/*
 * The game thread's stack below 4 GiB.
 *
 * DS code stores addresses in 32-bit words (the CARD backup API, u32 fields,
 * casts through u32...). Heaps live in the DS memory map and the executable
 * sits at 0x10000000, and on Windows the main thread's stack is low too, so
 * that works there. On Linux and macOS the main stack is near 0x7FFF...: any
 * stack buffer passed through such a word is truncated (crash in the Battle
 * Tower save on Linux). The game therefore runs on a stack mapped low, on the
 * same thread (SDL and Cocoa need the main thread).
 */
#if defined(_WIN32)
void pc_run_on_low_stack(void (*fn)(void *), void *arg) {
    fn(arg);
}
#else
#if defined(__APPLE__)
#define PPL_ASM_SYM(name) "_" name
#else
#define PPL_ASM_SYM(name) name
#endif
/* ppl_call_on_stack(stack_top, fn, arg): calls fn(arg) with rsp = stack_top. */
void ppl_call_on_stack(void *top, void (*fn)(void *), void *arg);
__asm__(".text\n"
        ".globl " PPL_ASM_SYM("ppl_call_on_stack") "\n"
        PPL_ASM_SYM("ppl_call_on_stack") ":\n"
        "    push %rbp\n"
        "    mov  %rsp, %rbp\n"
        "    mov  %rdi, %rsp\n"
        "    mov  %rdx, %rdi\n"
        "    call *%rsi\n"
        "    mov  %rbp, %rsp\n"
        "    pop  %rbp\n"
        "    ret\n");

#define PPL_LOW_STACK_SIZE (16u * 1024 * 1024)

void pc_run_on_low_stack(void (*fn)(void *), void *arg) {
    void *stack = MAP_FAILED;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_32BIT)
    stack = mmap(NULL, PPL_LOW_STACK_SIZE, PROT_READ | PROT_WRITE, flags | MAP_32BIT, -1, 0);
#endif
    /* Otherwise (macOS): try hints above the executable, below 4 GiB. */
    for (uintptr_t hint = 0x40000000u; stack == MAP_FAILED && hint < 0xF0000000u; hint += 0x10000000u) {
        void *got = mmap((void *)hint, PPL_LOW_STACK_SIZE, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (got == MAP_FAILED) {
            continue;
        }
        if ((uintptr_t)got + PPL_LOW_STACK_SIZE <= 0x100000000ull) {
            stack = got;
        } else {
            munmap(got, PPL_LOW_STACK_SIZE);
        }
    }
    if (stack == MAP_FAILED) {
        pc_warn("hw: no stack below 4 GiB available, running on the system stack");
        fn(arg);
        return;
    }
    pc_log("hw: game stack at %p-%p", stack, (void *)((char *)stack + PPL_LOW_STACK_SIZE));
    void *top = (char *)stack + PPL_LOW_STACK_SIZE - 64;   /* 16-byte aligned */
    ppl_call_on_stack(top, fn, arg);
}
#endif
