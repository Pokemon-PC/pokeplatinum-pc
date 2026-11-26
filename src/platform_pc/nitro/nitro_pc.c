/* nitro_pc.c - Nitro layer bring-up and per-frame update. */
#include "nitro_pc.h"

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "pc_overlay_sections.h"
#include "ppu_pc.h"
#include "gpu3d_pc.h"
#include "snd_pc.h"
#include "vram_pc.h"

#include <stdio.h>

/* game functions used by test hooks */
struct SaveData;
/* weak: the Nitro-layer self test links without the game */
__attribute__((weak)) struct SaveData *SaveData_Ptr(void);
__attribute__((weak)) int SaveData_Save(struct SaveData *saveData);
struct FieldSystem;
__attribute__((weak)) struct FieldSystem *pc_test_field_system(void);
__attribute__((weak)) BOOL pc_test_player_pos(int *mapId, int *x, int *z);
__attribute__((weak)) BOOL pc_heap_check(void);
__attribute__((weak)) BOOL pc_test_map_dump(int rx, int rz);
__attribute__((weak)) BOOL pc_test_field_idle(void);
__attribute__((weak)) void pc_test_set_party_level(int level, BOOL almostNext);
__attribute__((weak)) void pc_test_warp(u32 mapId, u32 x, u32 z);
__attribute__((weak)) void pc_test_trainer_battle(u32 trainerId);
__attribute__((weak)) void pc_test_egg_ready(void);
__attribute__((weak)) void pc_test_give_item(u16 item, u16 qty);
__attribute__((weak)) void pc_test_set_badges(int n);
__attribute__((weak)) void pc_test_set_move(int slot, u16 move);
__attribute__((weak)) void pc_test_frame(u32 frame);   /* game_test_hooks_pc.c */
int gPcHeapCheck = -1;   /* PPL_HEAPCHECK: -1 unread, 0 off, 1 on, 2 reported */
__attribute__((weak)) BOOL FieldSystem_Save(struct FieldSystem *fieldSystem);

static BOOL s_render_pending;   /* a V-Blank happened, frame not drawn yet */

/*
 * Game language (GAME_LANGUAGE / gGameLanguage, include/config.h). On the DS
 * it is fixed per cartridge; on PC it follows --lang so that the code agrees
 * with the data files (Pokémon language tags, Pokédex, Easy Chat...).
 * Not part of the game's variables: survives a soft reset.
 */
unsigned char gPcGameLanguage = 3;   /* LANGUAGE_FRENCH */

typedef struct {
    const char *code;     /* --lang value */
    unsigned char id;     /* LANGUAGE_* (include/config.h) */
    char rom_region;      /* 4th letter of the cartridge game code */
} PcLanguage;

static const PcLanguage s_languages[] = {
    { "ja", 1, 'J' },
    { "en", 2, 'E' },
    { "fr", 3, 'F' },
    { "it", 4, 'I' },
    { "de", 5, 'D' },
    { "es", 7, 'S' },
    { "ko", 8, 'K' },
};

static const PcLanguage *s_language = &s_languages[2];

static void pc_nitro_set_language(const char *lang) {
    if (lang == NULL) {
        return;
    }
    for (size_t i = 0; i < sizeof s_languages / sizeof s_languages[0]; i++) {
        if (strcmp(lang, s_languages[i].code) == 0) {
            s_language = &s_languages[i];
            gPcGameLanguage = s_languages[i].id;
            return;
        }
    }
    pc_warn("nitro: unknown language '%s', using '%s'", lang, s_language->code);
}

/*
 * What the DS boot loader leaves in memory before the game starts: a copy
 * of the cartridge header at HW_ROM_HEADER_BUF (0x027FFE00). The game copies
 * it to its mirror and checks the maker code (src/system_card.c).
 */
static void pc_nitro_boot_rom_header(void) {
    u8 *h = (u8 *)HW_ROM_HEADER_BUF;
    memset(h, 0, 0x160);
    memcpy(h + 0x00, "POKEMON PL\0\0", 12);           /* title */
    memcpy(h + 0x0C, "CPUE", 4);                       /* game code */
    h[0x0F] = (u8)s_language->rom_region;              /* CPUE, CPUF... */
    memcpy(h + 0x10, "01", 2);                         /* maker code: Nintendo */
}

/* ------------------------------------------------------------ soft reset */

