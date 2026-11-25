/*
 * ppu_pc.c - software renderer of the two DS 2D engines (milestone 4).
 *
 * Reads the same state as the DS hardware: I/O registers (engine A at
 * 0x04000000, B at 0x04001000), palettes, OAM and VRAM at their native
 * addresses, plus the hidden extended palettes (vram_pc.c). Each frame is
 * drawn scanline by scanline so that H-Blank DMA (mi_dma_pc.c) and the
 * H-Blank interrupt can change registers between lines, as the game's
 * raster effects expect.
 *
 * Implemented (GBATEK "DS Video"):
 *   - DISPCNT display modes 0 (off), 1 (graphics), 2 (VRAM display);
 *     forced blank; screen swap (POWCNT1 bit 15); engine power bits;
 *   - BG modes 0-5: text BGs (4/8 bpp, 4 sizes, flips, extended palettes),
 *     affine BGs, extended affine BGs (16-bit tile maps, 256-colour and
 *     direct-colour bitmaps), wrap-around, BG mosaic;
 *   - OBJ: 128 sprites, normal/affine/double size, 4/8 bpp, 1D/2D tile
 *     mapping and boundaries, bitmap sprites, extended palettes,
 *     semi-transparent and OBJ-window sprites, priorities;
 *   - windows 0/1/OBJ, blending (alpha, brighten, darken), master brightness.
 *   - display capture (DISPCAPCNT) into VRAM banks A-D;
 *   - the 3D layer (BG0 of engine A in 3D mode, gpu3d_pc.c).
 * Not yet: large-bitmap mode 6, OBJ mosaic.
 */
#include "nitro_pc.h"
#include "ppu_pc.h"
#include "vram_pc.h"
#include "gpu3d_pc.h"

#include <string.h>
#include <stdlib.h>

#define W 256
#define H 192

/* Layer ids used for windows / blending bit masks. */
enum { L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_BD };

typedef struct {
    int       id;             /* 0 = engine A, 1 = engine B */
    uintptr_t reg;            /* I/O base */
    uintptr_t bg_vram, obj_vram;
    u32       bg_mask, obj_mask;
    const u16 *bg_pal, *obj_pal;
    const u16 *oam;
    u16       bg_ext[0x4000];
    u16       obj_ext[0x1000];
    s32       ref_x[2], ref_y[2];         /* affine BG2/BG3 internal reference points */
    s32       last_x[2], last_y[2];       /* register values seen, to detect writes */
} Engine;

static Engine s_eng[2];

/* One scanline of a layer: bit 15 set = opaque pixel. */
typedef struct {
    u16 bg[4][W];
    u16 obj[W];
    u8  obj_prio[W];
    u8  obj_flags[W];     /* bit0 semi-transparent, bit1 bitmap, bits 4-7 bitmap alpha */
    u8  obj_win[W];
    u8  bg0_alpha[W];     /* 3D layer alpha (BG0 of engine A in 3D mode) */
    BOOL bg0_is_3d;
} Line;

#define OPAQUE 0x8000

static inline u16 r16(uintptr_t a) { return *(const volatile u16 *)a; }
static inline u32 r32(uintptr_t a) { return *(const volatile u32 *)a; }
static inline u8  v8(const Engine *e, BOOL obj, u32 off) {
    return *(const u8 *)((obj ? e->obj_vram : e->bg_vram) + (off & (obj ? e->obj_mask : e->bg_mask)));
}
static inline u16 v16(const Engine *e, BOOL obj, u32 off) {
    return *(const u16 *)((obj ? e->obj_vram : e->bg_vram) + (off & (obj ? e->obj_mask : e->bg_mask) & ~1u));
}

static void engine_setup(Engine *e, int id) {
    e->id = id;
    e->reg = id == 0 ? 0x04000000 : 0x04001000;
    e->bg_vram = id == 0 ? 0x06000000 : 0x06200000;
    e->bg_mask = id == 0 ? 0x7FFFF : 0x1FFFF;
    e->obj_vram = id == 0 ? 0x06400000 : 0x06600000;
    e->obj_mask = id == 0 ? 0x3FFFF : 0x1FFFF;
    e->bg_pal = (const u16 *)(uintptr_t)(id == 0 ? 0x05000000 : 0x05000400);
    e->obj_pal = (const u16 *)(uintptr_t)(id == 0 ? 0x05000200 : 0x05000600);
    e->oam = (const u16 *)(uintptr_t)(id == 0 ? 0x07000000 : 0x07000400);
}

