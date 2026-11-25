/*
 * gpu3d_pc.c - the DS 3D engine in software (milestone 5).
 *
 * Input: geometry commands, from three paths that all end in g3_command():
 *   - writes to the command registers (reg_G3_*, reg_G3X_GXFIFO): on PC they
 *     go through pc_g3_port() (lib/include/nitro/hw/ARM9/io_reg.h). C cannot
 *     intercept a store, so the port records the register and the value is
 *     read back at the next port access or at pc_g3_flush() (V-Blank, result
 *     reads); command order is preserved;
 *   - CPU / DMA copies to the FIFO (mi_nitro_pc.c, mi_dma_pc.c, sdk_asm_pc.c)
 *     -> pc_g3_write();
 *   - display lists sent with MI_SendGXCommand -> pc_gx_fifo_submit().
 *
 * Geometry (GBATEK "DS 3D"): matrix stacks, vertices, lighting, texture
 * coordinate transforms, polygon assembly, clipping, viewport. SWAP_BUFFERS
 * publishes the polygon list; pc_gpu3d_render() rasterises it (textures in
 * all 7 formats, modulate/decal/toon/highlight, alpha blending, alpha test,
 * depth test Z/W, fog, translucent polygon ID rule) into a 256x192 colour +
 * alpha image that the 2D renderer (ppu_pc.c) shows as BG0 of engine A.
 * Also: edge marking, anti-aliasing (approximation: half-way blend of edge
 * pixels), wireframe (alpha 0) polygons. Not emulated: the rear-plane
 * bitmap (clear image), unused by the game outside Wi-Fi setup.
 */
#include "nitro_pc.h"
#include "gpu3d_pc.h"
#include "vram_pc.h"

#include <string.h>
#include <stdlib.h>

/* <math.h> resolves to the NitroSDK header here: use the compiler builtins. */
#define sqrtf  __builtin_sqrtf
#define fminf  __builtin_fminf
#define fmaxf  __builtin_fmaxf
#define floorf __builtin_floorf
#define ceilf  __builtin_ceilf
#define fabsf  __builtin_fabsf

#define W 256
#define H 192

/* ------------------------------------------------------------- types */

typedef struct { float m[4][4]; } Mat;   /* row-vector convention: v' = v * M */

typedef struct {
    float x, y, z, w;   /* clip space */
    float r, g, b;      /* 0..31 */
    float s, t;         /* texels */
} Vtx;

#define MAX_POLY_VTX 10   /* a quad clipped by 6 planes */

typedef struct {
    Vtx  v[MAX_POLY_VTX];
    int  n;
    u32  attr;          /* POLYGON_ATTR */
    u32  tex;           /* TEXIMAGE_PARAM */
    u32  pltt;          /* PLTT_BASE */
    int  vx1, vy1, vx2, vy2;   /* viewport */
} Poly;

#define MAX_POLYS 4096

/* ------------------------------------------------------------- state */

static Mat  s_proj, s_proj_stack;
static Mat  s_pos, s_dir, s_pos_stack[32], s_dir_stack[32];
static Mat  s_tex, s_tex_stack;
static int  s_pos_sp, s_proj_sp;
static int  s_mtx_mode;

static float s_cr = 31, s_cg = 31, s_cb = 31;     /* current vertex colour */
static float s_s, s_t;                            /* current texcoord (raw 12.4) */
static float s_vx, s_vy, s_vz;                    /* last vertex (for XY/XZ/YZ/DIFF) */
static u32   s_attr_pending, s_attr, s_tex_param, s_pltt;
static int   s_vx1, s_vy1 = 0, s_vx2 = 255, s_vy2 = 191;

static float s_light_vec[4][3];
static float s_light_col[4][3];
static float s_dif[3], s_amb[3], s_spe[3], s_emi[3];
static BOOL  s_use_shininess;
static u8    s_shininess[128];

static int   s_prim;          /* BEGIN_VTXS type */
static Vtx   s_prim_vtx[4];
static int   s_prim_count, s_strip_count;

static Poly  s_build[MAX_POLYS], s_ready[MAX_POLYS];
static int   s_build_n, s_ready_n;
static u32   s_ready_swap;    /* SWAP_BUFFERS parameter of the ready list */
static BOOL  s_have_frame;

/* Port writes waiting for their value (see the header comment). */
static u32 s_pending_addr;

/* Command decoding (FIFO and register ports). */
static u32 s_cmd_params[32];
static int s_cmd_count;

/* Packed FIFO state. */
static u32 s_fifo_cmds;       /* remaining packed command bytes */
static int s_fifo_need;       /* parameters still expected for the current command */
static u32 s_fifo_cur;

static u16 s_fb_color[H][W];  /* 15-bit colour */
static u8  s_fb_alpha[H][W];  /* 0..31, 0 = no 3D pixel */

/* ------------------------------------------------------------ helpers */

static const u8 s_param_count[256] = {
    [0x10] = 1, [0x11] = 0, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 0,
    [0x16] = 16, [0x17] = 12, [0x18] = 16, [0x19] = 12, [0x1A] = 9, [0x1B] = 3, [0x1C] = 3,
    [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1, [0x25] = 1, [0x26] = 1,
    [0x27] = 1, [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 1,
    [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1, [0x34] = 32,
    [0x40] = 1, [0x41] = 0, [0x50] = 1, [0x60] = 1, [0x70] = 3, [0x71] = 2, [0x72] = 1,
};

static BOOL cmd_valid(u32 c) {
    return c == 0x11 || c == 0x15 || c == 0x41 || s_param_count[c] != 0;
}

static inline float fx12(s32 v) { return (float)v / 4096.0f; }

static void mat_identity(Mat *m) {
    memset(m, 0, sizeof *m);
    m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.0f;
}

static void mat_mul(Mat *out, const Mat *a, const Mat *b) {   /* out = a * b */
    Mat r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
        }
    }
    *out = r;
}

static void clip_matrix(Mat *out) {
    mat_mul(out, &s_pos, &s_proj);
}

static inline s32 to_fx(float v) {
    double d = (double)v * 4096.0;
    if (d > 2147483647.0) d = 2147483647.0;
    if (d < -2147483648.0) d = -2147483648.0;
    return (s32)d;
}

/* Keeps the result registers that the game reads (G3X_GetClipMtx...) and
 * the stack levels in GXSTAT current. */
static void update_results(void) {
    Mat c;
    clip_matrix(&c);
    volatile u32 *clip = (volatile u32 *)(uintptr_t)0x04000640;
    for (int i = 0; i < 16; i++) {
        clip[i] = (u32)to_fx(c.m[i / 4][i % 4]);
    }
    volatile u32 *vec = (volatile u32 *)(uintptr_t)0x04000680;
    for (int i = 0; i < 9; i++) {
        vec[i] = (u32)to_fx(s_dir.m[i / 3][i % 3]);
    }
    volatile u32 *gxstat = (volatile u32 *)(uintptr_t)0x04000600;
    *gxstat = (*gxstat & ~(0x1Fu << 8 | 1u << 13 | 1u << 27)) | ((u32)s_pos_sp & 0x1F) << 8 | ((u32)s_proj_sp & 1) << 13;
}

/* ----------------------------------------------------------- matrices */

static void load_params(Mat *m, const u32 *p, int rows, int cols) {
    mat_identity(m);
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < cols; j++) {
            m->m[i][j] = fx12((s32)p[i * cols + j]);
        }
    }
    if (cols == 3) {
        for (int i = 0; i < 4; i++) m->m[i][3] = (i == 3) ? 1.0f : 0.0f;
    }
}