/*
 * The game variables (".data$main$1", see tools/pc_port/gen_overlay_sections.py)
 * are snapshotted before any game or Nitro-layer code touches them; a reset
 * restores them, so the game restarts exactly as after power-on, minus the
 * cartridge (save memory) and the host window, which are not game state.
 */
static char    *s_main_snapshot;
static jmp_buf *s_restart;
static int      s_reset_count;

static void main_snapshot(void) {
#ifdef PPL_OVERLAY_RESET
    size_t n = (size_t)(gPcMainData.end - gPcMainData.begin);
    s_main_snapshot = malloc(n);
    if (s_main_snapshot == NULL) {
        pc_fatal("nitro: out of memory for the game-variable snapshot");
    }
    memcpy(s_main_snapshot, gPcMainData.begin, n);
    pc_log("nitro: %u bytes of game variables saved for soft reset", (unsigned)n);
#endif
}

/* What OS_Init + the DS boot code set up, done again after a reset. */
static void boot_nitro(void) {
    pc_gpu3d_reset();
    pc_nitro_boot_rom_header();
    OS_Init();
    FS_Init(FS_DMA_NOT_USE);
    TP_Init();
}

void pc_nitro_run(void (*entry)(void)) {
    jmp_buf restart;
    s_restart = &restart;
    if (setjmp(restart) != 0) {
        pc_log("nitro: soft reset #%d, parameter %u: restarting the game", s_reset_count,
               (unsigned)*(u32 *)HW_RESET_PARAMETER_BUF);
    }
    entry();
    pc_fatal("nitro: the game's entry point returned");
}

void pc_nitro_soft_reset(u32 parameter) {
#ifndef PPL_OVERLAY_RESET
    pc_fatal("OS_ResetSystem(%u): soft reset needs the Clang build", (unsigned)parameter);
#else
    if (s_restart == NULL) {
        pc_fatal("OS_ResetSystem(%u) outside pc_nitro_run", (unsigned)parameter);
    }
    s_reset_count++;
    pc_save_flush();
    pc_vram_reset();
    pc_gpu3d_reset();
    pc_snd_reset();
    pc_wm_reset();
    s_render_pending = FALSE;
    memcpy(gPcMainData.begin, s_main_snapshot, (size_t)(gPcMainData.end - gPcMainData.begin));
    pc_nitro_fs_soft_reset();
    pc_hw_reset();
    /* The one value the DS keeps across a reset (src/main.c reads it). */
    *(u32 *)HW_RESET_PARAMETER_BUF = parameter;
    boot_nitro();
    longjmp(*s_restart, 1);
#endif
}

BOOL pc_nitro_init(const char *data_dir, const char *lang, const char *save_path) {
    main_snapshot();
    if (!pc_hw_init()) {
        return FALSE;
    }
    pc_nitro_set_language(lang != NULL ? lang : "fr");
    pc_log("nitro: language %s (%u)", s_language->code, (unsigned)gPcGameLanguage);
    BOOL fs_ok = data_dir != NULL ? pc_fs_init(data_dir) : pc_fs_setup_layers(pc_install_base_dir(), lang);
    if (!fs_ok) {
        pc_warn("nitro: no usable data directory (%s)", data_dir != NULL ? data_dir : pc_install_base_dir());
        return FALSE;
    }
    for (int i = 0; i < pc_fs_root_count(); i++) {
        pc_log("fs: layer %d: %s", i, pc_fs_root_at(i));
    }
    if (!pc_save_init(save_path)) {
        return FALSE;
    }
    boot_nitro();
    return TRUE;
}

void pc_nitro_shutdown(void) {
    pc_save_shutdown();
    pc_fs_shutdown();
    pc_hw_shutdown();
}

static BOOL s_has_window;
static u32  s_frame_no;

/* PPL_DUMP_FRAMES=N[,M...]: saves both screens of those frames as
 * frame_NNNNNN.png (top above bottom) in the working directory. */
#define PC_DUMP_MAX 64
static u32 s_dump_frames[PC_DUMP_MAX];

static void read_dump_frames(void) {
    const char *r = getenv("PPL_DUMP_FRAMES");
    for (int i = 0; r != NULL && *r != '\0' && i < PC_DUMP_MAX; i++) {
        char *next;
        s_dump_frames[i] = (u32)strtoul(r, &next, 10);
        r = *next == ',' ? next + 1 : NULL;
    }
}

