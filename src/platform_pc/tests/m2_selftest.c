/*
 * m2_selftest.c - milestone 2 acceptance test.
 *
 * Runs the game's *own* code (src/heap.c, src/filesystem.c, NitroSystem
 * expanded heaps, SDK inlines such as PAD_Read) on the PC Nitro layer and
 * checks the results against the HAL. No window is opened.
 *
 * Usage: ppl_m2_selftest [--data DIR] [--save FILE]
 * Exit code 0 when every check passes.
 */
#include "nitro_pc.h"
#include "ppu_pc.h"
#include "vram_pc.h"
#include "gpu3d_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heap.h"
#include "narc.h"

/* src/narc.c itself, for its path table (sNarcPaths is static). */
#include "../../narc.c"

/* No SDL include in this file: with the game's include paths the SDK's own
 * math.h etc. shadow the host headers SDL relies on. Game-side code reaches
 * the HAL only through platform_pc.h. */

/* Declared by the game itself (src/touchpad.c), not by the SDK headers. */
void TP_RequestAutoSamplingStartAsync(u16 vcount, u16 frequency, TPData *samplingBufs, u16 bufSize);
void TP_RequestAutoSamplingStopAsync(void);
u16 TP_GetLatestIndexInAuto(void);
RTCResult RTC_GetDateTime(RTCDate *date, RTCTime *time);


static int s_failures;
static int s_checks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        s_checks++;                                         \
        if (!(cond)) {                                      \
            s_failures++;                                   \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                   \
            fputc('\n', stderr);                            \
        }                                                   \
    } while (0)

/* --- symbols heap.c needs from parts of the game not built yet ---------- */
void ErrorHandling_AssertFail(void) {
    s_failures++;
    fprintf(stderr, "  FAIL: GF_ASSERT triggered inside game code\n");
}
BOOL CommManager_IsInitialized(void) { return FALSE; }
void ErrorMessageReset_PrintErrorAndReset(void) {}

static u32 s_global_probe = 0x12345678;   /* static data must be < 4 GiB */

static void test_memory_map(void) {
    printf("[memory map]\n");
    CHECK(pc_hw_is_mapped(HW_IOREG, 0x1000), "I/O registers not mapped");
    CHECK(pc_hw_is_mapped(HW_BG_VRAM, HW_BG_VRAM_SIZE), "BG VRAM not mapped");
    CHECK(pc_hw_is_mapped(HW_DTCM, HW_DTCM_SIZE), "DTCM not mapped");
    reg_GX_DISPCNT = 0x00012345;
    CHECK(*(vu32 *)0x04000000 == 0x00012345, "register write not visible at its DS address");
    CHECK((uintptr_t)&s_global_probe <= 0xFFFFFFFFu,
          "static data at %p is above 4 GiB (image base not applied)", (void *)&s_global_probe);
    printf("  static data at %p, code at %p\n", (void *)&s_global_probe, (void *)test_memory_map);
}

static const HeapParam kHeapSpec[] = {   /* same as src/system.c */
    { HEAP_SIZE_SYSTEM, OS_ARENA_MAIN },
    { HEAP_SIZE_SAVE, OS_ARENA_MAIN },
    { HEAP_SIZE_DEBUG, OS_ARENA_MAIN },
    { HEAP_SIZE_APPLICATION, OS_ARENA_MAIN },
};

static void test_heaps(void) {
    printf("[heaps: src/heap.c + NNS_FndExpHeap]\n");
    Heap_InitSystem(kHeapSpec, NELEMS(kHeapSpec), HEAP_ID_MAX, 0);
    u32 free_before = HeapExp_FndGetTotalFreeSize(HEAP_ID_APPLICATION);
    u8 *a = Heap_Alloc(HEAP_ID_APPLICATION, 1000);
    u8 *b = Heap_AllocAtEnd(HEAP_ID_APPLICATION, 5000);
    CHECK(a != NULL && b != NULL, "Heap_Alloc failed");
    CHECK((uintptr_t)a >= PC_HW_MAIN_MEM && (uintptr_t)a < PC_HW_ARENA_END, "heap block %p outside the emulated main RAM", (void *)a);
    CHECK(b > a, "AllocAtEnd should come from the top of the heap");
    memset(a, 0xAA, 1000);
    memset(b, 0x55, 5000);
    Heap_Free(a);
    Heap_Free(b);
    CHECK(HeapExp_FndGetTotalFreeSize(HEAP_ID_APPLICATION) == free_before, "heap leaked after free");
    CHECK(Heap_Create(HEAP_ID_APPLICATION, HEAP_ID_FIELD1, 0x10000), "Heap_Create(child) failed");
    void *c = Heap_Alloc(HEAP_ID_FIELD1, 0x8000);
    CHECK(c != NULL, "child heap alloc failed");
    Heap_Free(c);
    Heap_Destroy(HEAP_ID_FIELD1);
    printf("  HEAP_ID_APPLICATION free: %u bytes\n", (unsigned)free_before);
}

