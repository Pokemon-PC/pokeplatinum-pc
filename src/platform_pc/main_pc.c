/*
 * main_pc.c - host entry point (milestone 1 bring-up harness).
 *
 * Opens the dual-screen window, mounts the NitroFS data directory, and proves
 * the data path end to end by decoding real game assets read through fs_pc:
 *   top screen    : data/ground0.NSCR tilemap + .NCGR tiles + .NCLR palette
 *   bottom screen : data/cell0.NCGR tile sheet, touch cursor, pad state
 * Later milestones replace this harness with a call to the game's
 * NitroMain() (src/main.c) running on the PC HAL.
 *
 * Usage: pokeheartgold_pc [--data DIR] [--save FILE] [--scale N]
 *                         [--side-by-side] [--fs-check] [--verbose]
 *                         [--frames N]
 */
#include "platform_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#ifndef PPL_DEFAULT_MANIFEST
#define PPL_DEFAULT_MANIFEST "nitrofs_manifest.txt"
#endif

/* ------------------------------------------------ Nitro G2D file helpers */

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Finds a block ("TTLP", "RAHC", "NRCS"...) in a Nitro G2D binary file. */
static const uint8_t *find_block(const uint8_t *file, uint32_t size, const char *magic, uint32_t *block_size) {
    if (size < 16) {
        return NULL;
    }
    uint32_t off = rd16(file + 12);
    uint16_t nblocks = rd16(file + 14);
    for (uint16_t i = 0; i < nblocks && off + 8 <= size; i++) {
        uint32_t bsize = rd32(file + off + 4);
        if (memcmp(file + off, magic, 4) == 0) {
            if (bsize < 8 || off + bsize > size) {
                return NULL;
            }
            *block_size = bsize;
            return file + off;
        }
        if (bsize == 0) {
            break;
        }
        off += bsize;
    }
    return NULL;
}

typedef struct {
    uint8_t *file;
    uint32_t colors[256];
    int      count;
} Palette;

typedef struct {
    uint8_t       *file;
    const uint8_t *data;
    uint32_t       size;
    int            bpp;       /* 4 or 8 */
    int            tiles;
} Tiles;

static bool load_palette(const char *path, Palette *pal) {
    uint32_t size, bsize;
    memset(pal, 0, sizeof *pal);
    pal->file = pc_fs_load(path, &size);
    const uint8_t *b = pal->file ? find_block(pal->file, size, "TTLP", &bsize) : NULL;
    if (b == NULL) {
        return false;
    }
    uint32_t data_size = rd32(b + 0x10), data_off = rd32(b + 0x14);
    const uint8_t *data = b + 8 + data_off;
    if (data + data_size > pal->file + size) {
        data_size = (uint32_t)(pal->file + size - data);
    }
    pal->count = (int)(data_size / 2 > 256 ? 256 : data_size / 2);
    for (int i = 0; i < pal->count; i++) {
        pal->colors[i] = pc_rgb555_to_rgba(rd16(data + 2 * i));
    }
    return true;
}

static bool load_tiles(const char *path, Tiles *t) {
    uint32_t size, bsize;
    memset(t, 0, sizeof *t);
    t->file = pc_fs_load(path, &size);
    const uint8_t *b = t->file ? find_block(t->file, size, "RAHC", &bsize) : NULL;
    if (b == NULL) {
        return false;
    }
    t->bpp  = rd32(b + 0x0C) == 4 ? 8 : 4;
    t->size = rd32(b + 0x18);
    t->data = b + 8 + rd32(b + 0x1C);
    if (t->data + t->size > t->file + size) {
        return false;
    }
    t->tiles = (int)(t->size / (uint32_t)(8 * t->bpp));
    return true;
}

static uint8_t tile_pixel(const Tiles *t, int tile, int x, int y) {
    if (tile >= t->tiles) {
        return 0;
    }
    if (t->bpp == 8) {
        return t->data[tile * 64 + y * 8 + x];
    }
    uint8_t v = t->data[tile * 32 + y * 4 + x / 2];
    return (x & 1) ? (v >> 4) : (v & 15);
}