static Mat *current_matrix(void) {
    switch (s_mtx_mode) {
    case 0: return &s_proj;
    case 3: return &s_tex;
    default: return &s_pos;
    }
}

/* cur = p * cur for the current mode (position + direction in mode 2). */
static void apply_mult(const Mat *p, BOOL affects_dir) {
    Mat *cur = current_matrix();
    mat_mul(cur, p, cur);
    if (s_mtx_mode == 2 && affects_dir) {
        mat_mul(&s_dir, p, &s_dir);
    }
}

static void apply_load(const Mat *p) {
    *current_matrix() = *p;
    if (s_mtx_mode == 2) {
        s_dir = *p;
    }
}

/* ------------------------------------------------------------ lighting */

static void normalize3(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static void do_lighting(float nx, float ny, float nz) {
    float n[3] = {
        nx * s_dir.m[0][0] + ny * s_dir.m[1][0] + nz * s_dir.m[2][0],
        nx * s_dir.m[0][1] + ny * s_dir.m[1][1] + nz * s_dir.m[2][1],
        nx * s_dir.m[0][2] + ny * s_dir.m[1][2] + nz * s_dir.m[2][2],
    };
    normalize3(n);
    float c[3] = { s_emi[0], s_emi[1], s_emi[2] };
    for (int i = 0; i < 4; i++) {
        if (!(s_attr & (1u << i))) {
            continue;
        }
        const float *L = s_light_vec[i];
        float diff = -(L[0] * n[0] + L[1] * n[1] + L[2] * n[2]);
        if (diff < 0) diff = 0;
        float h[3] = { (L[0] + 0) * 0.5f, (L[1] + 0) * 0.5f, (L[2] - 1) * 0.5f };
        float spec = -(h[0] * n[0] + h[1] * n[1] + h[2] * n[2]);
        if (spec < 0) spec = 0;
        spec = spec * spec;
        if (spec > 1) spec = 1;
        if (s_use_shininess) {
            spec = s_shininess[(int)(spec * 127.0f)] / 255.0f;
        }
        for (int k = 0; k < 3; k++) {
            float lc = s_light_col[i][k] / 31.0f;
            c[k] += s_spe[k] * lc * spec + s_dif[k] * lc * diff + s_amb[k] * lc;
        }
    }
    s_cr = c[0] > 31 ? 31 : c[0];
    s_cg = c[1] > 31 ? 31 : c[1];
    s_cb = c[2] > 31 ? 31 : c[2];
}

/* -------------------------------------------------------- polygons */

static int clip_plane(Vtx *in, int n, Vtx *out, int axis, float sign) {
    /* keeps sign*coord <= w */
    int m = 0;
    for (int i = 0; i < n; i++) {
        Vtx *a = &in[i], *b = &in[(i + 1) % n];
        float ca = (axis == 0 ? a->x : axis == 1 ? a->y : a->z) * sign;
        float cb = (axis == 0 ? b->x : axis == 1 ? b->y : b->z) * sign;
        float da = a->w - ca, db = b->w - cb;
        if (da >= 0) {
            if (m < MAX_POLY_VTX) out[m++] = *a;
        }
        if ((da >= 0) != (db >= 0)) {
            float t = da / (da - db);
            Vtx v;
            v.x = a->x + (b->x - a->x) * t;
            v.y = a->y + (b->y - a->y) * t;
            v.z = a->z + (b->z - a->z) * t;
            v.w = a->w + (b->w - a->w) * t;
            v.r = a->r + (b->r - a->r) * t;
            v.g = a->g + (b->g - a->g) * t;
            v.b = a->b + (b->b - a->b) * t;
            v.s = a->s + (b->s - a->s) * t;
            v.t = a->t + (b->t - a->t) * t;
            if (m < MAX_POLY_VTX) out[m++] = v;
        }
    }
    return m;
}

static void emit_polygon(const Vtx *v, int n) {
    /* Far plane: without POLYGON_ATTR bit 12 a polygon crossing it is dropped. */
    if (!(s_attr & (1u << 12))) {
        for (int i = 0; i < n; i++) {
            if (v[i].z > v[i].w) return;
        }
    }
    Vtx a[MAX_POLY_VTX], b[MAX_POLY_VTX];
    int m = n;
    memcpy(a, v, sizeof(Vtx) * n);
    for (int axis = 0; axis < 3 && m > 0; axis++) {
        m = clip_plane(a, m, b, axis, 1.0f);
        m = clip_plane(b, m, a, axis, -1.0f);
    }
    if (m < 3 || s_build_n >= MAX_POLYS) {
        return;
    }
    Poly *p = &s_build[s_build_n++];
    memcpy(p->v, a, sizeof(Vtx) * m);
    p->n = m;
    p->attr = s_attr;
    p->tex = s_tex_param;
    p->pltt = s_pltt;
    p->vx1 = s_vx1;
    p->vy1 = s_vy1;
    p->vx2 = s_vx2;
    p->vy2 = s_vy2;
}

static void add_vertex(float x, float y, float z) {
    s_vx = x;
    s_vy = y;
    s_vz = z;
    Mat c;
    clip_matrix(&c);
    Vtx v;
    v.x = x * c.m[0][0] + y * c.m[1][0] + z * c.m[2][0] + c.m[3][0];
    v.y = x * c.m[0][1] + y * c.m[1][1] + z * c.m[2][1] + c.m[3][1];
    v.z = x * c.m[0][2] + y * c.m[1][2] + z * c.m[2][2] + c.m[3][2];
    v.w = x * c.m[0][3] + y * c.m[1][3] + z * c.m[2][3] + c.m[3][3];
    v.r = s_cr;
    v.g = s_cg;
    v.b = s_cb;
    float s = s_s, t = s_t;
    if ((s_tex_param >> 30) == 3) {   /* vertex-source texture coordinates */
        s = x * s_tex.m[0][0] + y * s_tex.m[1][0] + z * s_tex.m[2][0] + s_s;
        t = x * s_tex.m[0][1] + y * s_tex.m[1][1] + z * s_tex.m[2][1] + s_t;
    }
    v.s = s / 16.0f;
    v.t = t / 16.0f;

    s_prim_vtx[s_prim_count++] = v;
    switch (s_prim) {
    case 0:   /* triangles */
        if (s_prim_count == 3) {
            emit_polygon(s_prim_vtx, 3);
            s_prim_count = 0;
        }
        break;
    case 1:   /* quads */
        if (s_prim_count == 4) {
            emit_polygon(s_prim_vtx, 4);
            s_prim_count = 0;
        }
        break;
    case 2:   /* triangle strip */
        if (s_prim_count == 3) {
            Vtx t3[3];
            if (s_strip_count & 1) {
                t3[0] = s_prim_vtx[1]; t3[1] = s_prim_vtx[0]; t3[2] = s_prim_vtx[2];
            } else {
                t3[0] = s_prim_vtx[0]; t3[1] = s_prim_vtx[1]; t3[2] = s_prim_vtx[2];
            }
            emit_polygon(t3, 3);
            s_strip_count++;
            s_prim_vtx[0] = s_prim_vtx[1];
            s_prim_vtx[1] = s_prim_vtx[2];
            s_prim_count = 2;
        }
        break;
    case 3:   /* quad strip: v0 v1 v3 v2 */
        if (s_prim_count == 4) {
            Vtx q[4] = { s_prim_vtx[0], s_prim_vtx[1], s_prim_vtx[3], s_prim_vtx[2] };
            emit_polygon(q, 4);
            s_prim_vtx[0] = s_prim_vtx[2];
            s_prim_vtx[1] = s_prim_vtx[3];
            s_prim_count = 2;
        }
        break;
    }
}

/* -------------------------------------------------------- commands */

static void swap_buffers(u32 param) {
    memcpy(s_ready, s_build, sizeof(Poly) * (size_t)s_build_n);
    s_ready_n = s_build_n;
    s_ready_swap = param;
    s_build_n = 0;
    s_have_frame = TRUE;
}

static void box_test(const u32 *p) {
    float x = fx12((s16)(p[0] & 0xFFFF)), y = fx12((s16)(p[0] >> 16));
    float z = fx12((s16)(p[1] & 0xFFFF)), w = fx12((s16)(p[1] >> 16));
    float h = fx12((s16)(p[2] & 0xFFFF)), d = fx12((s16)(p[2] >> 16));
    Mat c;
    clip_matrix(&c);
    int out_mask_all = 0x3F;
    for (int i = 0; i < 8; i++) {
        float vx = x + ((i & 1) ? w : 0), vy = y + ((i & 2) ? h : 0), vz = z + ((i & 4) ? d : 0);
        float cx = vx * c.m[0][0] + vy * c.m[1][0] + vz * c.m[2][0] + c.m[3][0];
        float cy = vx * c.m[0][1] + vy * c.m[1][1] + vz * c.m[2][1] + c.m[3][1];
        float cz = vx * c.m[0][2] + vy * c.m[1][2] + vz * c.m[2][2] + c.m[3][2];
        float cw = vx * c.m[0][3] + vy * c.m[1][3] + vz * c.m[2][3] + c.m[3][3];
        int mask = (cx > cw) | (cx < -cw) << 1 | (cy > cw) << 2 | (cy < -cw) << 3 | (cz > cw) << 4 | (cz < -cw) << 5;
        out_mask_all &= mask;
    }
    volatile u32 *gxstat = (volatile u32 *)(uintptr_t)0x04000600;
    *gxstat = (*gxstat & ~2u) | (out_mask_all == 0 ? 2u : 0u);
}

static void g3_command(u32 cmd, const u32 *p) {
    switch (cmd) {
    case 0x10: s_mtx_mode = p[0] & 3; break;
    case 0x11:   /* MTX_PUSH */
        if (s_mtx_mode == 0) { s_proj_stack = s_proj; s_proj_sp = 1; }
        else if (s_mtx_mode == 3) { s_tex_stack = s_tex; }
        else if (s_pos_sp < 31) { s_pos_stack[s_pos_sp] = s_pos; s_dir_stack[s_pos_sp] = s_dir; s_pos_sp++; }
        break;
    case 0x12: {  /* MTX_POP */
        if (s_mtx_mode == 0) { s_proj = s_proj_stack; s_proj_sp = 0; }
        else if (s_mtx_mode == 3) { s_tex = s_tex_stack; }
        else {
            int n = (int)(p[0] << 26) >> 26;
            s_pos_sp -= n;
            if (s_pos_sp < 0) s_pos_sp = 0;
            if (s_pos_sp > 31) s_pos_sp = 31;
            s_pos = s_pos_stack[s_pos_sp];
            s_dir = s_dir_stack[s_pos_sp];
        }
        break;
    }
    case 0x13:   /* MTX_STORE */
        if (s_mtx_mode == 0) s_proj_stack = s_proj;
        else if (s_mtx_mode == 3) s_tex_stack = s_tex;
        else { int i = p[0] & 31; s_pos_stack[i] = s_pos; s_dir_stack[i] = s_dir; }
        break;
    case 0x14:   /* MTX_RESTORE */
        if (s_mtx_mode == 0) s_proj = s_proj_stack;
        else if (s_mtx_mode == 3) s_tex = s_tex_stack;
        else { int i = p[0] & 31; s_pos = s_pos_stack[i]; s_dir = s_dir_stack[i]; }
        break;
    case 0x15: { Mat m; mat_identity(&m); apply_load(&m); break; }
    case 0x16: { Mat m; load_params(&m, p, 4, 4); apply_load(&m); break; }
    case 0x17: { Mat m; load_params(&m, p, 4, 3); apply_load(&m); break; }
    case 0x18: { Mat m; load_params(&m, p, 4, 4); apply_mult(&m, TRUE); break; }
    case 0x19: { Mat m; load_params(&m, p, 4, 3); apply_mult(&m, TRUE); break; }
    case 0x1A: { Mat m; load_params(&m, p, 3, 3); apply_mult(&m, TRUE); break; }
    case 0x1B: {  /* MTX_SCALE: not applied to the direction matrix */
        Mat m;
        mat_identity(&m);
        m.m[0][0] = fx12((s32)p[0]); m.m[1][1] = fx12((s32)p[1]); m.m[2][2] = fx12((s32)p[2]);
        apply_mult(&m, FALSE);
        break;
    }
    case 0x1C: {
        Mat m;
        mat_identity(&m);
        m.m[3][0] = fx12((s32)p[0]); m.m[3][1] = fx12((s32)p[1]); m.m[3][2] = fx12((s32)p[2]);
        apply_mult(&m, TRUE);
        break;
    }
    case 0x20:   /* COLOR */
        s_cr = (float)(p[0] & 31); s_cg = (float)((p[0] >> 5) & 31); s_cb = (float)((p[0] >> 10) & 31);
        break;
    case 0x21: {  /* NORMAL */
        float nx = (float)((s32)(p[0] << 22) >> 22) / 512.0f;
        float ny = (float)((s32)(p[0] << 12) >> 22) / 512.0f;
        float nz = (float)((s32)(p[0] << 2) >> 22) / 512.0f;
        if ((s_tex_param >> 30) == 2) {   /* normal-source texture coordinates */
            float s = nx * s_tex.m[0][0] + ny * s_tex.m[1][0] + nz * s_tex.m[2][0];
            float t = nx * s_tex.m[0][1] + ny * s_tex.m[1][1] + nz * s_tex.m[2][1];
            s_s = s * 16.0f + s_s;
            s_t = t * 16.0f + s_t;
        }
        do_lighting(nx, ny, nz);
        break;
    }
    case 0x22: {  /* TEXCOORD */
        float s = (float)(s16)(p[0] & 0xFFFF), t = (float)(s16)(p[0] >> 16);
        if ((s_tex_param >> 30) == 1) {
            float s2 = s * s_tex.m[0][0] + t * s_tex.m[1][0] + (s_tex.m[2][0] + s_tex.m[3][0]) / 16.0f;
            float t2 = s * s_tex.m[0][1] + t * s_tex.m[1][1] + (s_tex.m[2][1] + s_tex.m[3][1]) / 16.0f;
            s = s2;
            t = t2;
        }
        s_s = s;
        s_t = t;
        break;
    }
    case 0x23:
        add_vertex(fx12((s16)(p[0] & 0xFFFF)), fx12((s16)(p[0] >> 16)), fx12((s16)(p[1] & 0xFFFF)));
        break;
    case 0x24:
        add_vertex((float)((s32)(p[0] << 22) >> 22) / 64.0f, (float)((s32)(p[0] << 12) >> 22) / 64.0f,
                   (float)((s32)(p[0] << 2) >> 22) / 64.0f);
        break;
    case 0x25: add_vertex(fx12((s16)(p[0] & 0xFFFF)), fx12((s16)(p[0] >> 16)), s_vz); break;
    case 0x26: add_vertex(fx12((s16)(p[0] & 0xFFFF)), s_vy, fx12((s16)(p[0] >> 16))); break;
    case 0x27: add_vertex(s_vx, fx12((s16)(p[0] & 0xFFFF)), fx12((s16)(p[0] >> 16))); break;
    case 0x28:
        /* differences: sign + 9-bit fraction, scaled by 1/8 -> 1/4096 units */
        add_vertex(s_vx + (float)((s32)(p[0] << 22) >> 22) / 4096.0f,
                   s_vy + (float)((s32)(p[0] << 12) >> 22) / 4096.0f,
                   s_vz + (float)((s32)(p[0] << 2) >> 22) / 4096.0f);
        break;
    case 0x29: s_attr_pending = p[0]; break;
    case 0x2A: s_tex_param = p[0]; break;
    case 0x2B: s_pltt = p[0] & 0x1FFF; break;
    case 0x30:   /* DIF_AMB */
        s_dif[0] = (float)(p[0] & 31); s_dif[1] = (float)((p[0] >> 5) & 31); s_dif[2] = (float)((p[0] >> 10) & 31);
        s_amb[0] = (float)((p[0] >> 16) & 31); s_amb[1] = (float)((p[0] >> 21) & 31); s_amb[2] = (float)((p[0] >> 26) & 31);
        if (p[0] & 0x8000) { s_cr = s_dif[0]; s_cg = s_dif[1]; s_cb = s_dif[2]; }
        break;
    case 0x31:   /* SPE_EMI */
        s_spe[0] = (float)(p[0] & 31); s_spe[1] = (float)((p[0] >> 5) & 31); s_spe[2] = (float)((p[0] >> 10) & 31);
        s_emi[0] = (float)((p[0] >> 16) & 31); s_emi[1] = (float)((p[0] >> 21) & 31); s_emi[2] = (float)((p[0] >> 26) & 31);
        s_use_shininess = (p[0] >> 15) & 1;
        break;
    case 0x32: {  /* LIGHT_VECTOR: transformed by the direction matrix now */
        int i = p[0] >> 30;
        float lx = (float)((s32)(p[0] << 22) >> 22) / 512.0f;
        float ly = (float)((s32)(p[0] << 12) >> 22) / 512.0f;
        float lz = (float)((s32)(p[0] << 2) >> 22) / 512.0f;
        s_light_vec[i][0] = lx * s_dir.m[0][0] + ly * s_dir.m[1][0] + lz * s_dir.m[2][0];
        s_light_vec[i][1] = lx * s_dir.m[0][1] + ly * s_dir.m[1][1] + lz * s_dir.m[2][1];
        s_light_vec[i][2] = lx * s_dir.m[0][2] + ly * s_dir.m[1][2] + lz * s_dir.m[2][2];
        break;
    }
    case 0x33: {
        int i = p[0] >> 30;
        s_light_col[i][0] = (float)(p[0] & 31); s_light_col[i][1] = (float)((p[0] >> 5) & 31); s_light_col[i][2] = (float)((p[0] >> 10) & 31);
        break;
    }
    case 0x34:
        for (int i = 0; i < 32; i++) {
            s_shininess[i * 4 + 0] = (u8)p[i];
            s_shininess[i * 4 + 1] = (u8)(p[i] >> 8);
            s_shininess[i * 4 + 2] = (u8)(p[i] >> 16);
            s_shininess[i * 4 + 3] = (u8)(p[i] >> 24);
        }
        break;
    case 0x40:
        s_attr = s_attr_pending;
        s_prim = p[0] & 3;
        s_prim_count = 0;
        s_strip_count = 0;
        break;
    case 0x41: break;
    case 0x50: swap_buffers(p[0]); break;
    case 0x60:
        s_vx1 = p[0] & 0xFF; s_vy1 = (p[0] >> 8) & 0xFF; s_vx2 = (p[0] >> 16) & 0xFF; s_vy2 = (p[0] >> 24) & 0xFF;
        break;
    case 0x70: box_test(p); break;
    case 0x71: {  /* POS_TEST */
        float x = fx12((s16)(p[0] & 0xFFFF)), y = fx12((s16)(p[0] >> 16)), z = fx12((s16)(p[1] & 0xFFFF));
        s_vx = x; s_vy = y; s_vz = z;
        Mat c;
        clip_matrix(&c);
        volatile u32 *r = (volatile u32 *)(uintptr_t)0x04000620;
        for (int k = 0; k < 4; k++) {
            r[k] = (u32)to_fx(x * c.m[0][k] + y * c.m[1][k] + z * c.m[2][k] + c.m[3][k]);
        }
        break;
    }
    case 0x72: {  /* VEC_TEST */
        float nx = (float)((s32)(p[0] << 22) >> 22) / 512.0f;
        float ny = (float)((s32)(p[0] << 12) >> 22) / 512.0f;
        float nz = (float)((s32)(p[0] << 2) >> 22) / 512.0f;
        volatile u16 *r = (volatile u16 *)(uintptr_t)0x04000630;
        for (int k = 0; k < 3; k++) {
            r[k] = (u16)(s16)to_fx(nx * s_dir.m[0][k] + ny * s_dir.m[1][k] + nz * s_dir.m[2][k]);
        }
        break;
    }
    default:
        break;
    }
    if (cmd >= 0x10 && cmd <= 0x1C) {
        update_results();
    }
}

/* Unpacked command written to its own register: parameters accumulate. */
static u32 s_port_cmd;

static void port_write(u32 cmd, u32 value) {
    if (s_port_cmd != cmd) {
        s_port_cmd = cmd;
        s_cmd_count = 0;
    }
    if (s_param_count[cmd] == 0) {
        g3_command(cmd, &value);
        s_port_cmd = 0;
        return;
    }
    s_cmd_params[s_cmd_count++] = value;
    if (s_cmd_count == s_param_count[cmd]) {
        g3_command(cmd, s_cmd_params);
        s_cmd_count = 0;
        s_port_cmd = 0;
    }
}

/* Packed GXFIFO stream: a word of up to 4 command bytes, then their
 * parameters in order. */
static void fifo_next_command(void) {
    while (s_fifo_cmds != 0) {
        u32 c = s_fifo_cmds & 0xFF;
        s_fifo_cmds >>= 8;
        if (c == 0 || !cmd_valid(c)) {
            continue;
        }
        if (s_param_count[c] == 0) {
            u32 dummy = 0;
            g3_command(c, &dummy);
            continue;
        }
        s_fifo_cur = c;
        s_fifo_need = s_param_count[c];
        s_cmd_count = 0;
        return;
    }
    s_fifo_cur = 0;
}

static void fifo_write(u32 word) {
    if (s_fifo_cur != 0) {
        s_cmd_params[s_cmd_count++] = word;
        if (--s_fifo_need == 0) {
            g3_command(s_fifo_cur, s_cmd_params);
            fifo_next_command();
        }
        return;
    }
    s_fifo_cmds = word;
    fifo_next_command();
}

/* ------------------------------------------------------------ entry points */

void pc_g3_write(u32 addr, u32 value) {
    if (addr >= 0x04000400 && addr < 0x04000440) {
        fifo_write(value);
    } else if (addr >= 0x04000440 && addr < 0x04000600) {
        port_write((addr - 0x04000400) >> 2, value);
    }
}

void pc_g3_flush(void) {
    if (s_pending_addr != 0) {
        u32 a = s_pending_addr;
        s_pending_addr = 0;
        pc_g3_write(a, *(volatile u32 *)(uintptr_t)a);
    }
}

REGType32v *pc_g3_port(u32 addr) {
    pc_g3_flush();
    s_pending_addr = addr;
    return (REGType32v *)(uintptr_t)addr;
}

void *pc_g3_result(u32 addr) {
    pc_g3_flush();
    return (void *)(uintptr_t)addr;
}

BOOL pc_g3_is_port(const volatile void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= 0x04000400 && a < 0x04000600;
}

void pc_g3_cancel(const volatile void *p) {
    if (s_pending_addr == (u32)(uintptr_t)p) {
        s_pending_addr = 0;
    }
}

void pc_gx_fifo_submit(const u32 *words, u32 count) {
    pc_g3_flush();
    for (u32 i = 0; i < count; i++) {
        fifo_write(words[i]);
    }
}

void pc_gpu3d_reset(void) {
    mat_identity(&s_proj);
    mat_identity(&s_pos);
    mat_identity(&s_dir);
    mat_identity(&s_tex);
    s_pos_sp = s_proj_sp = 0;
    s_mtx_mode = 0;
    s_build_n = s_ready_n = 0;
    s_have_frame = FALSE;
    s_pending_addr = 0;
    s_fifo_cmds = 0;
    s_fifo_cur = 0;
    s_port_cmd = 0;
    s_cmd_count = 0;
    memset(s_fb_alpha, 0, sizeof s_fb_alpha);
}

/* ============================================================ rasteriser */

static u8  s_tex_img[0x80000];
static u8  s_tex_pal[0x18000];
static float s_depth[H][W];
static u8  s_attr_id[H][W];      /* polygon ID of the pixel (translucent rule) */
static u8  s_fog[H][W];
static u8  s_stencil[H][W];      /* shadow volume mask */
static BOOL s_stencil_used;      /* a shadow was drawn since the last clear */

static inline u16 rd16(const u8 *b, u32 off, u32 size) { off %= size; return (u16)(b[off] | b[(off + 1) % size] << 8); }

/* Texel fetch: returns 15-bit colour and alpha 0..31 (0 = transparent). */
static void tex_fetch(u32 param, u32 pltt, int s, int t, u16 *c, int *a) {
    int ssh = 3 + ((param >> 20) & 7), tsh = 3 + ((param >> 23) & 7);
    int sw = 1 << ssh, th = 1 << tsh;
    BOOL rs = (param >> 16) & 1, rt = (param >> 17) & 1, fs = (param >> 18) & 1, ft = (param >> 19) & 1;
    /* Repeat period = floor(coord / size) (arithmetic shift, also right for
     * negative coordinates); odd periods are mirrored when flip is on. */
    if (rs) {
        if (fs && ((s >> ssh) & 1) != 0) s = sw - 1 - (s & (sw - 1));
        else s &= sw - 1;
    } else {
        s = s < 0 ? 0 : s >= sw ? sw - 1 : s;
    }
    if (rt) {
        if (ft && ((t >> tsh) & 1) != 0) t = th - 1 - (t & (th - 1));
        else t &= th - 1;
    } else {
        t = t < 0 ? 0 : t >= th ? th - 1 : t;
    }
    u32 base = (param & 0xFFFF) << 3;
    int fmt = (param >> 26) & 7;
    BOOL c0trans = (param >> 29) & 1;
    u32 idx = 0;
    u32 texel = (u32)(t * sw + s);
    switch (fmt) {
    case 1: {   /* A3I5 */
        u8 b = s_tex_img[(base + texel) & 0x7FFFF];
        int a3 = b >> 5;
        *a = (a3 << 2) + (a3 >> 1);
        *c = rd16(s_tex_pal, (pltt << 4) + (b & 31) * 2, sizeof s_tex_pal) & 0x7FFF;
        return;
    }
    case 2: {   /* 4 colours */
        u8 b = s_tex_img[(base + texel / 4) & 0x7FFFF];
        idx = (b >> ((texel & 3) * 2)) & 3;
        *a = (c0trans && idx == 0) ? 0 : 31;
        *c = rd16(s_tex_pal, (pltt << 3) + idx * 2, sizeof s_tex_pal) & 0x7FFF;
        return;
    }
    case 3: {   /* 16 colours */
        u8 b = s_tex_img[(base + texel / 2) & 0x7FFFF];
        idx = (texel & 1) ? b >> 4 : b & 15;
        *a = (c0trans && idx == 0) ? 0 : 31;
        *c = rd16(s_tex_pal, (pltt << 4) + idx * 2, sizeof s_tex_pal) & 0x7FFF;
        return;
    }
    case 4: {   /* 256 colours */
        idx = s_tex_img[(base + texel) & 0x7FFFF];
        *a = (c0trans && idx == 0) ? 0 : 31;
        *c = rd16(s_tex_pal, (pltt << 4) + idx * 2, sizeof s_tex_pal) & 0x7FFF;
        return;
    }
    case 5: {   /* 4x4 compressed */
        u32 blocks_w = (u32)sw / 4;
        u32 block = (u32)(t / 4) * blocks_w + (u32)(s / 4);
        u32 addr = base + block * 4;
        u32 word = s_tex_img[addr & 0x7FFFF] | s_tex_img[(addr + 1) & 0x7FFFF] << 8 |
                   (u32)s_tex_img[(addr + 2) & 0x7FFFF] << 16 | (u32)s_tex_img[(addr + 3) & 0x7FFFF] << 24;
        u32 code = (word >> (((t & 3) * 4 + (s & 3)) * 2)) & 3;
        u32 slot1 = 0x20000 + ((addr & 0x1FFFF) >> 1) + ((addr >= 0x40000) ? 0x10000 : 0);
        u16 info = (u16)(s_tex_img[slot1 & 0x7FFFF] | s_tex_img[(slot1 + 1) & 0x7FFFF] << 8);
        u32 pal = (pltt << 4) + ((u32)(info & 0x3FFF) << 2);
        int mode = info >> 14;
        u16 c0 = rd16(s_tex_pal, pal, sizeof s_tex_pal), c1 = rd16(s_tex_pal, pal + 2, sizeof s_tex_pal);
        *a = 31;
        switch (code) {
        case 0: *c = c0 & 0x7FFF; return;
        case 1: *c = c1 & 0x7FFF; return;
        case 2:
            if (mode == 0 || mode == 2) { *c = rd16(s_tex_pal, pal + 4, sizeof s_tex_pal) & 0x7FFF; return; }
            if (mode == 1) {
                *c = (u16)((((c0 & 31) + (c1 & 31)) / 2) | ((((c0 >> 5) & 31) + ((c1 >> 5) & 31)) / 2) << 5 |
                           ((((c0 >> 10) & 31) + ((c1 >> 10) & 31)) / 2) << 10);
                return;
            }
            *c = (u16)((((c0 & 31) * 5 + (c1 & 31) * 3) / 8) | ((((c0 >> 5) & 31) * 5 + ((c1 >> 5) & 31) * 3) / 8) << 5 |
                       ((((c0 >> 10) & 31) * 5 + ((c1 >> 10) & 31) * 3) / 8) << 10);
            return;
        default:
            if (mode == 0 || mode == 1) { *a = 0; *c = 0; return; }
            if (mode == 2) { *c = rd16(s_tex_pal, pal + 6, sizeof s_tex_pal) & 0x7FFF; return; }
            *c = (u16)((((c0 & 31) * 3 + (c1 & 31) * 5) / 8) | ((((c0 >> 5) & 31) * 3 + ((c1 >> 5) & 31) * 5) / 8) << 5 |
                       ((((c0 >> 10) & 31) * 3 + ((c1 >> 10) & 31) * 5) / 8) << 10);
            return;
        }
    }
    case 6: {   /* A5I3 */
        u8 b = s_tex_img[(base + texel) & 0x7FFFF];
        *a = b >> 3;
        *c = rd16(s_tex_pal, (pltt << 4) + (b & 7) * 2, sizeof s_tex_pal) & 0x7FFF;
        return;
    }
    case 7: {   /* direct colour */
        u16 v = (u16)(s_tex_img[(base + texel * 2) & 0x7FFFF] | s_tex_img[(base + texel * 2 + 1) & 0x7FFFF] << 8);
        *a = (v & 0x8000) ? 31 : 0;
        *c = v & 0x7FFF;
        return;
    }
    default:
        *a = 31;
        *c = 0x7FFF;
        return;
    }
}

typedef struct {
    float x, y;        /* screen */
    float z;           /* depth value used for the test */
    float iw;          /* 1/w */
    float r, g, b, s, t;   /* divided by w */
} SVtx;

static BOOL poly_is_translucent(const Poly *p) {
    int alpha = (p->attr >> 16) & 31;
    int fmt = (p->tex >> 26) & 7;
    return (alpha > 0 && alpha < 31) || fmt == 1 || fmt == 6;
}

static void raster_poly(const Poly *p, BOOL wbuffer, u32 disp3dcnt, BOOL translucent_pass) {
    int mode = (p->attr >> 4) & 3;
    /* Shadow polygons (GBATEK "DS 3D Shadow Polygons"): a mask (ID 0) sets
     * the stencil where it is hidden behind the scene (depth test fails)
     * and draws nothing; the shadow itself (ID != 0) then draws only on
     * stencilled pixels that belong to another polygon ID. A mask that
     * follows a shadow starts a new group: the stencil is cleared. */
    BOOL shadow_mask = mode == 3 && ((p->attr >> 24) & 63) == 0;
    BOOL shadow = mode == 3 && !shadow_mask;
    if (shadow_mask && s_stencil_used) {
        memset(s_stencil, 0, sizeof s_stencil);
        s_stencil_used = FALSE;
    }
    if (shadow) {
        s_stencil_used = TRUE;
    }
    int alpha = (p->attr >> 16) & 31;
    BOOL wire = alpha == 0;   /* wireframe: outline only, drawn opaque */
    if (wire) {
        alpha = 31;
    }
    SVtx sv[MAX_POLY_VTX];
    float vw = (float)(p->vx2 - p->vx1 + 1), vh = (float)(p->vy2 - p->vy1 + 1);
    for (int i = 0; i < p->n; i++) {
        const Vtx *v = &p->v[i];
        float w = v->w != 0 ? v->w : 1e-6f;
        float iw = 1.0f / w;
        sv[i].x = (v->x * iw + 1.0f) * vw * 0.5f + (float)p->vx1;
        sv[i].y = 192.0f - ((v->y * iw + 1.0f) * vh * 0.5f + (float)p->vy1);
        sv[i].z = wbuffer ? w : v->z * iw;
        sv[i].iw = iw;
        sv[i].r = v->r * iw; sv[i].g = v->g * iw; sv[i].b = v->b * iw;
        sv[i].s = v->s * iw; sv[i].t = v->t * iw;
    }
    /* Facing (screen space, y down): positive area = clockwise on screen. */
    float area = 0;
    for (int i = 0; i < p->n; i++) {
        const SVtx *a = &sv[i], *b = &sv[(i + 1) % p->n];
        area += a->x * b->y - b->x * a->y;
    }
    BOOL front = area < 0;
    if (front && !((p->attr >> 7) & 1)) return;
    if (!front && !((p->attr >> 6) & 1)) return;

    BOOL textured = (disp3dcnt & 1) && ((p->tex >> 26) & 7) != 0;
    BOOL depth_equal = (p->attr >> 14) & 1;
    BOOL update_depth_translucent = (p->attr >> 11) & 1;
    BOOL blend = (disp3dcnt >> 3) & 1;
    BOOL alpha_test = (disp3dcnt >> 2) & 1;
    int alpha_ref = *(volatile u16 *)(uintptr_t)0x04000340 & 31;
    BOOL highlight = (disp3dcnt >> 1) & 1;
    u8 poly_id = (u8)((p->attr >> 24) & 63);
    const volatile u16 *toon = (const volatile u16 *)(uintptr_t)0x04000380;

    for (int i = 1; i + 1 < p->n; i++) {
        const SVtx *a = &sv[0], *b = &sv[i], *c = &sv[i + 1];
        float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x));
        float miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
        int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > W) x1 = W;
        if (y1 > H) y1 = H;
        float den = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
        if (fabsf(den) < 1e-6f) continue;
        float inv = 1.0f / den;
        /* Wireframe: only the polygon's own edges (not the fan diagonals):
         * a pixel is kept within 1 pixel of an outer edge. */
        float len_bc = __builtin_sqrtf((c->x - b->x) * (c->x - b->x) + (c->y - b->y) * (c->y - b->y)), len_ab = __builtin_sqrtf((b->x - a->x) * (b->x - a->x) + (b->y - a->y) * (b->y - a->y));
        float len_ca = __builtin_sqrtf((a->x - c->x) * (a->x - c->x) + (a->y - c->y) * (a->y - c->y));
        BOOL outer_ab = i == 1, outer_ca = i + 2 == p->n;
        for (int y = y0; y < y1; y++) {
            float py = (float)y + 0.5f;
            for (int x = x0; x < x1; x++) {
                float px = (float)x + 0.5f;
                float w0 = ((b->x - px) * (c->y - py) - (c->x - px) * (b->y - py)) * inv;
                float w1 = ((c->x - px) * (a->y - py) - (a->x - px) * (c->y - py)) * inv;
                float w2 = 1.0f - w0 - w1;
                if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
                if (wire) {
                    float d_bc = fabsf(w0 * den) / (len_bc > 0 ? len_bc : 1);
                    float d_ca = fabsf(w1 * den) / (len_ca > 0 ? len_ca : 1);
                    float d_ab = fabsf(w2 * den) / (len_ab > 0 ? len_ab : 1);
                    if (!(d_bc < 1.0f || (outer_ca && d_ca < 1.0f) || (outer_ab && d_ab < 1.0f))) continue;
                }
                /* top-left-ish rule: exclude pixels exactly on the far edges */
                float z = w0 * a->z + w1 * b->z + w2 * c->z;
                float depth = wbuffer ? z : z;
                float cur = s_depth[y][x];
                if (shadow_mask) {
                    BOOL pass = depth_equal ? fabsf(depth - cur) <= (wbuffer ? 0.01f : 0.0005f) : depth < cur;
                    if (!pass) s_stencil[y][x] = 1;
                    continue;
                }
                if (shadow && (!s_stencil[y][x] || (s_attr_id[y][x] & 63) == poly_id)) continue;
                if (depth_equal ? fabsf(depth - cur) > (wbuffer ? 0.01f : 0.0005f) : !(depth < cur)) continue;
                float iw = w0 * a->iw + w1 * b->iw + w2 * c->iw;
                float pw = 1.0f / iw;
                float vr = (w0 * a->r + w1 * b->r + w2 * c->r) * pw;
                float vg = (w0 * a->g + w1 * b->g + w2 * c->g) * pw;
                float vb = (w0 * a->b + w1 * b->b + w2 * c->b) * pw;
                int R = (int)(vr + 0.5f), G = (int)(vg + 0.5f), B = (int)(vb + 0.5f);
                R = R < 0 ? 0 : R > 31 ? 31 : R;
                G = G < 0 ? 0 : G > 31 ? 31 : G;
                B = B < 0 ? 0 : B > 31 ? 31 : B;
                int A = alpha;
                if (textured) {
                    float s = (w0 * a->s + w1 * b->s + w2 * c->s) * pw;
                    float t = (w0 * a->t + w1 * b->t + w2 * c->t) * pw;
                    u16 tc;
                    int ta;
                    tex_fetch(p->tex, p->pltt, (int)floorf(s), (int)floorf(t), &tc, &ta);
                    if (ta == 0) continue;
                    int tr = tc & 31, tg = (tc >> 5) & 31, tb = (tc >> 10) & 31;
                    if (mode == 1) {   /* decal */
                        R = (tr * ta + R * (31 - ta)) / 31;
                        G = (tg * ta + G * (31 - ta)) / 31;
                        B = (tb * ta + B * (31 - ta)) / 31;
                    } else if (mode == 2) {   /* toon / highlight */
                        u16 tcol = toon[R & 31];
                        int sr = tcol & 31, sg = (tcol >> 5) & 31, sb = (tcol >> 10) & 31;
                        if (!highlight) {
                            R = ((tr + 1) * (sr + 1) - 1) / 32; G = ((tg + 1) * (sg + 1) - 1) / 32; B = ((tb + 1) * (sb + 1) - 1) / 32;
                        } else {
                            R = ((tr + 1) * (R + 1) - 1) / 32 + sr; G = ((tg + 1) * (R + 1) - 1) / 32 + sg; B = ((tb + 1) * (R + 1) - 1) / 32 + sb;
                            R = R > 31 ? 31 : R; G = G > 31 ? 31 : G; B = B > 31 ? 31 : B;
                        }
                        A = ((ta + 1) * (alpha + 1) - 1) / 32;
                    } else {   /* modulate */
                        R = ((tr + 1) * (R + 1) - 1) / 32;
                        G = ((tg + 1) * (G + 1) - 1) / 32;
                        B = ((tb + 1) * (B + 1) - 1) / 32;
                        A = ((ta + 1) * (alpha + 1) - 1) / 32;
                    }
                } else if (mode == 2) {
                    u16 tcol = toon[R & 31];
                    if (!highlight) {
                        R = tcol & 31; G = (tcol >> 5) & 31; B = (tcol >> 10) & 31;
                    }
                }
                if (A == 0) continue;
                if (alpha_test && A <= alpha_ref) continue;
                BOOL pix_translucent = A < 31;
                (void)translucent_pass;
                u16 col = (u16)(R | G << 5 | B << 10);
                if (pix_translucent) {
                    /* same translucent polygon ID already there: not drawn */
                    if ((s_attr_id[y][x] & 0x80) && (s_attr_id[y][x] & 63) == poly_id) continue;
                    int da = s_fb_alpha[y][x];
                    if (blend && da != 0) {
                        u16 d = s_fb_color[y][x];
                        int dr = d & 31, dg = (d >> 5) & 31, db = (d >> 10) & 31;
                        int r2 = (R * (A + 1) + dr * (31 - A)) / 32;
                        int g2 = (G * (A + 1) + dg * (31 - A)) / 32;
                        int b2 = (B * (A + 1) + db * (31 - A)) / 32;
                        col = (u16)(r2 | g2 << 5 | b2 << 10);
                        A = A > da ? A : da;
                    }
                    s_fb_color[y][x] = col;
                    s_fb_alpha[y][x] = (u8)A;
                    s_attr_id[y][x] = (u8)(0x80 | poly_id);
                    if (update_depth_translucent) s_depth[y][x] = depth;
                } else {
                    s_fb_color[y][x] = col;
                    s_fb_alpha[y][x] = 31;
                    s_attr_id[y][x] = poly_id;
                    s_depth[y][x] = depth;
                }
                s_fog[y][x] = (u8)((p->attr >> 15) & 1);
            }
        }
    }
}

