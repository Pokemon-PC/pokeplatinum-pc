/*
 * gfx_pc.c - window, OpenGL context and dual-screen presentation.
 *
 * Milestone 1: an SDL2 window with an OpenGL 3.3 core context presenting two
 * 256x192 RGBA framebuffers (top / bottom screen), aspect-correct, nearest
 * filtered, resizable, stacked / side by side / large top screen (F9),
 * optionally stretched to the whole window, fullscreen (F11).
 *
 * Later milestones render into these framebuffers:
 *   - ppu2d_pc.c  : software renderer of engine A/B (BG0-3 text/affine/
 *                   bitmap, OAM sprites, windows, blending, mosaic, master
 *                   brightness) reading the virtual VRAM/OAM/palette.
 *   - gpu3d_pc.c  : G3_* / GX FIFO command decoder -> OpenGL draw calls into
 *                   an FBO, composited as BG0 of engine A.
 */
#include "platform_pc.h"

#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <SDL_opengl.h>

/* --- minimal GL 2.0+/3.0 loader (GL 1.1 symbols come from SDL_opengl.h) */
#define PC_GL_FUNCS(X)                                                         \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                   \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                   \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                 \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                     \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                           \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                   \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                 \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                   \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                     \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                   \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                         \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                                 \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                       \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                       \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                         \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                         \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                             \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                             \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                       \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)

#define PC_GL_DECLARE(type, name) static type p_##name;
PC_GL_FUNCS(PC_GL_DECLARE)

static SDL_Window   *s_window;
static SDL_GLContext s_gl;
static GLuint        s_tex[2];
static GLuint        s_prog;
static GLuint        s_vao;
static PcLayout      s_layout = PC_LAYOUT_STACKED;
static bool          s_stretch;   /* fill the window, ignoring the aspect ratio */
static uint32_t      s_fb[2][PC_SCREEN_W * PC_SCREEN_H];

/* Screen rectangles in *window* coordinates (for touch mapping). */
typedef struct { int x, y, w, h; } Rect;
static Rect s_rect[2];

static const char *k_vs =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
    "    v_uv = vec2(p.x, 1.0 - p.y);\n"          /* row 0 = top of screen */
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* Picture filters (config.ini [display] filter, F7 cycles them). The texture
 * is sampled bilinearly; u_scale = window pixels per DS pixel.
 *   nearest   : blocky pixels (texel centres)
 *   sharp     : sharp bilinear - crisp pixels, smoothed only on texel
 *               borders, clean at any scale (default)
 *   smooth    : plain bilinear
 *   scanlines : sharp + darker lines between DS pixel rows (CRT look)
 *   lcd       : sharp + faint grid between pixels (DS LCD look) */
static const char *k_fs =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_screen;\n"
    "uniform int u_mode;\n"
    "uniform float u_scale;\n"
    "const vec2 size = vec2(256.0, 192.0);\n"
    "vec2 sharp(vec2 texel) {\n"
    "    vec2 c = fract(texel) - 0.5;\n"
    "    float r = 0.5 - 0.5 / max(u_scale, 1.0);\n"
    "    vec2 f = (c - clamp(c, -r, r)) * max(u_scale, 1.0) + 0.5;\n"
    "    return (floor(texel) + f) / size;\n"
    "}\n"
    "void main() {\n"
    "    vec2 texel = v_uv * size;\n"
    "    vec3 col;\n"
    "    if (u_mode == 0) col = texture(u_screen, (floor(texel) + 0.5) / size).rgb;\n"
    "    else if (u_mode == 2) col = texture(u_screen, v_uv).rgb;\n"
    "    else col = texture(u_screen, sharp(texel)).rgb;\n"
    "    if (u_mode == 3 && u_scale >= 1.5) {\n"
    "        col *= (0.72 + 0.28 * sin(3.14159 * fract(texel.y))) * 1.12;\n"
    "    } else if (u_mode == 4 && u_scale >= 2.5) {\n"
    "        vec2 g = fract(texel);\n"
    "        float e = 1.0 / u_scale;\n"
    "        if (g.x < e || g.y < e) col *= 0.82;\n"
    "    }\n"
    "    o_color = vec4(min(col, vec3(1.0)), 1.0);\n"
    "}\n";

static int s_filter = PC_FILTER_SHARP;
static const char *const k_filter_names[PC_FILTER_COUNT] = { "nearest", "sharp", "smooth", "scanlines", "lcd" };

void pc_gfx_set_filter(int filter) {
    s_filter = filter >= 0 && filter < PC_FILTER_COUNT ? filter : PC_FILTER_SHARP;
}

int pc_gfx_filter(void) {
    return s_filter;
}

const char *pc_gfx_filter_name(int filter) {
    return filter >= 0 && filter < PC_FILTER_COUNT ? k_filter_names[filter] : "?";
}