/* ---------------------------------------------------------------- BGs */

/* Kind of each BG for each BG mode: 0 none, 1 text, 2 affine, 3 extended. */
static const u8 s_bg_kind[8][4] = {
    { 1, 1, 1, 1 }, { 1, 1, 1, 2 }, { 1, 1, 2, 2 }, { 1, 1, 1, 3 },
    { 1, 1, 2, 3 }, { 1, 1, 3, 3 }, { 1, 0, 0, 0 }, { 0, 0, 0, 0 },
};

static void draw_text_bg(const Engine *e, u32 dispcnt, int bg, int line, u16 *out) {
    u16 cnt = r16(e->reg + 0x08 + 2 * bg);
    u32 hofs = r16(e->reg + 0x10 + 4 * bg) & 0x1FF;
    u32 vofs = r16(e->reg + 0x12 + 4 * bg) & 0x1FF;
    u32 size = cnt >> 14;
    u32 wmask = (size & 1) ? 511 : 255, hmask = (size & 2) ? 511 : 255;
    u32 char_base = ((cnt >> 2) & 15) * 0x4000 + (e->id == 0 ? ((dispcnt >> 24) & 7) * 0x10000 : 0);
    u32 scr_base = ((cnt >> 8) & 31) * 0x800 + (e->id == 0 ? ((dispcnt >> 27) & 7) * 0x10000 : 0);
    BOOL bpp8 = (cnt >> 7) & 1;
    BOOL ext = bpp8 && (dispcnt & (1u << 30));
    int slot = bg;
    if (bg < 2 && (cnt & 0x2000)) {
        slot += 2;
    }
    u32 mos_h = 1;
    if (cnt & 0x40) {
        u16 mosaic = r16(e->reg + 0x4C);
        mos_h = (mosaic & 15) + 1;
        line -= line % ((mosaic >> 4 & 15) + 1);
    }
    u32 py = ((u32)line + vofs) & hmask;
    for (int x = 0; x < W; x++) {
        int sxp = x - (int)(x % mos_h);
        u32 px = ((u32)sxp + hofs) & wmask;
        u32 block = 0;
        if (size == 1) block = px >> 8;
        else if (size == 2) block = py >> 8;
        else if (size == 3) block = (px >> 8) + (py >> 8) * 2;
        u16 entry = v16(e, FALSE, scr_base + block * 0x800 + ((py & 255) >> 3) * 64 + ((px & 255) >> 3) * 2);
        u32 tx = px & 7, ty = py & 7;
        if (entry & 0x400) tx = 7 - tx;
        if (entry & 0x800) ty = 7 - ty;
        u32 tile = entry & 0x3FF, pal = entry >> 12;
        u16 c;
        if (bpp8) {
            u8 idx = v8(e, FALSE, char_base + tile * 64 + ty * 8 + tx);
            if (idx == 0) { out[x] = 0; continue; }
            c = ext ? e->bg_ext[slot * 0x1000 + pal * 256 + idx] : e->bg_pal[idx];
        } else {
            u8 b = v8(e, FALSE, char_base + tile * 32 + ty * 4 + tx / 2);
            u8 idx = (tx & 1) ? b >> 4 : b & 15;
            if (idx == 0) { out[x] = 0; continue; }
            c = e->bg_pal[pal * 16 + idx];
        }
        out[x] = (c & 0x7FFF) | OPAQUE;
    }
}