/*
 * Edge marking (DISP3DCNT bit 5) and anti-aliasing (bit 4), after all
 * polygons (GBATEK "DS 3D Rear-Plane / Edge Marking / Anti-Aliasing"):
 * an opaque pixel is on a polygon edge when a 4-neighbour has another
 * polygon ID and lies farther away (the clear plane counts, with the clear
 * polygon ID and depth, beyond the screen borders). Edge marking paints it
 * with EDGE_COLOR[polygonID / 8]; anti-aliasing blends it half-way with the
 * farther neighbour (the DS uses the polygon's coverage of the pixel).
 */
static u16 s_edge_src_color[H][W];
static u8  s_edge_src_alpha[H][W];

static void apply_edges(u32 disp3dcnt, u8 clear_id, float clear_depth) {
    BOOL edge = (disp3dcnt >> 5) & 1, aa = (disp3dcnt >> 4) & 1;
    if (!edge && !aa) {
        return;
    }
    const volatile u16 *edge_color = (const volatile u16 *)(uintptr_t)0x04000330;
    memcpy(s_edge_src_color, s_fb_color, sizeof s_fb_color);
    memcpy(s_edge_src_alpha, s_fb_alpha, sizeof s_fb_alpha);
    static const int dx[4] = { -1, 1, 0, 0 }, dy[4] = { 0, 0, -1, 1 };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            u8 id = s_attr_id[y][x];
            if ((id & 0x80) || s_edge_src_alpha[y][x] < 31) {
                continue;   /* translucent / empty: no edge */
            }
            float d = s_depth[y][x];
            int far_k = -1;
            float far_d = d;
            for (int k = 0; k < 4; k++) {
                int nx = x + dx[k], ny = y + dy[k];
                u8 nid;
                float nd;
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) {
                    nid = clear_id;
                    nd = clear_depth;
                } else {
                    nid = s_attr_id[ny][nx] & 63;
                    nd = s_depth[ny][nx];
                }
                if (nid != (id & 63) && nd > far_d) {
                    far_d = nd;
                    far_k = k;
                }
            }
            if (far_k < 0) {
                continue;
            }
            u16 c = s_edge_src_color[y][x];
            if (edge) {
                c = edge_color[(id & 63) >> 3] & 0x7FFF;
            }
            if (aa) {
                int nx = x + dx[far_k], ny = y + dy[far_k];
                BOOL inside = nx >= 0 && ny >= 0 && nx < W && ny < H;
                if (inside && s_edge_src_alpha[ny][nx] != 0) {
                    u16 n = s_edge_src_color[ny][nx];
                    int r = ((c & 31) + (n & 31)) >> 1, g = (((c >> 5) & 31) + ((n >> 5) & 31)) >> 1;
                    int b = (((c >> 10) & 31) + ((n >> 10) & 31)) >> 1;
                    c = (u16)(r | g << 5 | b << 10);
                } else {
                    s_fb_alpha[y][x] = 16;   /* against the 2D layers below */
                }
            }
            s_fb_color[y][x] = c;
        }
    }
}