static bool load_gl(void) {
#define PC_GL_LOAD(type, name)                                                     \
    p_##name = (type)SDL_GL_GetProcAddress(#name);                                 \
    if (p_##name == NULL) {                                                        \
        pc_warn("OpenGL function %s missing", #name);                              \
        return false;                                                              \
    }
    PC_GL_FUNCS(PC_GL_LOAD)
#undef PC_GL_LOAD
    return true;
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        pc_warn("shader compile error: %s", log);
        p_glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool create_pipeline(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, k_vs);
    GLuint fs = compile(GL_FRAGMENT_SHADER, k_fs);
    if (!vs || !fs) {
        return false;
    }
    s_prog = p_glCreateProgram();
    p_glAttachShader(s_prog, vs);
    p_glAttachShader(s_prog, fs);
    p_glLinkProgram(s_prog);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetProgramInfoLog(s_prog, sizeof log, NULL, log);
        pc_warn("shader link error: %s", log);
        return false;
    }
    p_glUseProgram(s_prog);
    p_glUniform1i(p_glGetUniformLocation(s_prog, "u_screen"), 0);
    p_glGenVertexArrays(1, &s_vao);

    glGenTextures(2, s_tex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, s_tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, PC_SCREEN_W, PC_SCREEN_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_fb[i]);
    }
    return true;
}

/*
 * Each layout places the two screens in a "content" area (in DS pixels),
 * which is then scaled into the window: uniformly (black bars, no
 * distortion) or, with stretch on, to fill the whole window.
 *   stacked      top above bottom, like the console     (256x384, 2:3)
 *   side by side                                         (512x192, 8:3)
 *   large top    top screen 3x larger, touch screen on its right,
 *                vertically centred                      (1024x576, 16:9)
 */
static void compute_layout(void) {
    const int W = PC_SCREEN_W, H = PC_SCREEN_H;
    int ww, wh;
    SDL_GetWindowSize(s_window, &ww, &wh);
    Rect c[2];
    int cw, ch;
    switch (s_layout) {
    case PC_LAYOUT_SIDE_BY_SIDE:
        cw = 2 * W, ch = H;
        c[0] = (Rect){ 0, 0, W, H };
        c[1] = (Rect){ W, 0, W, H };
        break;
    case PC_LAYOUT_LARGE_TOP:
        cw = 4 * W, ch = 3 * H;
        c[0] = (Rect){ 0, 0, 3 * W, 3 * H };
        c[1] = (Rect){ 3 * W, H, W, H };
        break;
    default:
        cw = W, ch = 2 * H;
        c[0] = (Rect){ 0, 0, W, H };
        c[1] = (Rect){ 0, H, W, H };
        break;
    }
    double sx = (double)ww / cw, sy = (double)wh / ch;
    if (!s_stretch) {
        sx = sy = sx < sy ? sx : sy;
    }
    int ox = (ww - (int)(cw * sx)) / 2, oy = (wh - (int)(ch * sy)) / 2;
    for (int i = 0; i < 2; i++) {
        int x0 = (int)(c[i].x * sx), x1 = (int)((c[i].x + c[i].w) * sx);
        int y0 = (int)(c[i].y * sy), y1 = (int)((c[i].y + c[i].h) * sy);
        s_rect[i] = (Rect){ ox + x0, oy + y0, x1 - x0, y1 - y0 };
    }
}

/* Resizes a windowed window to the current layout's shape: the DS screens at
 * `scale` (large top: the touch screen at `scale`/2, the top one at 1.5x),
 * shrunk to fit the desktop. */