/* Renders a text-BG screen (NSCR) exactly like a DS BG in 256x256 mode. */
static bool render_nscr(uint32_t *fb, const char *nscr_path, const Tiles *t, const Palette *pal) {
    uint32_t size, bsize;
    uint8_t *file = pc_fs_load(nscr_path, &size);
    const uint8_t *b = file ? find_block(file, size, "NRCS", &bsize) : NULL;
    if (b == NULL) {
        pc_os_free(file);
        return false;
    }
    int w_tiles = rd16(b + 8) / 8, h_tiles = rd16(b + 10) / 8;
    const uint8_t *map = b + 0x14;
    for (int ty = 0; ty < h_tiles && ty < PC_SCREEN_H / 8; ty++) {
        for (int tx = 0; tx < w_tiles && tx < PC_SCREEN_W / 8; tx++) {
            uint16_t e = rd16(map + 2 * (ty * w_tiles + tx));
            int tile = e & 0x3FF, hf = (e >> 10) & 1, vf = (e >> 11) & 1, pl = e >> 12;
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    uint8_t c = tile_pixel(t, tile, hf ? 7 - x : x, vf ? 7 - y : y);
                    int idx = t->bpp == 4 ? pl * 16 + c : c;
                    fb[(ty * 8 + y) * PC_SCREEN_W + tx * 8 + x] =
                        (c == 0 || idx >= pal->count) ? pal->colors[0] : pal->colors[idx];
                }
            }
        }
    }
    pc_os_free(file);
    return true;
}

static void render_tilesheet(uint32_t *fb, const Tiles *t, const Palette *pal) {
    for (int i = 0; i < PC_SCREEN_W * PC_SCREEN_H; i++) {
        fb[i] = 0xFF302020u;
    }
    const int per_row = PC_SCREEN_W / 8;
    for (int tile = 0; tile < t->tiles && tile < per_row * (PC_SCREEN_H / 8); tile++) {
        int ox = (tile % per_row) * 8, oy = (tile / per_row) * 8;
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                uint8_t c = tile_pixel(t, tile, x, y);
                if (c != 0 && c < pal->count) {
                    fb[(oy + y) * PC_SCREEN_W + ox + x] = pal->colors[c];
                }
            }
        }
    }
}

static void render_fallback(uint32_t *fb, uint32_t frame) {
    for (int y = 0; y < PC_SCREEN_H; y++) {
        for (int x = 0; x < PC_SCREEN_W; x++) {
            uint32_t r = (uint32_t)x, g = (uint32_t)y + 32, b = (frame * 2) & 0xFF;
            fb[y * PC_SCREEN_W + x] = r | (g << 8) | (b << 16) | 0xFF000000u;
        }
    }
}

/* ------------------------------------------------------------- overlay */

static void draw_rect(uint32_t *fb, int x0, int y0, int w, int h, uint32_t c) {
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            if (x >= 0 && y >= 0 && x < PC_SCREEN_W && y < PC_SCREEN_H) {
                fb[y * PC_SCREEN_W + x] = c;
            }
        }
    }
}

/* One indicator per PAD bit, in bit order:
 * A B Select Start Right Left Up Down R L X Y */
static void draw_pad_state(uint32_t *fb, uint16_t buttons) {
    for (int i = 0; i < 12; i++) {
        uint32_t c = (buttons & (1u << i)) ? 0xFF40FF40u : 0xFF404040u;
        draw_rect(fb, 4 + i * 10, PC_SCREEN_H - 10, 8, 6, c);
    }
}

/* Writes both screens (top above bottom, 256x384) as a 24-bit BMP. */
static bool dump_screens_bmp(const char *path) {
    const int w = PC_SCREEN_W, h = PC_SCREEN_H * 2, row = w * 3;
    const uint32_t img = (uint32_t)(row * h), fsize = 54 + img;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t v[] = { fsize, 0, 54, 40, (uint32_t)w, (uint32_t)h };
    memcpy(hdr + 2, &v[0], 4);
    memcpy(hdr + 10, &v[2], 4);
    memcpy(hdr + 14, &v[3], 4);
    memcpy(hdr + 18, &v[4], 4);
    memcpy(hdr + 22, &v[5], 4);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return false;
    }
    fwrite(hdr, 1, sizeof hdr, fp);
    uint8_t line[PC_SCREEN_W * 3];
    for (int y = h - 1; y >= 0; y--) {          /* BMP rows are bottom-up */
        const uint32_t *src = y < PC_SCREEN_H ? pc_gfx_framebuffer(PC_SCREEN_TOP) + y * w
                                              : pc_gfx_framebuffer(PC_SCREEN_BOTTOM) + (y - PC_SCREEN_H) * w;
        for (int x = 0; x < w; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] >> 16);   /* B */
            line[x * 3 + 1] = (uint8_t)(src[x] >> 8);    /* G */
            line[x * 3 + 2] = (uint8_t)(src[x]);         /* R */
        }
        fwrite(line, 1, (size_t)row, fp);
    }
    return fclose(fp) == 0;
}