static void dump_frame(u32 frame) {
    for (int i = 0; i < PC_DUMP_MAX; i++) {
        if (s_dump_frames[i] != 0 && s_dump_frames[i] == frame) {
            static uint32_t both[PC_SCREEN_W * PC_SCREEN_H * 2];
            memcpy(both, pc_gfx_framebuffer(PC_SCREEN_TOP), sizeof both / 2);
            memcpy(both + PC_SCREEN_W * PC_SCREEN_H, pc_gfx_framebuffer(PC_SCREEN_BOTTOM), sizeof both / 2);
            char name[64];
            snprintf(name, sizeof name, "frame_%06u.png", (unsigned)frame);
            if (pc_write_png(name, both, PC_SCREEN_W, PC_SCREEN_H * 2)) {
                pc_log("dump: %s", name);
            }
            /* Raw video state next to it, for offline analysis:
             * I/O 0x04000000 (4 KiB) + 0x04001000 (4 KiB), palettes (2 KiB),
             * OAM (2 KiB), VRAM 0x06000000-0x068A4000, then the texture image
             * (512 KiB) and texture palette (96 KiB) slots. */
            if (getenv("PPL_DUMP_VIDEO") != NULL) {
                snprintf(name, sizeof name, "frame_%06u.video", (unsigned)frame);
                FILE *f = fopen(name, "wb");
                if (f != NULL) {
                    fwrite((void *)(uintptr_t)0x04000000, 1, 0x2000, f);
                    fwrite((void *)(uintptr_t)0x05000000, 1, 0x800, f);
                    fwrite((void *)(uintptr_t)0x07000000, 1, 0x800, f);
                    fwrite((void *)(uintptr_t)0x06000000, 1, 0x8A4000, f);
                    static u8 tex[0x80000], texpal[0x18000];   /* 3D slots, hidden from the CPU */
                    pc_vram_texture_image(tex);
                    pc_vram_texture_palette(texpal);
                    fwrite(tex, 1, sizeof tex, f);
                    fwrite(texpal, 1, sizeof texpal, f);
                    fclose(f);
                }
            }
        }
    }
}

/*
 * Power management (PM_*). The DS turns the screens off and sleeps when the
 * lid closes (src/main.c HandleDSLidAction); here the lid is the F10 hotkey.
 */
static PMBackLightSwitch s_backlight[2] = { PM_BACKLIGHT_ON, PM_BACKLIGHT_ON };

u32 PM_GetBackLight(PMBackLightSwitch *top, PMBackLightSwitch *bottom) {
    if (top != NULL) {
        *top = s_backlight[0];
    }
    if (bottom != NULL) {
        *bottom = s_backlight[1];
    }
    return PM_RESULT_SUCCESS;
}

u32 PM_SetBackLight(PMLCDTarget target, PMBackLightSwitch status) {
    if (target == PM_LCD_TOP || target == PM_LCD_ALL) {
        s_backlight[0] = status;
    }
    if (target == PM_LCD_BOTTOM || target == PM_LCD_ALL) {
        s_backlight[1] = status;
    }
    return 0;   /* PM_RESULT_SUCCESS */
}

u32 PM_ForceToPowerOff(void) {
    pc_log("PM_ForceToPowerOff: exiting");
    pc_save_flush();
    exit(0);
}

static void blank_screens(void) {
    memset(pc_gfx_framebuffer(PC_SCREEN_TOP), 0, PC_SCREEN_W * PC_SCREEN_H * 4);
    memset(pc_gfx_framebuffer(PC_SCREEN_BOTTOM), 0, PC_SCREEN_W * PC_SCREEN_H * 4);
}

/* Sleeps until the lid opens: black screens, no sound, the game is frozen
 * (no V-Blank interrupts) but the window keeps answering. */
void PM_GoSleepMode(PMWakeUpTrigger trigger, PMLogic logic, u16 keyPattern) {
    (void)trigger;
    (void)logic;
    (void)keyPattern;
    if (!s_has_window) {
        return;   /* headless: the lid cannot be closed */
    }
    pc_log("lid closed: sleeping");
    int speed = pc_os_speed();
    pc_os_set_speed(1);   /* do not spin at 100% CPU while asleep */
    pc_audio_pause(true);
    while (pc_input_lid_closed()) {
        if (!pc_input_poll()) {
            pc_log("window closed: exiting");
            pc_save_flush();
            exit(0);
        }
        blank_screens();
        pc_gfx_present();
        pc_watchdog_kick();
        pc_os_wait_vblank();
    }
    pc_hw_set_pad(pc_input_buttons(), FALSE);
    pc_audio_pause(false);
    if (pc_os_speed() == 1) {
        pc_os_set_speed(speed);   /* unless fast-forward was toggled meanwhile */
    }
    pc_log("lid opened: waking up");
}