static void apply_fog(u32 disp3dcnt, BOOL wbuffer) {
    if (!((disp3dcnt >> 7) & 1)) {
        return;
    }
    BOOL alpha_only = (disp3dcnt >> 6) & 1;
    int shift = (disp3dcnt >> 8) & 15;
    u32 fogc = *(volatile u32 *)(uintptr_t)0x04000358;
    u16 offset = *(volatile u16 *)(uintptr_t)0x0400035C & 0x7FFF;
    const volatile u8 *table = (const volatile u8 *)(uintptr_t)0x04000360;
    int fr = fogc & 31, fg = (fogc >> 5) & 31, fb = (fogc >> 10) & 31, fa = (fogc >> 16) & 31;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!s_fog[y][x] || s_fb_alpha[y][x] == 0) continue;
            float d = s_depth[y][x];
            /* depth as the 15-bit value of the DS depth buffer */
            int z15 = wbuffer ? (int)(d * 64.0f) : (int)((d * 0.5f + 0.5f) * 32767.0f);
            int step = 0x400 >> shift;
            int idx = step > 0 ? (z15 - offset) / step : 0;
            int density;
            if (idx < 0) density = table[0] & 127;
            else if (idx >= 31) density = table[31] & 127;
            else density = table[idx] & 127;
            if (density >= 127) density = 128;
            u16 c = s_fb_color[y][x];
            int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31, a = s_fb_alpha[y][x];
            if (!alpha_only) {
                r = (fr * density + r * (128 - density)) / 128;
                g = (fg * density + g * (128 - density)) / 128;
                b = (fb * density + b * (128 - density)) / 128;
            }
            a = (fa * density + a * (128 - density)) / 128;
            s_fb_color[y][x] = (u16)(r | g << 5 | b << 10);
            s_fb_alpha[y][x] = (u8)(a > 31 ? 31 : a);
        }
    }
}