static void test_narcs(void) {
    printf("[NARC: src/narc.c over FS_* adapters]\n");
    int present = 0, compared = 0, total = (int)NELEMS(sNarcPaths);
    for (int id = 0; id < total; id++) {
        const char *path = sNarcPaths[id];
        PcNarc ref;
        if (path == NULL || !pc_narc_open(&ref, path)) {
            continue;   /* not a NARC, or not on disk */
        }
        present++;
        NARC *narc = NARC_ctor((enum NarcID)id, HEAP_ID_APPLICATION);
        CHECK(narc != NULL, "NARC_ctor(%s) failed", path);
        if (narc != NULL) {
            CHECK(NARC_GetFileCount(narc) == ref.count, "%s: %u members, expected %u", path,
                  (unsigned)NARC_GetFileCount(narc), (unsigned)ref.count);
            NARC_dtor(narc);
        }
        /* Compare member 0 byte for byte with the HAL's independent reader. */
        uint32_t ref_size = 0;
        void *expect = pc_narc_load_member(&ref, 0, &ref_size);
        if (expect != NULL && ref_size > 0) {
            u32 size = NARC_GetMemberSizeByIndexPair((enum NarcID)id, 0);
            void *got = NARC_AllocAndReadWholeMemberByIndexPair((enum NarcID)id, 0, HEAP_ID_APPLICATION);
            CHECK(size == ref_size, "%s[0]: size %u, expected %u", path, (unsigned)size, (unsigned)ref_size);
            CHECK(got != NULL && memcmp(got, expect, ref_size) == 0, "%s[0]: content differs", path);
            if (got != NULL) {
                Heap_Free(got);
            }
            compared++;
        }
        pc_os_free(expect);
        pc_narc_close(&ref);
    }
    printf("  %d/%d NARCs found, %d compared byte for byte\n", present, total, compared);
    CHECK(present > total / 2, "only %d NARCs found under %s", present, pc_fs_root());
}

static void test_pad(void) {
    printf("[PAD_Read / PAD_DetectFold via virtual registers]\n");
    pc_hw_set_pad(PAD_BUTTON_A | PAD_BUTTON_X | PAD_KEY_UP, FALSE);
    CHECK(PAD_Read() == (PAD_BUTTON_A | PAD_BUTTON_X | PAD_KEY_UP), "PAD_Read = 0x%04X", PAD_Read());
    CHECK(!PAD_DetectFold(), "lid reported closed");
    pc_hw_set_pad(0, TRUE);
    CHECK(PAD_Read() == 0, "PAD_Read = 0x%04X with nothing pressed", PAD_Read());
    CHECK(PAD_DetectFold(), "lid reported open");
    pc_hw_set_pad(0, FALSE);
}

static void test_card(void) {
    printf("[CARD backup -> save file]\n");
    CHECK(CARD_IdentifyBackup(CARD_BACKUP_TYPE_FLASH_4MBITS), "CARD_IdentifyBackup failed");
    u8 *out = Heap_Alloc(HEAP_ID_APPLICATION, 0x200);
    u8 *in = Heap_Alloc(HEAP_ID_APPLICATION, 0x200);
    for (int i = 0; i < 0x200; i++) {
        out[i] = (u8)(i * 7 + 3);
    }
    CHECK(CARD_WriteAndVerifyBackup(0x40000, out, 0x200), "write+verify failed");
    memset(in, 0, 0x200);
    CHECK(CARD_ReadBackup(0x40000, in, 0x200), "read failed");
    CHECK(memcmp(in, out, 0x200) == 0, "read-back differs");
    CHECK(CARD_GetResultCode() == CARD_RESULT_SUCCESS, "result code %d", (int)CARD_GetResultCode());
    Heap_Free(out);
    Heap_Free(in);
}