static void render_frame(u32 frame) {
    s_render_pending = FALSE;
    pc_ppu_render_frame();
    if (s_backlight[0] == PM_BACKLIGHT_OFF) {
        memset(pc_gfx_framebuffer(PC_SCREEN_TOP), 0, PC_SCREEN_W * PC_SCREEN_H * 4);
    }
    if (s_backlight[1] == PM_BACKLIGHT_OFF) {
        memset(pc_gfx_framebuffer(PC_SCREEN_BOTTOM), 0, PC_SCREEN_W * PC_SCREEN_H * 4);
    }
    pc_hw_set_vcount(0);
    dump_frame(frame);
    /* While fast-forwarding, only some frames reach the window: the screen
     * keeps a normal refresh rate while game time runs faster. */
    int speed = pc_os_speed();
    int every = speed == 0 ? 8 : speed;
    if (s_has_window && (every <= 1 || frame % (u32)every == 0)) {
        pc_gfx_present();
    }
}

void pc_nitro_set_window(BOOL has_window) {
    s_has_window = has_window;
}

/*
 * Test hooks, read from the environment once:
 *   PPL_EXIT_AFTER=N         quit cleanly (save flushed) after N frames
 *   PPL_TEST_RESET_AT=N[,M]  hold L+R+Start+Select (the game's soft-reset
 *                            combo) for one frame at frame N, and M...
 *   PPL_TEST_KEYS=N:KEYS[*D][,...]  hold KEYS for D frames (default 6)
 *                            from frame N; KEYS are joined with '+': A B X Y
 *                            L R START SELECT UP DOWN LEFT RIGHT
 *                            (e.g. "4300:START,4500:A,4600:UP*32")
 *   PPL_TEST_POS=N           log the map and player tile every N frames
 *   PPL_TEST_MAP=N[,M]       ASCII map around the player at those frames
 *   PPL_TEST_WARP=MAP:X:Z[:N]  warp there (not before frame N)
 *   PPL_TEST_GIVE_ITEMS=ID[:Q],...  items added to the bag; PPL_TEST_EGG_READY=1
 *   PPL_TEST_HOUR=H (rtc_nitro_pc.c), PPL_TEST_BADGES=N, PPL_TEST_MOVES=SLOT:MOVE,...
 *   PPL_TEST_PARTY_LEVEL=N[+]  party set to level N ("+": 1 exp point
 *                            before N+1, to test level-up/evolution)
 *   PPL_TEST_NO_ENCOUNTERS=1 / PPL_TEST_NO_TRAINERS=1  (field code)
 *   PPL_TEST_TALK=1          after the last warp, face north and press A
 *   PPL_TEST_MASH=P[:FROM]   press A for 2 frames every P frames while the
 *                            player is not free (battles, dialogues, menus
 *                            opened by scripts), from frame FROM
 *   PPL_TEST_TOUCH=N:X:Y[>X2:Y2][,...]  touch the bottom screen at (X, Y)
 *                            for 6 frames from frame N (">": drag to X2,Y2)
 */
#define PC_TEST_RESETS 4
#define PC_TEST_KEYS   1024
static u32 s_exit_after;
static u32 s_test_reset_at[PC_TEST_RESETS];
static struct { u32 frame; u16 keys; u16 len; } s_test_keys[PC_TEST_KEYS];
static u32 s_test_pos_every;
static u32 s_test_mash, s_test_mash_from;   /* PPL_TEST_MASH */
static u32 s_test_talk_at;                   /* PPL_TEST_TALK */
static struct { u32 frame; u16 x, y, x2, y2; } s_test_touch[PC_TEST_KEYS];   /* x2/y2: drag end */