void pc_gpu3d_render(void) {
    pc_g3_flush();
    u32 disp3dcnt = *(volatile u16 *)(uintptr_t)0x04000060;
    u32 clear = *(volatile u32 *)(uintptr_t)0x04000350;
    u16 clear_depth = *(volatile u16 *)(uintptr_t)0x04000354 & 0x7FFF;
    BOOL wbuffer = (s_ready_swap >> 1) & 1;
    u16 cc = clear & 0x7FFF;
    u8 ca = (u8)((clear >> 16) & 31);
    u8 cid = (u8)((clear >> 24) & 63);
    float cd = wbuffer ? (float)clear_depth * 512.0f / 4096.0f * 8.0f : ((float)clear_depth / 32767.0f) * 2.0f - 1.0f;
    if (clear_depth == 0x7FFF) {
        cd = wbuffer ? 1e30f : 1.0f + 1e-6f;
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            s_fb_color[y][x] = cc;
            s_fb_alpha[y][x] = ca;
            s_depth[y][x] = cd;
            s_attr_id[y][x] = cid;
            s_fog[y][x] = (u8)((clear >> 15) & 1);
        }
    }
    memset(s_stencil, 0, sizeof s_stencil);
    s_stencil_used = FALSE;
    if (!s_have_frame) {
        return;
    }
    pc_vram_texture_image(s_tex_img);
    pc_vram_texture_palette(s_tex_pal);
    for (int i = 0; i < s_ready_n; i++) {
        if (!poly_is_translucent(&s_ready[i])) {
            raster_poly(&s_ready[i], wbuffer, disp3dcnt, FALSE);
        }
    }
    for (int i = 0; i < s_ready_n; i++) {
        if (poly_is_translucent(&s_ready[i])) {
            raster_poly(&s_ready[i], wbuffer, disp3dcnt, TRUE);
        }
    }
    apply_edges(disp3dcnt, cid, cd);
    apply_fog(disp3dcnt, wbuffer);
}

const u16 *pc_gpu3d_line_color(int y) { return s_fb_color[y]; }
const u8 *pc_gpu3d_line_alpha(int y) { return s_fb_alpha[y]; }