/* Affine and extended BGs (BG2 / BG3). */
static void draw_affine_bg(Engine *e, u32 dispcnt, int bg, int kind, int line, u16 *out) {
    int k = bg - 2;
    u16 cnt = r16(e->reg + 0x08 + 2 * bg);
    uintptr_t p = e->reg + 0x20 + 0x10 * k;
    s32 pa = (s16)r16(p), pc = (s16)r16(p + 4);
    s32 x0 = e->ref_x[k], y0 = e->ref_y[k];
    BOOL wrap = (cnt >> 13) & 1;
    u32 size = cnt >> 14;
    u32 char_base = ((cnt >> 2) & 15) * 0x4000 + (e->id == 0 ? ((dispcnt >> 24) & 7) * 0x10000 : 0);
    u32 scr_base = ((cnt >> 8) & 31) * 0x800 + (e->id == 0 ? ((dispcnt >> 27) & 7) * 0x10000 : 0);
    (void)line;

    int mode;   /* 0 affine (8-bit map), 1 ext tiles (16-bit map), 2 bitmap 256, 3 direct */
    u32 bw, bh;
    if (kind == 2) {
        mode = 0;
        bw = bh = 128u << size;
    } else if (!(cnt & 0x80)) {
        mode = 1;
        bw = bh = 128u << size;
    } else {
        mode = (cnt & 4) ? 3 : 2;
        static const u16 dims[4][2] = { { 128, 128 }, { 256, 256 }, { 512, 256 }, { 512, 512 } };
        bw = dims[size][0];
        bh = dims[size][1];
    }
    u32 bmp_base = ((cnt >> 8) & 31) * 0x4000;
    BOOL ext = (dispcnt & (1u << 30)) != 0;

    for (int x = 0; x < W; x++) {
        s32 tx = (x0 + pa * x) >> 8, ty = (y0 + pc * x) >> 8;
        if (wrap) {
            tx &= (s32)bw - 1;
            ty &= (s32)bh - 1;
        } else if (tx < 0 || ty < 0 || (u32)tx >= bw || (u32)ty >= bh) {
            out[x] = 0;
            continue;
        }
        u16 c;
        if (mode == 0) {
            u8 tile = v8(e, FALSE, scr_base + (ty >> 3) * (bw >> 3) + (tx >> 3));
            u8 idx = v8(e, FALSE, char_base + tile * 64 + (ty & 7) * 8 + (tx & 7));
            if (idx == 0) { out[x] = 0; continue; }
            c = e->bg_pal[idx];
        } else if (mode == 1) {
            u16 entry = v16(e, FALSE, scr_base + ((ty >> 3) * (bw >> 3) + (tx >> 3)) * 2);
            u32 px = tx & 7, py = ty & 7;
            if (entry & 0x400) px = 7 - px;
            if (entry & 0x800) py = 7 - py;
            u8 idx = v8(e, FALSE, char_base + (entry & 0x3FF) * 64 + py * 8 + px);
            if (idx == 0) { out[x] = 0; continue; }
            c = ext ? e->bg_ext[bg * 0x1000 + (entry >> 12) * 256 + idx] : e->bg_pal[idx];
        } else if (mode == 2) {
            u8 idx = v8(e, FALSE, bmp_base + ty * bw + tx);
            if (idx == 0) { out[x] = 0; continue; }
            c = e->bg_pal[idx];
        } else {
            c = v16(e, FALSE, bmp_base + (ty * bw + tx) * 2);
            if (!(c & 0x8000)) { out[x] = 0; continue; }
        }
        out[x] = (c & 0x7FFF) | OPAQUE;
    }
}

/* ---------------------------------------------------------------- OBJ */