static u16 parse_keys(const char *s, const char **end) {
    static const struct { const char *name; u16 bit; } names[] = {
        { "START", PC_PAD_START }, { "SELECT", PC_PAD_SELECT }, { "UP", PC_PAD_UP },
        { "DOWN", PC_PAD_DOWN }, { "LEFT", PC_PAD_LEFT }, { "RIGHT", PC_PAD_RIGHT },
        { "A", PC_PAD_A }, { "B", PC_PAD_B }, { "X", PC_PAD_X }, { "Y", PC_PAD_Y },
        { "L", PC_PAD_L }, { "R", PC_PAD_R },
    };
    u16 keys = 0;
    for (;;) {
        size_t i, n = 0;
        for (i = 0; i < sizeof names / sizeof names[0]; i++) {
            n = strlen(names[i].name);
            if (strncmp(s, names[i].name, n) == 0) {
                break;
            }
        }
        if (i == sizeof names / sizeof names[0]) {
            break;
        }
        keys |= names[i].bit;
        s += n;
        if (*s != '+') {
            break;
        }
        s++;
    }
    *end = s;
    return keys;
}

static void read_test_hooks(void) {
    const char *e = getenv("PPL_EXIT_AFTER");
    s_exit_after = e != NULL ? (u32)strtoul(e, NULL, 10) : 0;
    const char *r = getenv("PPL_TEST_RESET_AT");
    for (int i = 0; r != NULL && *r != '\0' && i < PC_TEST_RESETS; i++) {
        char *next;
        s_test_reset_at[i] = (u32)strtoul(r, &next, 10);
        r = *next == ',' ? next + 1 : NULL;
    }
    const char *k = getenv("PPL_TEST_KEYS");
    for (int i = 0; k != NULL && *k != '\0' && i < PC_TEST_KEYS; i++) {
        char *next;
        s_test_keys[i].frame = (u32)strtoul(k, &next, 10);
        if (*next != ':') {
            break;
        }
        const char *end;
        s_test_keys[i].keys = parse_keys(next + 1, &end);
        s_test_keys[i].len = 6;
        if (*end == '*') {
            char *e2;
            s_test_keys[i].len = (u16)strtoul(end + 1, &e2, 10);
            end = e2;
        }
        k = *end == ',' ? end + 1 : NULL;
    }
    const char *m = getenv("PPL_TEST_MASH");
    if (m != NULL) {
        char *n;
        s_test_mash = (u32)strtoul(m, &n, 10);
        s_test_mash_from = *n == ':' ? (u32)strtoul(n + 1, NULL, 10) : 0;
    }
    const char *pe = getenv("PPL_TEST_POS");
    s_test_pos_every = pe != NULL ? (u32)strtoul(pe, NULL, 10) : 0;
    const char *t = getenv("PPL_TEST_TOUCH");
    for (int i = 0; t != NULL && *t != '\0' && i < PC_TEST_KEYS; i++) {
        char *next;
        s_test_touch[i].frame = (u32)strtoul(t, &next, 10);
        if (*next != ':') break;
        s_test_touch[i].x = (u16)strtoul(next + 1, &next, 10);
        if (*next != ':') break;
        s_test_touch[i].y = (u16)strtoul(next + 1, &next, 10);
        s_test_touch[i].x2 = s_test_touch[i].x;
        s_test_touch[i].y2 = s_test_touch[i].y;
        if (*next == '>') {   /* N:X:Y>X2:Y2 drags over 30 frames */
            s_test_touch[i].x2 = (u16)strtoul(next + 1, &next, 10);
            if (*next == ':') s_test_touch[i].y2 = (u16)strtoul(next + 1, &next, 10);
        }
        t = *next == ',' ? next + 1 : NULL;
    }
}

static void test_hook_touch(u32 frame) {
    for (int i = 0; i < PC_TEST_KEYS; i++) {
        BOOL drag = s_test_touch[i].x2 != s_test_touch[i].x || s_test_touch[i].y2 != s_test_touch[i].y;
        u32 len = drag ? 30 : 6;
        if (s_test_touch[i].frame != 0 && frame >= s_test_touch[i].frame && frame < s_test_touch[i].frame + len) {
            int k = (int)(frame - s_test_touch[i].frame), n = drag ? 24 : 1;   /* last frames held at the end */
            if (k > n) k = n;
            int x = s_test_touch[i].x + (s_test_touch[i].x2 - s_test_touch[i].x) * k / n;
            int y = s_test_touch[i].y + (s_test_touch[i].y2 - s_test_touch[i].y) * k / n;
            pc_input_set_test_touch(true, (u16)x, (u16)y);
            return;
        }
    }
    pc_input_set_test_touch(false, 0, 0);
}