/* --------------------------------------------------------------- main */

typedef struct {
    const char *data_dir;     /* NULL: standard layers for `lang` */
    const char *lang;
    const char *save_path;
    const char *manifest;
    const char *dump_path;    /* --dump: BMP of both screens at exit */
    int         scale;
    bool        side_by_side;
    bool        fs_check_only;
    bool        verbose;
    long        max_frames;   /* 0 = run until closed */
} Options;

static void usage(void) {
    fprintf(stderr,
            "usage: pokeheartgold_pc [--lang fr|en] [--data DIR] [--save FILE] [--scale N] [--side-by-side]\n"
            "                        [--fs-check] [--verbose] [--frames N] [--dump FILE.bmp]\n");
}

static bool parse_args(int argc, char **argv, Options *o) {
    o->data_dir = getenv("PPL_DATA_DIR");
    o->lang = getenv("PPL_LANG") ? getenv("PPL_LANG") : PPL_DEFAULT_LANG;
    o->save_path = "save.bin";
    o->manifest = PPL_DEFAULT_MANIFEST;
    o->scale = 2;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has_val = i + 1 < argc;
        if (!strcmp(a, "--data") && has_val) o->data_dir = argv[++i];
        else if (!strcmp(a, "--lang") && has_val) o->lang = argv[++i];
        else if (!strcmp(a, "--save") && has_val) o->save_path = argv[++i];
        else if (!strcmp(a, "--manifest") && has_val) o->manifest = argv[++i];
        else if (!strcmp(a, "--dump") && has_val) o->dump_path = argv[++i];
        else if (!strcmp(a, "--scale") && has_val) o->scale = atoi(argv[++i]);
        else if (!strcmp(a, "--frames") && has_val) o->max_frames = atol(argv[++i]);
        else if (!strcmp(a, "--side-by-side")) o->side_by_side = true;
        else if (!strcmp(a, "--fs-check")) o->fs_check_only = true;
        else if (!strcmp(a, "--verbose")) o->verbose = true;
        else { usage(); return false; }
    }
    return true;
}

static void report_fs(const Options *o) {
    PcFsReport r;
    if (!pc_fs_check_manifest(o->manifest, &r, o->verbose)) {
        pc_warn("fs: manifest %s not readable, skipping NitroFS check", o->manifest);
        return;
    }
    pc_log("fs: NitroFS manifest %u files: %u present, %u missing", r.total, r.present, r.missing);
    if (r.missing) {
        pc_log("fs: first missing: %s (build them with `make filesystem`, or point --data at an "
               "extracted ROM file system)", r.first_missing);
    }

    PcNarc narc;
    if (pc_narc_open(&narc, "data/arealight.narc")) {
        uint32_t off = 0, size = 0;
        pc_narc_member(&narc, 0, &off, &size);
        pc_log("fs: data/arealight.narc -> %u members (member 0: %u bytes @0x%X)", narc.count, size, off);
        pc_narc_close(&narc);
    } else {
        pc_warn("fs: could not open data/arealight.narc");
    }
}

