/*
 * input_pc.c - keyboard / gamepad -> DS buttons, mouse -> touch panel,
 * hotkeys. Bindings come from config.ini (config_pc.c).
 *
 * The game's own PAD_Read()/TP_* code paths run unchanged: the Nitro layer
 * publishes pc_input_buttons() into the virtual KEYINPUT / X-Y registers
 * once per frame.
 *
 * Default layout (same as most DS emulators):
 *   D-pad  arrows      A  X       B  Z       X  S       Y  A
 *   L      Q           R  W       Start Enter  Select Right Shift / Backspace
 *   Tab fast-forward (held), F8 fast-forward on/off, M mute, F12 screenshot,
 *   F7 picture filter, F9 swap screen layout, F10 close/open lid, F11 fullscreen, Esc quit.
 * Gamepads use the Nintendo *positional* layout (east = A, south = B).
 */
#include "platform_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0755)
#endif

#define MAX_PADS 4

static uint16_t s_kbd_buttons;
static uint16_t s_pad_buttons;
static PcTouch  s_touch;
static bool     s_mouse_down;
static bool     s_lid_closed;
static bool     s_ff_held, s_ff_toggled;
static SDL_GameController *s_pads[MAX_PADS];

static uint16_t key_to_buttons(SDL_Keycode k) {
    const PcConfig *cfg = pc_config();
    uint16_t b = 0;
    for (int i = 0; i < PC_BUTTON_COUNT; i++) {
        for (int j = 0; j < PC_MAX_BINDINGS; j++) {
            if (cfg->keys[i][j] != 0 && cfg->keys[i][j] == (int32_t)k) {
                b |= (uint16_t)(1u << i);
            }
        }
    }
    return b;
}

static int key_to_hotkey(SDL_Keycode k) {
    const PcConfig *cfg = pc_config();
    for (int h = 0; h < PC_HOTKEY_COUNT; h++) {
        for (int j = 0; j < PC_MAX_BINDINGS; j++) {
            if (cfg->hotkeys[h][j] != 0 && cfg->hotkeys[h][j] == (int32_t)k) {
                return h;
            }
        }
    }
    return -1;
}

static void open_pad(int device_index) {
    if (!SDL_IsGameController(device_index)) {
        return;
    }
    for (int i = 0; i < MAX_PADS; i++) {
        if (s_pads[i] == NULL) {
            s_pads[i] = SDL_GameControllerOpen(device_index);
            if (s_pads[i] != NULL) {
                pc_log("gamepad connected: %s", SDL_GameControllerName(s_pads[i]));
            }
            return;
        }
    }
}

static void close_pad(SDL_JoystickID id) {
    for (int i = 0; i < MAX_PADS; i++) {
        if (s_pads[i] != NULL && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pads[i])) == id) {
            SDL_GameControllerClose(s_pads[i]);
            s_pads[i] = NULL;
        }
    }
}

/* Gamepad state is re-read every frame (handles hot-plug + analog stick). */
static uint16_t sample_pads(void) {
    const PcConfig *cfg = pc_config();
    const int dead = 16000;
    uint16_t b = 0;
    for (int p = 0; p < MAX_PADS; p++) {
        SDL_GameController *gc = s_pads[p];
        if (gc == NULL) {
            continue;
        }
        for (int i = 0; i < PC_BUTTON_COUNT; i++) {
            for (int j = 0; j < PC_MAX_BINDINGS; j++) {
                int btn = cfg->pad[i][j];
                bool down = false;
                if (btn == PC_PAD_TRIGGER_LEFT) {
                    down = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > dead;
                } else if (btn == PC_PAD_TRIGGER_RIGHT) {
                    down = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > dead;
                } else if (btn >= 0) {
                    down = SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)btn) != 0;
                }
                if (down) {
                    b |= (uint16_t)(1u << i);
                }
            }
        }
        if (cfg->analogDpad) {
            int lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
            int ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
            if (lx < -dead) b |= PC_PAD_LEFT;
            if (lx >  dead) b |= PC_PAD_RIGHT;
            if (ly < -dead) b |= PC_PAD_UP;
            if (ly >  dead) b |= PC_PAD_DOWN;
        }
    }
    return b;
}

static void update_touch(int wx, int wy) {
    uint16_t tx, ty;
    if (s_mouse_down && pc_gfx_window_to_touch(wx, wy, &tx, &ty)) {
        s_touch.touching = true;
        s_touch.x = tx;
        s_touch.y = ty;
    } else {
        s_touch.touching = false;
    }
}

static void apply_speed(void) {
    int ff = pc_config()->fastForwardSpeed;
    pc_os_set_speed((s_ff_held || s_ff_toggled) ? ff : 1);
}

/* Both screens, top above bottom, in screenshots/ next to the working dir. */
static void take_screenshot(void) {
    static uint32_t both[PC_SCREEN_W * PC_SCREEN_H * 2];
    memcpy(both, pc_gfx_framebuffer(PC_SCREEN_TOP), sizeof both / 2);
    memcpy(both + PC_SCREEN_W * PC_SCREEN_H, pc_gfx_framebuffer(PC_SCREEN_BOTTOM), sizeof both / 2);
    make_dir("screenshots");
    char name[96];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    snprintf(name, sizeof name, "screenshots/hgss_%04d%02d%02d_%02d%02d%02d.png", tm->tm_year + 1900, tm->tm_mon + 1,
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    if (pc_write_png(name, both, PC_SCREEN_W, PC_SCREEN_H * 2)) {
        pc_log("screenshot: %s", name);
    }
}

bool pc_input_init(void) {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        pc_warn("gamepad support unavailable: %s", SDL_GetError());
    }
    memset(s_pads, 0, sizeof s_pads);
    s_kbd_buttons = s_pad_buttons = 0;
    memset(&s_touch, 0, sizeof s_touch);
    pc_config_load();
    return true;
}