static u16 test_hook_buttons(u32 frame) {
    u16 keys = 0;
    if (s_test_talk_at != 0 && frame >= s_test_talk_at && frame < s_test_talk_at + 4) {
        keys |= PC_PAD_UP;
    }
    if (s_test_talk_at != 0 && frame >= s_test_talk_at + 40 && frame < s_test_talk_at + 46) {
        keys |= PC_PAD_A;
    }
    if (s_test_mash != 0 && (s_test_mash_from != 0 || getenv("PPL_TEST_TALK") == NULL) && frame >= s_test_mash_from
        && frame % s_test_mash < 2
        && pc_test_field_idle != NULL && !pc_test_field_idle()) {
        keys |= PC_PAD_A;
    }
    for (int i = 0; i < PC_TEST_RESETS; i++) {
        if (s_test_reset_at[i] != 0 && frame == s_test_reset_at[i]) {
            keys |= PC_PAD_L | PC_PAD_R | PC_PAD_START | PC_PAD_SELECT;
        }
    }
    for (int i = 0; i < PC_TEST_KEYS; i++) {
        if (s_test_keys[i].frame != 0 && frame >= s_test_keys[i].frame && frame < s_test_keys[i].frame + s_test_keys[i].len) {
            keys |= s_test_keys[i].keys;
        }
    }
    return keys;
}