void pc_gfx_fit_window(int scale) {
    if (s_window == NULL || (SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    int w, h;
    switch (s_layout) {
    case PC_LAYOUT_SIDE_BY_SIDE: w = 2 * PC_SCREEN_W * scale, h = PC_SCREEN_H * scale; break;
    case PC_LAYOUT_LARGE_TOP:    w = 2 * PC_SCREEN_W * scale, h = 3 * PC_SCREEN_H * scale / 2; break;
    default:                     w = PC_SCREEN_W * scale, h = 2 * PC_SCREEN_H * scale; break;
    }
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(s_window), &usable) == 0) {
        int maxw = usable.w * 9 / 10, maxh = usable.h * 9 / 10;   /* room for borders */
        if (w > maxw || h > maxh) {
            double k = (double)maxw / w < (double)maxh / h ? (double)maxw / w : (double)maxh / h;
            w = (int)(w * k), h = (int)(h * k);
        }
    }
    SDL_SetWindowSize(s_window, w, h);
    SDL_SetWindowPosition(s_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    compute_layout();
}

void pc_gfx_set_stretch(bool stretch) {
    s_stretch = stretch;
    if (s_window != NULL) {
        compute_layout();
    }
}

bool pc_gfx_stretch(void) {
    return s_stretch;
}

bool pc_gfx_init(const char *title, int scale) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        pc_warn("SDL video init failed: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);   /* needed by the 3D backend */

    if (scale < 1) {
        scale = 1;
    }
    s_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                PC_SCREEN_W * scale, PC_SCREEN_H * 2 * scale,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (s_window == NULL) {
        pc_warn("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetWindowMinimumSize(s_window, PC_SCREEN_W, PC_SCREEN_H);
    s_gl = SDL_GL_CreateContext(s_window);
    if (s_gl == NULL) {
        pc_warn("OpenGL 3.3 core context unavailable: %s", SDL_GetError());
        return false;
    }
    /* VSync off: pacing comes from pc_os_wait_vblank() at the DS rate
     * (59.83 Hz), which a 60/120/144 Hz monitor cannot match exactly. */
    SDL_GL_SetSwapInterval(0);
    if (!load_gl() || !create_pipeline()) {
        return false;
    }
    pc_log("gfx: %s / %s / %s", (const char *)glGetString(GL_VENDOR),
           (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    compute_layout();
    return true;
}

void pc_gfx_shutdown(void) {
    if (s_gl != NULL) {
        glDeleteTextures(2, s_tex);
        p_glDeleteVertexArrays(1, &s_vao);
        p_glDeleteProgram(s_prog);
        SDL_GL_DeleteContext(s_gl);
        s_gl = NULL;
    }
    if (s_window != NULL) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

uint32_t *pc_gfx_framebuffer(PcScreen screen) {
    return s_fb[screen == PC_SCREEN_BOTTOM ? 1 : 0];
}

/* Test hook: PPL_DUMP_WINDOW=N saves the whole window (as presented, layout
 * and scaling included) to window.png at the N-th presented frame. */
static void dump_window(int dw, int dh) {
    static long s_at = -1, s_count;
    if (s_at == -1) {
        const char *e = getenv("PPL_DUMP_WINDOW");
        s_at = e != NULL ? strtol(e, NULL, 10) : 0;
    }
    if (s_at <= 0 || ++s_count != s_at) {
        return;
    }
    uint32_t *px = malloc((size_t)dw * dh * 4), *flip = malloc((size_t)dw * dh * 4);
    if (px != NULL && flip != NULL) {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (int y = 0; y < dh; y++) {   /* GL rows are bottom-up */
            memcpy(flip + (size_t)y * dw, px + (size_t)(dh - 1 - y) * dw, (size_t)dw * 4);
        }
        if (pc_write_png("window.png", flip, dw, dh)) {
            pc_log("window dump: window.png (%dx%d)", dw, dh);
        }
    }
    free(px);
    free(flip);
}

void pc_gfx_present(void) {
    int ww, wh, dw, dh;
    SDL_GetWindowSize(s_window, &ww, &wh);
    SDL_GL_GetDrawableSize(s_window, &dw, &dh);
    double kx = ww ? (double)dw / ww : 1.0, ky = wh ? (double)dh / wh : 1.0;

    glViewport(0, 0, dw, dh);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    p_glUseProgram(s_prog);
    p_glUniform1i(p_glGetUniformLocation(s_prog, "u_mode"), s_filter);
    p_glBindVertexArray(s_vao);
    p_glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, s_tex[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PC_SCREEN_W, PC_SCREEN_H,
                        GL_RGBA, GL_UNSIGNED_BYTE, s_fb[i]);
        const Rect *r = &s_rect[i];
        int vx = (int)(r->x * kx), vw = (int)(r->w * kx);
        int vh = (int)(r->h * ky);
        int vy = dh - (int)((r->y + r->h) * ky);   /* GL origin = bottom-left */
        glViewport(vx, vy, vw, vh);
        p_glUniform1f(p_glGetUniformLocation(s_prog, "u_scale"), (float)vh / PC_SCREEN_H);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    dump_window(dw, dh);
    SDL_GL_SwapWindow(s_window);
}

void pc_gfx_set_layout(PcLayout layout) {
    s_layout = layout;
    compute_layout();
}

PcLayout pc_gfx_layout(void) {
    return s_layout;
}

void pc_gfx_toggle_fullscreen(void) {
    Uint32 flags = SDL_GetWindowFlags(s_window);
    SDL_SetWindowFullscreen(s_window, (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    compute_layout();
}

void pc_gfx_set_title(const char *title) {
    SDL_SetWindowTitle(s_window, title);
}

void pc_gfx_on_resize(void) {
    compute_layout();
}

bool pc_gfx_window_to_touch(int wx, int wy, uint16_t *tx, uint16_t *ty) {
    const Rect *r = &s_rect[1];
    if (r->w <= 0 || r->h <= 0 || wx < r->x || wy < r->y || wx >= r->x + r->w || wy >= r->y + r->h) {
        return false;
    }
    *tx = (uint16_t)((wx - r->x) * PC_SCREEN_W / r->w);
    *ty = (uint16_t)((wy - r->y) * PC_SCREEN_H / r->h);
    return true;
}