static const u8 s_obj_size[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

static void draw_objs(const Engine *e, u32 dispcnt, int line, Line *l) {
    for (int i = 0; i < 128; i++) {
        const u16 *o = e->oam + i * 4;
        u16 a0 = o[0], a1 = o[1], a2 = o[2];
        BOOL affine = (a0 >> 8) & 1;
        if (!affine && (a0 & 0x200)) {
            continue;   /* disabled */
        }
        int shape = (a0 >> 14) & 3;
        if (shape == 3) {
            continue;
        }
        int mode = (a0 >> 10) & 3;
        int w = s_obj_size[shape][a1 >> 14][0], h = s_obj_size[shape][a1 >> 14][1];
        int bw = w, bh = h;
        if (affine && (a0 & 0x200)) {
            bw *= 2;
            bh *= 2;
        }
        int dy = (line - (a0 & 0xFF)) & 0xFF;
        if (dy >= bh) {
            continue;
        }
        int x = a1 & 0x1FF;
        if (x >= 256) {
            x -= 512;
        }
        s32 pa = 0, pb = 0, pc = 0, pd = 0;
        if (affine) {
            const u16 *g = e->oam + ((a1 >> 9) & 31) * 16;
            pa = (s16)g[3];
            pb = (s16)g[7];
            pc = (s16)g[11];
            pd = (s16)g[15];
        }
        BOOL bpp8 = (a0 >> 13) & 1;
        u32 tile = a2 & 0x3FF;
        int prio = (a2 >> 10) & 3;
        int pal = a2 >> 12;
        for (int bx = 0; bx < bw; bx++) {
            int sx = x + bx;
            if (sx < 0 || sx >= W) {
                continue;
            }
            int tx, ty;
            if (affine) {
                int ix = bx - bw / 2, iy = dy - bh / 2;
                tx = ((pa * ix + pb * iy) >> 8) + w / 2;
                ty = ((pc * ix + pd * iy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) {
                    continue;
                }
            } else {
                tx = (a1 & 0x1000) ? w - 1 - bx : bx;
                ty = (a1 & 0x2000) ? h - 1 - dy : dy;
            }
            u16 c;
            if (mode == 3) {   /* bitmap sprite */
                u32 addr;
                if (dispcnt & (1u << 6)) {
                    addr = tile * ((dispcnt & (1u << 22)) ? 256 : 128) + (ty * w + tx) * 2;
                } else if (dispcnt & (1u << 5)) {
                    addr = (tile & 0x1F) * 0x10 + (tile & 0x3E0) * 0x80 + (ty * 256 + tx) * 2;
                } else {
                    addr = (tile & 0x0F) * 0x10 + (tile & 0x3F0) * 0x80 + (ty * 128 + tx) * 2;
                }
                c = v16(e, TRUE, addr);
                if (!(c & 0x8000) || pal == 0) {
                    continue;
                }
            } else {
                u32 tile_index, base;
                if (dispcnt & (1u << 4)) {   /* 1D mapping */
                    base = tile * (32u << ((dispcnt >> 20) & 3));
                    tile_index = (ty >> 3) * (w >> 3) + (tx >> 3);
                    base += tile_index * (bpp8 ? 64 : 32);
                } else {                     /* 2D: 32 x 32 tiles of 32 bytes */
                    base = tile * 32 + (ty >> 3) * 1024 + (tx >> 3) * (bpp8 ? 64 : 32);
                }
                u8 idx;
                if (bpp8) {
                    idx = v8(e, TRUE, base + (ty & 7) * 8 + (tx & 7));
                } else {
                    u8 b = v8(e, TRUE, base + (ty & 7) * 4 + (tx & 7) / 2);
                    idx = (tx & 1) ? b >> 4 : b & 15;
                }
                if (idx == 0) {
                    continue;
                }
                if (mode == 2) {
                    l->obj_win[sx] = 1;
                    continue;
                }
                if (bpp8) {
                    c = (dispcnt & (1u << 31)) ? e->obj_ext[pal * 256 + idx] : e->obj_pal[idx];
                } else {
                    c = e->obj_pal[pal * 16 + idx];
                }
            }
            if ((l->obj[sx] & OPAQUE) && l->obj_prio[sx] <= prio) {
                continue;   /* a lower-numbered sprite of higher or equal priority wins */
            }
            l->obj[sx] = (c & 0x7FFF) | OPAQUE;
            l->obj_prio[sx] = (u8)prio;
            l->obj_flags[sx] = (u8)((mode == 1 ? 1 : 0) | (mode == 3 ? 2 | (pal << 4) : 0));
        }
    }
}

/* ------------------------------------------------------------ compose */

static inline u16 blend(u16 a, u16 b, int eva, int evb) {
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = ((a >> 5 & 31) * eva + (b >> 5 & 31) * evb) >> 4;
    int bl = ((a >> 10 & 31) * eva + (b >> 10 & 31) * evb) >> 4;
    return (u16)((r > 31 ? 31 : r) | (g > 31 ? 31 : g) << 5 | (bl > 31 ? 31 : bl) << 10);
}

static inline u16 brighten(u16 a, int evy) {
    int r = a & 31, g = a >> 5 & 31, b = a >> 10 & 31;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return (u16)(r | g << 5 | b << 10);
}

static inline u16 darken(u16 a, int evy) {
    int r = a & 31, g = a >> 5 & 31, b = a >> 10 & 31;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return (u16)(r | g << 5 | b << 10);
}

static inline BOOL in_range(int v, int lo, int hi) {
    return lo <= hi ? (v >= lo && v < hi) : (v >= lo || v < hi);
}

static void compose_line(const Engine *e, u32 dispcnt, int line, const Line *l, const BOOL bg_on[4], u16 *out) {
    u16 bgcnt[4];
    for (int i = 0; i < 4; i++) {
        bgcnt[i] = r16(e->reg + 0x08 + 2 * i);
    }
    u16 bldcnt = r16(e->reg + 0x50), bldalpha = r16(e->reg + 0x52);
    int effect = (bldcnt >> 6) & 3;
    int eva = bldalpha & 31, evb = (bldalpha >> 8) & 31;
    int evy = r16(e->reg + 0x54) & 31;
    eva = eva > 16 ? 16 : eva;
    evb = evb > 16 ? 16 : evb;
    evy = evy > 16 ? 16 : evy;
    u16 backdrop = e->bg_pal[0] & 0x7FFF;

    BOOL win0 = (dispcnt >> 13) & 1, win1 = (dispcnt >> 14) & 1, winobj = (dispcnt >> 15) & 1;
    BOOL any_win = win0 || win1 || winobj;
    u16 win0h = r16(e->reg + 0x40), win1h = r16(e->reg + 0x42);
    u16 win0v = r16(e->reg + 0x44), win1v = r16(e->reg + 0x46);
    u16 winin = r16(e->reg + 0x48), winout = r16(e->reg + 0x4A);
    BOOL in_w0v = win0 && in_range(line, win0v >> 8, win0v & 0xFF);
    BOOL in_w1v = win1 && in_range(line, win1v >> 8, win1v & 0xFF);

    for (int x = 0; x < W; x++) {
        u8 mask = 0x3F;
        if (any_win) {
            if (in_w0v && in_range(x, win0h >> 8, win0h & 0xFF)) {
                mask = winin & 0x3F;
            } else if (in_w1v && in_range(x, win1h >> 8, win1h & 0xFF)) {
                mask = (winin >> 8) & 0x3F;
            } else if (winobj && l->obj_win[x]) {
                mask = (winout >> 8) & 0x3F;
            } else {
                mask = winout & 0x3F;
            }
        }
        /* The two front-most opaque layers. */
        int top = L_BD, bot = L_BD;
        u16 ctop = backdrop, cbot = backdrop;
        int found = 0;
        for (int prio = 0; prio < 4 && found < 2; prio++) {
            if ((mask & 0x10) && (l->obj[x] & OPAQUE) && l->obj_prio[x] == prio) {
                if (found == 0) { top = L_OBJ; ctop = l->obj[x] & 0x7FFF; }
                else { bot = L_OBJ; cbot = l->obj[x] & 0x7FFF; }
                found++;
            }
            for (int bg = 0; bg < 4 && found < 2; bg++) {
                if (!bg_on[bg] || (bgcnt[bg] & 3) != prio || !(mask & (1 << bg)) || !(l->bg[bg][x] & OPAQUE)) {
                    continue;
                }
                if (found == 0) { top = bg; ctop = l->bg[bg][x] & 0x7FFF; }
                else { bot = bg; cbot = l->bg[bg][x] & 0x7FFF; }
                found++;
            }
        }
        u16 c = ctop;
        if (mask & 0x20) {
            BOOL bot_is_2nd = (bldcnt >> (8 + bot)) & 1;
            if (top == L_BG0 && l->bg0_is_3d && l->bg0_alpha[x] < 31 && bot_is_2nd) {
                /* the 3D layer blends with its own per-pixel alpha */
                int a = l->bg0_alpha[x] + 1;
                c = blend(ctop, cbot, a / 2, 16 - a / 2);
            } else if (top == L_OBJ && (l->obj_flags[x] & 3) && bot_is_2nd) {
                if (l->obj_flags[x] & 2) {
                    int a = (l->obj_flags[x] >> 4) + 1;
                    c = blend(ctop, cbot, a, 16 - a);
                } else {
                    c = blend(ctop, cbot, eva, evb);
                }
            } else if ((bldcnt >> top) & 1) {
                if (effect == 1 && bot_is_2nd) {
                    c = blend(ctop, cbot, eva, evb);
                } else if (effect == 2) {
                    c = brighten(ctop, evy);
                } else if (effect == 3) {
                    c = darken(ctop, evy);
                }
            }
        }
        out[x] = c;
    }
}

/* ---------------------------------------------------------- H-Blank */

typedef struct {
    BOOL      active;
    uintptr_t src, dest;
    u32       ctrl;
} HDma;

static void hdma_begin(HDma dma[4]) {
    for (u32 i = 0; i < 4; i++) {
        const PcDmaHBlank *ch = pc_dma_hblank_channel(i);
        dma[i].active = ch != NULL;
        if (ch != NULL) {
            dma[i].src = ch->src;
            dma[i].dest = ch->dest;
            dma[i].ctrl = ch->ctrl;
        }
    }
}

static void hdma_step(HDma dma[4]) {
    for (int i = 0; i < 4; i++) {
        if (!dma[i].active) {
            continue;
        }
        u32 ctrl = dma[i].ctrl, count = ctrl & 0x1FFFFF;
        int unit = (ctrl >> 26) & 1 ? 4 : 2;
        int smode = (ctrl >> 23) & 3, dmode = (ctrl >> 21) & 3;
        intptr_t sstep = smode == 0 ? unit : smode == 1 ? -unit : 0;
        intptr_t dstep = (dmode == 0 || dmode == 3) ? unit : dmode == 1 ? -unit : 0;
        uintptr_t d = dma[i].dest;
        for (u32 n = 0; n < count; n++) {
            if (unit == 4) {
                *(volatile u32 *)d = *(const volatile u32 *)dma[i].src;
            } else {
                *(volatile u16 *)d = *(const volatile u16 *)dma[i].src;
            }
            dma[i].src += sstep;
            d += dstep;
        }
        if (dmode != 3) {
            dma[i].dest = d;
        }
        if (!((ctrl >> 25) & 1)) {   /* not repeating: one transfer only */
            dma[i].active = FALSE;
        }
    }
}

/* ------------------------------------------------------------- frame */

static inline u32 to_rgba(u16 c) {
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

static u16 master_bright(u16 c, u16 mb) {
    int mode = mb >> 14, f = mb & 31;
    f = f > 16 ? 16 : f;
    if (mode == 1) return brighten(c, f);
    if (mode == 2) return darken(c, f);
    return c;
}

/* Reloads an affine reference point when the game (or an H-Blank DMA)
 * wrote BGxX/BGxY since the last line, as the hardware does. */
static void affine_reload(Engine *e, BOOL frame_start) {
    for (int k = 0; k < 2; k++) {
        s32 x = (s32)(r32(e->reg + 0x28 + 0x10 * k) << 4) >> 4;
        s32 y = (s32)(r32(e->reg + 0x2C + 0x10 * k) << 4) >> 4;
        if (frame_start || x != e->last_x[k]) {
            e->ref_x[k] = x;
            e->last_x[k] = x;
        }
        if (frame_start || y != e->last_y[k]) {
            e->ref_y[k] = y;
            e->last_y[k] = y;
        }
    }
}

static void affine_advance(Engine *e) {
    for (int k = 0; k < 2; k++) {
        uintptr_t p = e->reg + 0x20 + 0x10 * k;
        e->ref_x[k] += (s16)r16(p + 2);   /* PB */
        e->ref_y[k] += (s16)r16(p + 6);   /* PD */
    }
}

static u16 s_lines[2][H][W];

/*
 * Display capture (DISPCAPCNT, GBATEK "DS Display Capture"): while bit 31 is
 * set at the start of a frame, engine A's picture (source A: BG+OBJ+3D
 * before master brightness, or the 3D layer alone) and/or a VRAM bank
 * (source B) are written, optionally blended, into VRAM bank A-D; bit 31
 * clears when the frame is done. Used by the wild-encounter transition,
 * the field motion blur and the legendary cut-scenes.
 */
typedef struct {
    BOOL on;
    int  mode, src_a, src_b, dest, w, h, eva, evb;
    u32  wofs, rofs;   /* in halfwords */
} Capture;
static Capture s_cap;

static void capture_begin(void) {
    static const int k_w[4] = { 128, 256, 256, 256 }, k_h[4] = { 128, 64, 128, 192 };
    u32 c = r32(0x04000064);
    s_cap.on = (c >> 31) & 1;
    if (!s_cap.on) {
        return;
    }
    s_cap.eva = c & 0x1F;
    s_cap.evb = (c >> 8) & 0x1F;
    if (s_cap.eva > 16) s_cap.eva = 16;
    if (s_cap.evb > 16) s_cap.evb = 16;
    s_cap.dest = (c >> 16) & 3;
    s_cap.wofs = ((c >> 18) & 3) * 0x4000u;
    s_cap.w = k_w[(c >> 20) & 3];
    s_cap.h = k_h[(c >> 20) & 3];
    s_cap.src_a = (c >> 24) & 1;
    s_cap.src_b = (c >> 25) & 1;
    s_cap.rofs = ((c >> 26) & 3) * 0x4000u;
    s_cap.mode = (c >> 29) & 3;
}

static void capture_end(void) {
    if (s_cap.on) {
        *(volatile u32 *)0x04000064 &= 0x7FFFFFFFu;
        s_cap.on = FALSE;
    }
}

static void capture_line(int line, const u16 *gfx, u32 dispcnt, int display) {
    u16 *dst = (u16 *)pc_vram_lcdc_bank(s_cap.dest);
    const u16 *vram = (const u16 *)pc_vram_lcdc_bank((dispcnt >> 18) & 3);
    if (dst == NULL || vram == NULL) {
        return;
    }
    const u16 *c3 = s_cap.src_a ? pc_gpu3d_line_color(line) : NULL;
    const u8 *a3 = s_cap.src_a ? pc_gpu3d_line_alpha(line) : NULL;
    u32 rofs = display == 2 ? 0 : s_cap.rofs;   /* ignored in VRAM display mode */
    u32 row = (u32)line * (u32)s_cap.w;
    for (int x = 0; x < s_cap.w; x++) {
        u16 a = c3 != NULL ? (u16)((c3[x] & 0x7FFF) | (a3[x] != 0 ? 0x8000 : 0)) : (u16)(gfx[x] | 0x8000);
        u16 b = s_cap.src_b ? 0 : vram[(rofs + row + x) & 0xFFFF];   /* main-memory FIFO: unused */
        u16 v;
        switch (s_cap.mode) {
        case 0:  v = a; break;
        case 1:  v = b; break;
        default: {
            int fa = (a >> 15) ? s_cap.eva : 0, fb = (b >> 15) ? s_cap.evb : 0;
            v = 0;
            for (int s = 0; s < 15; s += 5) {
                int ch = (((a >> s) & 31) * fa + ((b >> s) & 31) * fb + 8) >> 4;
                v |= (u16)((ch > 31 ? 31 : ch) << s);
            }
            if (fa != 0 || fb != 0) {
                v |= 0x8000;
            }
            break;
        }
        }
        dst[(s_cap.wofs + row + x) & 0xFFFF] = v;
    }
}

static void render_graphics(Engine *e, u32 dispcnt, int line, u16 *out);

static void render_line(Engine *e, int line, u16 *out) {
    u32 dispcnt = r32(e->reg);
    u32 powcnt = r16(0x04000304);
    BOOL powered = e->id == 0 ? (powcnt >> 1) & 1 : (powcnt >> 9) & 1;
    int display = (dispcnt >> 16) & 3;
    if (e->id == 1) {
        display &= 1;
    }
    if (!powered) {
        memset(out, 0, W * sizeof *out);
        return;
    }
    BOOL capture = e->id == 0 && s_cap.on && line < s_cap.h;
    static u16 gfx[W];
    if (capture || (display == 1 && !(dispcnt & 0x80))) {
        if (dispcnt & 0x80) {
            for (int x = 0; x < W; x++) gfx[x] = 0x7FFF;   /* forced blank: white */
        } else {
            render_graphics(e, dispcnt, line, gfx);
        }
    }
    if (display == 0 || (dispcnt & 0x80)) {
        for (int x = 0; x < W; x++) out[x] = 0x7FFF;   /* white */
    } else if (display == 1) {
        memcpy(out, gfx, sizeof gfx);
    } else if (display == 2) {
        const u16 *bank = (const u16 *)pc_vram_lcdc_bank((dispcnt >> 18) & 3);
        memcpy(out, bank + line * W, W * sizeof *out);
    } else {
        memset(out, 0, W * sizeof *out);   /* main-memory display: not used by the game */
    }
    if (capture) {
        capture_line(line, gfx, dispcnt, display);   /* after the display read */
    }
}

/* Debug: PPL_LAYERS=hex mask of the layers drawn (bits 0-3 engine A BG0-3,
 * bit 4 its OBJ, bits 5-9 the same for engine B; default all). */
static u32 layer_mask(void) {
    static long s_mask = -1;
    if (s_mask < 0) {
        const char *v = getenv("PPL_LAYERS");
        s_mask = v != NULL ? strtol(v, NULL, 16) : 0x3FF;
    }
    return (u32)s_mask;
}

static void render_graphics(Engine *e, u32 dispcnt, int line, u16 *out) {
    static Line l;
    dispcnt &= ~((((~layer_mask()) >> (e->id * 5)) & 0x1Fu) << 8);
    memset(&l, 0, sizeof l);
    int mode = dispcnt & 7;
    BOOL bg_on[4];
    for (int bg = 0; bg < 4; bg++) {
        int kind = s_bg_kind[mode][bg];
        bg_on[bg] = kind != 0 && ((dispcnt >> (8 + bg)) & 1);
        if (!bg_on[bg]) {
            continue;
        }
        if (bg == 0 && e->id == 0 && (dispcnt & 8)) {   /* 3D layer */
            const u16 *c3 = pc_gpu3d_line_color(line);
            const u8 *a3 = pc_gpu3d_line_alpha(line);
            int hofs = r16(e->reg + 0x10) & 0x1FF;
            if (hofs >= 256) hofs -= 512;
            l.bg0_is_3d = TRUE;
            for (int x = 0; x < W; x++) {
                int sx = x + hofs;
                if (sx < 0 || sx >= W || a3[sx] == 0) {
                    l.bg[0][x] = 0;
                    continue;
                }
                l.bg[0][x] = (c3[sx] & 0x7FFF) | OPAQUE;
                l.bg0_alpha[x] = a3[sx];
            }
            continue;
        }
        if (kind == 1) {
            draw_text_bg(e, dispcnt, bg, line, l.bg[bg]);
        } else {
            draw_affine_bg(e, dispcnt, bg, kind, line, l.bg[bg]);
        }
    }
    if (dispcnt & (1u << 12)) {
        draw_objs(e, dispcnt, line, &l);
    }
    compose_line(e, dispcnt, line, &l, bg_on, out);
}

void pc_ppu_render_frame(void) {
    HDma dma[4];
    hdma_begin(dma);
    {
        u32 dispcnt_a = r32(0x04000000);
        if ((dispcnt_a & 8) && (dispcnt_a & 0x100)) {
            pc_gpu3d_render();
        }
    }
    capture_begin();
    for (int i = 0; i < 2; i++) {
        engine_setup(&s_eng[i], i);
        pc_vram_bg_ext_palettes(i, s_eng[i].bg_ext);
        pc_vram_obj_ext_palettes(i, s_eng[i].obj_ext);
        affine_reload(&s_eng[i], TRUE);
    }
    for (int line = 0; line < H; line++) {
        pc_hw_set_vcount((u16)line);
        for (int i = 0; i < 2; i++) {
            affine_reload(&s_eng[i], FALSE);
            render_line(&s_eng[i], line, s_lines[i][line]);
            affine_advance(&s_eng[i]);
        }
        /* H-Blank after the line: DMA, then the interrupt if enabled. */
        hdma_step(dma);
        if (r16(0x04000004) & 0x10) {
            pc_nitro_os_dispatch_irq(OS_IE_H_BLANK);
        }
    }

    capture_end();

    /* Master brightness, then engine A goes to the top screen when
     * POWCNT1 bit 15 is set, otherwise to the bottom one. */
    BOOL a_on_top = (r16(0x04000304) >> 15) & 1;
    for (int i = 0; i < 2; i++) {
        u16 mb = r16(s_eng[i].reg + 0x6C);
        uint32_t *fb = pc_gfx_framebuffer((i == 0) == a_on_top ? PC_SCREEN_TOP : PC_SCREEN_BOTTOM);
        for (int line = 0; line < H; line++) {
            for (int x = 0; x < W; x++) {
                fb[line * W + x] = to_rgba(master_bright(s_lines[i][line][x], mb));
            }
        }
    }
}