static void test_rtc(void) {
    printf("[RTC]\n");
    RTCDate d;
    RTCTime t;
    CHECK(RTC_GetDateTime(&d, &t) == RTC_RESULT_SUCCESS, "RTC_GetDateTime failed");
    CHECK(d.month >= 1 && d.month <= 12 && d.day >= 1 && d.day <= 31 && t.hour < 24, "bad date/time");
    s64 sec = RTC_ConvertDateTimeToSecond(&d, &t);
    RTCDate d2;
    RTCTime t2;
    RTC_ConvertSecondToDateTime(&d2, &t2, sec);
    CHECK(d2.year == d.year && d2.month == d.month && d2.day == d.day && d2.week == d.week &&
          t2.hour == t.hour && t2.minute == t.minute && t2.second == t.second,
          "SDK RTC conversion round-trip mismatch");
    printf("  20%02u-%02u-%02u %02u:%02u:%02u (weekday %d)\n", (unsigned)d.year, (unsigned)d.month,
           (unsigned)d.day, (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second, (int)d.week);
}

static int s_vblank_irqs;
static void on_vblank(void) {
    s_vblank_irqs++;
}

static void test_vblank_irq(void) {
    printf("[VBlank IRQ + OS_WaitIrq pacing]\n");
    OS_SetIrqFunction(OS_IE_V_BLANK, on_vblank);
    OS_EnableIrqMask(OS_IE_V_BLANK);
    reg_OS_IME = 1;
    uint64_t t0 = pc_os_ms();
    for (int i = 0; i < 30; i++) {
        OS_WaitIrq(TRUE, OS_IE_V_BLANK);
    }
    uint64_t ms = pc_os_ms() - t0;
    CHECK(s_vblank_irqs == 30, "%d VBlank handlers ran, expected 30", s_vblank_irqs);
    CHECK(ms >= 450 && ms <= 650, "30 frames took %llu ms (expected ~501)", (unsigned long long)ms);
    printf("  30 frames in %llu ms\n", (unsigned long long)ms);
    {
        /* Diagnostic: the per-frame work without pacing (slow hosts). */
        int prev = pc_os_speed();
        pc_os_set_speed(0);
        uint64_t w0 = pc_os_ms();
        for (int i = 0; i < 30; i++) {
            OS_WaitIrq(TRUE, OS_IE_V_BLANK);
        }
        printf("  info: 30 unpaced frames took %llu ms\n", (unsigned long long)(pc_os_ms() - w0));
        pc_os_set_speed(prev);
        s_vblank_irqs -= 30;
    }
    OSIntrMode m = OS_DisableInterrupts();
    OS_WaitIrq(TRUE, OS_IE_V_BLANK);
    OS_RestoreInterrupts(m);
    CHECK(s_vblank_irqs == 30, "handler ran while interrupts were disabled");
}

static void test_touch(void) {
    printf("[TP auto sampling]\n");
    static TPData buf[9];
    TP_RequestAutoSamplingStartAsync(0, 4, buf, NELEMS(buf));
    OS_WaitIrq(TRUE, OS_IE_V_BLANK);
    u16 idx = TP_GetLatestIndexInAuto();
    CHECK(idx == 4, "latest index %u after one frame at 4 samples/frame", idx);
    CHECK(buf[idx].touch == TP_TOUCH_OFF && buf[idx].validity == TP_VALIDITY_VALID, "unexpected sample");
    TP_RequestAutoSamplingStopAsync();
}

/* Display capture: blend of engine A's picture (blue backdrop) and VRAM
 * bank D (red) at 8/16 + 8/16, written to bank C (ppu_pc.c). */
static void test_display_capture(void) {
    printf("[display capture]\n");
    *(vu8 *)0x04000242 = 0x80;   /* VRAMCNT_C / _D: enabled, LCDC */
    *(vu8 *)0x04000243 = 0x80;
    pc_vram_sync();
    u16 *bank_c = (u16 *)0x06840000, *bank_d = (u16 *)0x06860000;
    for (int i = 0; i < 0x10000; i++) {
        bank_c[i] = 0;
        bank_d[i] = 0x801F;   /* red, alpha bit set */
    }
    *(vu16 *)0x04000304 = 0x8003;          /* POWCNT1: LCDs + engine A on */
    *(vu16 *)0x05000000 = 0x7C00;          /* engine A backdrop: blue */
    reg_GX_DISPCNT = (1u << 16) | (3u << 18);   /* graphics display, VRAM block D */
    reg_GX_DISPCAPCNT = (1u << 31) | (2u << 29) | (3u << 20) | (2u << 16) | (8u << 8) | 8u;
    pc_ppu_render_frame();
    CHECK(bank_c[0] == 0xC010 && bank_c[256 * 192 - 1] == 0xC010, "captured pixels %04X / %04X, expected C010",
          bank_c[0], bank_c[256 * 192 - 1]);
    CHECK(bank_c[256 * 192] == 0, "capture wrote past 256x192");
    CHECK((reg_GX_DISPCAPCNT >> 31) == 0, "DISPCAPCNT enable bit not cleared after the frame");
    pc_ppu_render_frame();   /* disabled now: bank C untouched */
    CHECK(bank_c[0] == 0xC010, "capture ran again while disabled");
    reg_GX_DISPCNT = 0;
    *(vu8 *)0x04000242 = 0;
    *(vu8 *)0x04000243 = 0;
    pc_vram_sync();
}

/* 3D shadow polygons (gpu3d_pc.c): a red floor, a shadow mask behind it in
 * the middle of the screen, then a black half-transparent shadow over the
 * whole screen: only the masked middle gets darker. */
static void g3_quad(u32 attr, u16 color, s16 x0, s16 y0, s16 x1, s16 y1, s16 z) {
    pc_g3_write(0x040004A4, attr);      /* POLYGON_ATTR */
    pc_g3_write(0x04000500, 1);         /* BEGIN_VTXS: quads */
    pc_g3_write(0x04000480, color);     /* COLOR */
    const s16 v[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    for (int i = 0; i < 4; i++) {
        pc_g3_write(0x0400048C, (u16)v[i][0] | ((u32)(u16)v[i][1] << 16));   /* VTX_16 */
        pc_g3_write(0x0400048C, (u16)z);
    }
    pc_g3_write(0x04000504, 0);         /* END_VTXS */
}

static void test_shadow_polygons(void) {
    printf("[3D shadow polygons]\n");
    *(vu16 *)0x04000060 = 0x0008;       /* DISP3DCNT: alpha blending */
    *(vu32 *)0x04000350 = 0;            /* clear colour: transparent */
    *(vu16 *)0x04000354 = 0x7FFF;       /* clear depth: farthest */
    for (u32 mode = 0; mode <= 2; mode += 2) {
        pc_g3_write(0x04000440, mode);  /* MTX_MODE: projection, then position */
        pc_g3_write(0x04000454, 0);     /* MTX_IDENTITY */
    }
    const u32 both_sides = 0xC0, opaque = 31u << 16;
    g3_quad(both_sides | opaque | (1u << 24), 0x001F, -0x1000, -0x1000, 0x1000, 0x1000, 0);   /* floor */
    g3_quad(both_sides | (3u << 4) | opaque, 0, -0x800, -0x800, 0x800, 0x800, 0x800);         /* mask, behind */
    g3_quad(both_sides | (3u << 4) | (16u << 16) | (2u << 24), 0, -0x1000, -0x1000, 0x1000, 0x1000, -0x800);
    pc_g3_write(0x04000540, 0);         /* SWAP_BUFFERS */
    pc_g3_flush();
    pc_gpu3d_render();
    u16 corner = pc_gpu3d_line_color(10)[10], middle = pc_gpu3d_line_color(96)[128];
    CHECK(corner == 0x001F && pc_gpu3d_line_alpha(10)[10] == 31, "outside the mask: %04X, expected the red floor", corner);
    CHECK((middle & 31) > 0 && (middle & 31) < 20 && (middle >> 5) == 0, "inside the mask: %04X, expected a darker red",
          middle);
    *(vu16 *)0x04000060 = 0;
}

int main(int argc, char **argv) {
    const char *data = NULL;   /* NULL: standard layers for `lang` */
    const char *lang = PPL_DEFAULT_LANG;
    const char *save = "m2_selftest_save.bin";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--data")) data = argv[i + 1];
        else if (!strcmp(argv[i], "--lang")) lang = argv[i + 1];
        else if (!strcmp(argv[i], "--save")) save = argv[i + 1];
    }
    if (!pc_os_init()) {
        fprintf(stderr, "timer init failed\n");
        return 2;
    }
    if (!pc_nitro_init(data, lang, save)) {
        fprintf(stderr, "Nitro layer init failed\n");
        return 2;
    }
    test_memory_map();
    test_heaps();
    test_narcs();
    test_pad();
    test_card();
    test_rtc();
    test_vblank_irq();
    test_touch();
    test_display_capture();
    test_shadow_polygons();
    pc_nitro_shutdown();
    pc_os_shutdown();
    printf("\n%d checks, %d failed\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