void pc_nitro_vblank(void) {
    static u32 frames;
    static BOOL hooks_read;
    if (!hooks_read) {
        hooks_read = TRUE;
        read_test_hooks();
        read_dump_frames();
    }
    pc_watchdog_kick();
    pc_g3_flush();   /* a command written just before the wait */
    {
        /* PPL_HEAPCHECK=1: validate the game heaps every frame (slow). */
        int heapcheck = gPcHeapCheck;
        if (heapcheck < 0) {
            const char *h = getenv("PPL_HEAPCHECK");
            heapcheck = h != NULL && h[0] != '\0' && h[0] != '0';
        }
        if (heapcheck == 1 && pc_heap_check != NULL && !pc_heap_check()) {
            pc_log("heapcheck: damage found at frame %u", (unsigned)frames);
            pc_log_backtrace("heapcheck");
            heapcheck = 2;   /* report once */
        }
        gPcHeapCheck = heapcheck;
    }
    {
        /* PPL_TEST_PARTY_LEVEL=N: party set to level N once the field is idle. */
        static int party_level = -1;
        static BOOL almost_next;
        if (party_level < 0) {
            const char *l = getenv("PPL_TEST_PARTY_LEVEL");
            party_level = l != NULL ? atoi(l) : 0;
            almost_next = l != NULL && strchr(l, '+') != NULL;
        }
        if (party_level > 0 && pc_test_field_idle != NULL && pc_test_field_idle() && pc_test_set_party_level != NULL) {
            pc_test_set_party_level(party_level, almost_next);
            party_level = 0;
        }
        /* PPL_TEST_GIVE_ITEMS=ID[:QTY][,...]: bag items, once the field is idle. */
        static int give_state = -1;
        if (give_state < 0) {
            give_state = getenv("PPL_TEST_GIVE_ITEMS") != NULL || getenv("PPL_TEST_BADGES") != NULL
                      || getenv("PPL_TEST_MOVES") != NULL;
        }
        if (give_state == 1 && pc_test_field_idle != NULL && pc_test_field_idle() && pc_test_give_item != NULL) {
            const char *g = getenv("PPL_TEST_GIVE_ITEMS");
            while (g != NULL && *g != '\0') {
                char *next;
                unsigned item = (unsigned)strtoul(g, &next, 10), qty = 1;
                if (*next == ':') {
                    qty = (unsigned)strtoul(next + 1, &next, 10);
                }
                pc_test_give_item((u16)item, (u16)qty);
                g = *next == ',' ? next + 1 : NULL;
            }
            give_state = 0;
            if (getenv("PPL_TEST_BADGES") != NULL && pc_test_set_badges != NULL) {
                pc_test_set_badges(atoi(getenv("PPL_TEST_BADGES")));
            }
            const char *m = getenv("PPL_TEST_MOVES");
            while (m != NULL && *m != 0 && pc_test_set_move != NULL) {
                char *next;
                int slot = (int)strtol(m, &next, 10);
                if (*next != ':') break;
                unsigned move = (unsigned)strtoul(next + 1, &next, 10);
                pc_test_set_move(slot, (u16)move);
                m = *next == ',' ? next + 1 : NULL;
            }
        }
        /* PPL_TEST_EGG_READY=1: party eggs hatch within a few steps. */
        static int egg_ready = -1;
        if (egg_ready < 0) {
            const char *e = getenv("PPL_TEST_EGG_READY");
            egg_ready = e != NULL && e[0] == '1';
        }
        if (egg_ready == 1 && pc_test_field_idle != NULL && pc_test_field_idle() && pc_test_egg_ready != NULL) {
            pc_test_egg_ready();
            egg_ready = 0;
        }
        /* PPL_TEST_WARP=MAP:X:Z[:N][,MAP:X:Z:N...]: warps, each once the field
         * is idle and not before frame N. */
        static int warp_n = -1, warp_next;
        static unsigned warp_map[64], warp_x[64], warp_z[64], warp_at[64];
        if (warp_n < 0) {
            const char *w = getenv("PPL_TEST_WARP");
            warp_n = 0;
            while (w != NULL && warp_n < 64) {
                warp_at[warp_n] = 0;
                if (sscanf(w, "%u:%u:%u:%u", &warp_map[warp_n], &warp_x[warp_n], &warp_z[warp_n], &warp_at[warp_n]) < 3) {
                    break;
                }
                warp_n++;
                w = strchr(w, ',');
                w = w != NULL ? w + 1 : NULL;
            }
        }
        static u32 last_warp;   /* each map gets at least 120 frames */
        if (warp_next < warp_n && frames >= warp_at[warp_next] && (warp_next == 0 || frames >= last_warp + 120)
            && pc_test_field_idle != NULL && pc_test_field_idle() && pc_test_warp != NULL) {
            pc_test_warp(warp_map[warp_next], warp_x[warp_next], warp_z[warp_next]);
            last_warp = frames;
            warp_next++;
        }
        /* PPL_TEST_TALK=1: once the last warp is done and the player is free
         * for 30 frames, face north and press A (talk to whoever is there),
         * then start PPL_TEST_MASH if it has no start frame. */
        static int talk = -1;
        static u32 idle_since;
        if (talk < 0) {
            const char *t = getenv("PPL_TEST_TALK");
            talk = t != NULL && atoi(t) != 0;
        }
        if (talk == 1 && warp_next == warp_n && pc_test_field_idle != NULL) {
            if (!pc_test_field_idle()) {
                idle_since = 0;
            } else if (idle_since == 0) {
                idle_since = frames;
            } else if (frames - idle_since >= 30) {
                s_test_talk_at = frames;
                if (s_test_mash != 0 && s_test_mash_from == 0) {
                    s_test_mash_from = frames + 100;
                }
                pc_log("test: talking (frame %u)", (unsigned)frames);
                talk = 2;
            }
        }
    }
    {
        /* PPL_TEST_TRAINER=ID[:FRAME][,...]: start trainer battles from the
         * field (each once the field is idle, after FRAME). */
        static int tr_n = -1, tr_next;
        static unsigned tr_id[16], tr_at[16];
        if (tr_n < 0) {
            const char *w = getenv("PPL_TEST_TRAINER");
            tr_n = 0;
            while (w != NULL && tr_n < 16) {
                tr_at[tr_n] = 0;
                if (sscanf(w, "%u:%u", &tr_id[tr_n], &tr_at[tr_n]) < 1) {
                    break;
                }
                tr_n++;
                w = strchr(w, ',');
                w = w != NULL ? w + 1 : NULL;
            }
        }
        if (tr_next < tr_n && frames >= tr_at[tr_next] && pc_test_field_idle != NULL && pc_test_field_idle()
            && pc_test_trainer_battle != NULL) {
            pc_test_trainer_battle(tr_id[tr_next]);
            tr_next++;
        }
    }
    {
        /* PPL_TEST_SAVE_AT=N: save the game (SaveData_Save) at frame N. */
        static long save_at = -1;
        if (save_at < 0) {
            const char *t = getenv("PPL_TEST_SAVE_AT");
            save_at = t != NULL ? strtol(t, NULL, 10) : 0;
        }
        if (save_at > 0 && frames >= (u32)save_at && SaveData_Save != NULL) {
            /* in the field, save like the menu does (player and map objects
             * first) - deferred while the field map is not running */
            struct FieldSystem *fs = pc_test_field_system != NULL ? pc_test_field_system() : NULL;
            if (fs != NULL && pc_test_field_idle != NULL && !pc_test_field_idle()) {
                /* retry next frame */
            } else if ((save_at = 0, fs != NULL) && FieldSystem_Save != NULL) {   /* 0: done */
                pc_log("test: FieldSystem_Save -> %d", FieldSystem_Save(fs));
            } else {
                pc_log("test: SaveData_Save -> %d", SaveData_Save(SaveData_Ptr()));
            }
        }
    }
    {
        /* PPL_BACKTRACE_AT=N: print where the game is waiting at frame N. */
        static long trace_at = -1;
        if (trace_at < 0) {
            const char *t = getenv("PPL_BACKTRACE_AT");
            trace_at = t != NULL ? strtol(t, NULL, 10) : 0;
        }
        if (trace_at > 0 && frames == (u32)trace_at) {
            pc_log_backtrace("trace");
        }
    }
    if (s_exit_after != 0 && frames >= s_exit_after) {
        pc_log("exit after %u frames (PPL_EXIT_AFTER)", (unsigned)frames);
        pc_save_flush();
        exit(0);
    }
    if (++frames % 300 == 0) {   /* ~5 s heartbeat: proves the game loop advances */
        pc_log("vblank: %u frames, DISPCNT=%08X POWCNT=%04X", (unsigned)frames,
               (unsigned)reg_GX_DISPCNT, (unsigned)reg_GX_POWCNT);
    }
    if (s_render_pending) {
        render_frame(frames);   /* the game did not call pc_nitro_frame_ready() */
    }
    if (s_has_window && !pc_input_poll()) {
        pc_log("window closed: exiting");
        pc_save_flush();
        exit(0);
    }
    {
        /* PPL_TEST_MAP=N[,M...]: ASCII map around the player at those frames. */
        static u32 map_at[16];
        static int map_n = -1;
        if (map_n < 0) {
            map_n = 0;
            const char *m = getenv("PPL_TEST_MAP");
            while (m != NULL && *m != '\0' && map_n < 16) {
                char *next;
                map_at[map_n++] = (u32)strtoul(m, &next, 10);
                m = *next == ',' ? next + 1 : NULL;
            }
        }
        static BOOL map_pending;
        for (int i = 0; i < map_n; i++) {
            if (map_at[i] == frames) {
                map_pending = TRUE;
            }
        }
        if (map_pending && pc_test_map_dump != NULL && pc_test_map_dump(48, 24)) {
            map_pending = FALSE;   /* deferred while the field map is not running */
        }
    }
    if (s_test_pos_every != 0 && frames % s_test_pos_every == 0 && pc_test_player_pos != NULL) {
        int map, x, z;
        if (pc_test_player_pos(&map, &x, &z)) {
            pc_log("pos f=%u map=%d x=%d z=%d", (unsigned)frames, map, x, z);
        }
    }
    /* Scripted runs ignore the real keyboard / gamepad: deterministic, and
     * the window (which may take the focus) cannot catch the user's typing. */
    u16 host = s_test_keys[0].frame != 0 ? 0 : pc_input_buttons();
    pc_hw_set_pad((u16)(host | test_hook_buttons(frames)), pc_input_lid_closed());
    pc_hw_set_vcount(192);
    test_hook_touch(frames);
    if (pc_test_frame != NULL) {
        pc_test_frame(frames);
    }
    pc_nitro_tp_vblank();
    pc_snd_vblank();
    pc_wm_vblank();   /* wireless requests answer like the ARM7 (wm_pc.c) */
    pc_nitro_os_alarm_tick();
    OS_SetIrqCheckFlag(OS_IE_V_BLANK);
    pc_nitro_os_dispatch_irq(OS_IE_V_BLANK);
    pc_hw_set_vcount(0);
    s_render_pending = TRUE;
    s_frame_no = frames;
}

/*
 * When to draw: on the DS the picture of a frame is scanned out during the
 * 192 lines that follow the V-Blank, i.e. with the registers, OAM and VRAM as
 * the game left them once its V-Blank work is done (src/main.c runs part of it
 * after OS_WaitIrq returns). The game calls this hook at that point; if it
 * does not (other wait loops), the next V-Blank draws the frame instead.
 */
void pc_nitro_frame_ready(void) {
    if (s_render_pending) {
        render_frame(s_frame_no);
    }
}