void pc_input_shutdown(void) {
    for (int i = 0; i < MAX_PADS; i++) {
        if (s_pads[i] != NULL) {
            SDL_GameControllerClose(s_pads[i]);
            s_pads[i] = NULL;
        }
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

bool pc_input_poll(void) {
    SDL_Event ev;
    bool keep_running = true;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            keep_running = false;
            break;
        case SDL_KEYDOWN: {
            if (ev.key.repeat) {
                break;
            }
            int h = key_to_hotkey(ev.key.keysym.sym);
            switch (h) {
            case PC_HOTKEY_QUIT:          keep_running = false; break;
            case PC_HOTKEY_SWAP_SCREENS:  pc_gfx_set_layout((PcLayout)((pc_gfx_layout() + 1) % PC_LAYOUT_COUNT)); break;
            case PC_HOTKEY_LID:           s_lid_closed = !s_lid_closed; break;
            case PC_HOTKEY_FULLSCREEN:    pc_gfx_toggle_fullscreen(); break;
            case PC_HOTKEY_FAST_FORWARD:  s_ff_held = true; apply_speed(); break;
            case PC_HOTKEY_FAST_FORWARD_TOGGLE:
                s_ff_toggled = !s_ff_toggled;
                apply_speed();
                pc_log("fast-forward %s", s_ff_toggled ? "on" : "off");
                break;
            case PC_HOTKEY_MUTE:
                pc_audio_set_muted(!pc_audio_muted());
                pc_log("sound %s", pc_audio_muted() ? "muted" : "on");
                break;
            case PC_HOTKEY_SCREENSHOT:    take_screenshot(); break;
            case PC_HOTKEY_FILTER:
                pc_gfx_set_filter((pc_gfx_filter() + 1) % PC_FILTER_COUNT);
                pc_log("filter: %s", pc_gfx_filter_name(pc_gfx_filter()));
                break;
            default:                      break;
            }
            s_kbd_buttons |= key_to_buttons(ev.key.keysym.sym);
            break;
        }
        case SDL_KEYUP:
            if (key_to_hotkey(ev.key.keysym.sym) == PC_HOTKEY_FAST_FORWARD) {
                s_ff_held = false;
                apply_speed();
            }
            s_kbd_buttons &= (uint16_t)~key_to_buttons(ev.key.keysym.sym);
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                s_mouse_down = true;
                update_touch(ev.button.x, ev.button.y);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                s_mouse_down = false;
                s_touch.touching = false;
            }
            break;
        case SDL_MOUSEMOTION:
            if (s_mouse_down) {
                update_touch(ev.motion.x, ev.motion.y);
            }
            break;
        case SDL_CONTROLLERDEVICEADDED:
            open_pad(ev.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            close_pad(ev.cdevice.which);
            break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                pc_gfx_on_resize();
            } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                s_kbd_buttons = 0;   /* avoid stuck keys after alt-tab */
                if (s_ff_held) {
                    s_ff_held = false;
                    apply_speed();
                }
            }
            break;
        default:
            break;
        }
    }
    s_pad_buttons = sample_pads();
    return keep_running;
}

uint16_t pc_input_buttons(void) {
    uint16_t b = s_kbd_buttons | s_pad_buttons;
    /* The DS cannot report opposite directions at once; neither should we. */
    if ((b & (PC_PAD_LEFT | PC_PAD_RIGHT)) == (PC_PAD_LEFT | PC_PAD_RIGHT)) {
        b &= (uint16_t)~(PC_PAD_LEFT | PC_PAD_RIGHT);
    }
    if ((b & (PC_PAD_UP | PC_PAD_DOWN)) == (PC_PAD_UP | PC_PAD_DOWN)) {
        b &= (uint16_t)~(PC_PAD_UP | PC_PAD_DOWN);
    }
    return b;
}

static PcTouch s_test_touch;   /* scripted touch (tests), wins over the mouse */

void pc_input_set_test_touch(bool touching, uint16_t x, uint16_t y) {
    s_test_touch.touching = touching;
    s_test_touch.x = x;
    s_test_touch.y = y;
}

PcTouch pc_input_touch(void) {
    return s_test_touch.touching ? s_test_touch : s_touch;
}

bool pc_input_lid_closed(void) {
    /* Test hook: PPL_TEST_LID=FROM:TO closes the lid between V-Blanks FROM
     * and TO (pc_os_vblank_count, which keeps counting while asleep). */
    static long s_test_from = -1, s_test_to;
    if (s_test_from == -1) {
        const char *e = getenv("PPL_TEST_LID");
        s_test_from = -2;
        if (e != NULL && sscanf(e, "%ld:%ld", &s_test_from, &s_test_to) != 2) {
            s_test_from = -2;
        }
    }
    if (s_test_from >= 0) {
        uint64_t v = pc_os_vblank_count();
        if (v >= (uint64_t)s_test_from && v < (uint64_t)s_test_to) {
            return true;
        }
    }
    return s_lid_closed;
}