int main(int argc, char **argv) {
    Options opt = { 0 };
    if (!parse_args(argc, argv, &opt)) {
        return 2;
    }
    SDL_SetMainReady();
    if (SDL_Init(0) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!pc_os_init()) {
        pc_fatal("OS layer init failed");
    }
    bool fs_ok = opt.data_dir != NULL ? pc_fs_init(opt.data_dir) : pc_fs_setup_layers(PPL_REPO_DIR, opt.lang);
    if (!fs_ok) {
        pc_fatal("no usable data directory under %s", opt.data_dir ? opt.data_dir : PPL_REPO_DIR);
    }
    for (int i = 0; i < pc_fs_root_count(); i++) {
        pc_log("fs: layer %d: %s", i, pc_fs_root_at(i));
    }
    report_fs(&opt);
    if (opt.fs_check_only) {
        SDL_Quit();
        return 0;
    }

    if (!pc_save_init(opt.save_path)) {
        pc_fatal("cannot allocate save memory");
    }
    if (!pc_gfx_init("Pokemon Platinum - PC port (milestone 1)", opt.scale)) {
        pc_fatal("graphics init failed (OpenGL 3.3 required)");
    }
    if (opt.side_by_side) {
        pc_gfx_set_layout(PC_LAYOUT_SIDE_BY_SIDE);
    }
    pc_input_init();
    pc_audio_init();

    /* Decode real assets through the PC file system. */
    uint32_t *top = pc_gfx_framebuffer(PC_SCREEN_TOP);
    uint32_t *bottom = pc_gfx_framebuffer(PC_SCREEN_BOTTOM);
    static uint32_t bottom_base[PC_SCREEN_W * PC_SCREEN_H];
    Palette pal_bg, pal_cell;
    Tiles chr_bg, chr_cell;
    bool top_ok = load_palette("data/ground0.NCLR", &pal_bg) &&
                  load_tiles("data/ground0.NCGR", &chr_bg) &&
                  render_nscr(top, "data/ground0.NSCR", &chr_bg, &pal_bg);
    bool bottom_ok = load_palette("data/cell0.NCLR", &pal_cell) &&
                     load_tiles("data/cell0.NCGR", &chr_cell);
    if (bottom_ok) {
        render_tilesheet(bottom_base, &chr_cell, &pal_cell);
        pc_log("gfx: decoded data/cell0.NCGR (%d tiles, %d bpp)", chr_cell.tiles, chr_cell.bpp);
    }
    pc_log("gfx: top screen %s", top_ok ? "= data/ground0.NSCR" : "= fallback pattern (asset missing)");
    pc_os_free(pal_bg.file); pc_os_free(chr_bg.file);
    pc_os_free(pal_cell.file); pc_os_free(chr_cell.file);

    uint64_t last_title_ms = 0, frames_at_title = 0;
    long frame = 0;
    while (pc_input_poll()) {
        uint16_t buttons = pc_input_buttons();
        PcTouch touch = pc_input_touch();

        if (!top_ok) {
            render_fallback(top, (uint32_t)frame);
        }
        if (bottom_ok) {
            memcpy(bottom, bottom_base, sizeof bottom_base);
        } else {
            render_fallback(bottom, (uint32_t)frame + 64);
        }
        draw_pad_state(bottom, buttons);
        if (touch.touching) {
            draw_rect(bottom, touch.x - 6, touch.y, 13, 1, 0xFF0000FFu);
            draw_rect(bottom, touch.x, touch.y - 6, 1, 13, 0xFF0000FFu);
        }
        if (pc_input_lid_closed()) {
            memset(top, 0, PC_SCREEN_W * PC_SCREEN_H * 4);
            memset(bottom, 0, PC_SCREEN_W * PC_SCREEN_H * 4);
        }

        pc_gfx_present();
        pc_os_wait_vblank();
        frame++;

        uint64_t now = pc_os_ms();
        if (now - last_title_ms >= 1000) {
            uint64_t vb = pc_os_vblank_count();
            char title[160];
            snprintf(title, sizeof title, "Pokemon Platinum - PC port (milestone 1) - %.2f fps",
                     (double)(vb - frames_at_title) * 1000.0 / (double)(now - last_title_ms ? now - last_title_ms : 1));
            pc_gfx_set_title(title);
            last_title_ms = now;
            frames_at_title = vb;
        }
        if (opt.max_frames > 0 && frame >= opt.max_frames) {
            break;
        }
    }

    pc_log("exiting after %ld frames (%llu vblanks)", frame, (unsigned long long)pc_os_vblank_count());
    if (opt.dump_path != NULL) {
        pc_log("dump: %s %s", opt.dump_path, dump_screens_bmp(opt.dump_path) ? "written" : "FAILED");
    }
    pc_audio_shutdown();
    pc_input_shutdown();
    pc_gfx_shutdown();
    pc_save_shutdown();
    pc_fs_shutdown();
    pc_os_shutdown();
    SDL_Quit();
    return 0;
}
