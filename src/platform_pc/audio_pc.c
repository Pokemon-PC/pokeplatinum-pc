/*
 * audio_pc.c - SDL2 audio output.
 *
 * Milestone 1 opens a 32768 Hz stereo S16 device (the DS mixer rate) and
 * pulls samples from a registered mixer callback; without one it outputs
 * silence. The device is optional: the game keeps running if no audio
 * device exists (CI, remote desktop...).
 *
 * Milestone 5 registers the software sequencer as the mixer: SDAT parsing
 * (SSEQ/SSAR/SBNK/SWAR/STRM), 16 channels of PCM8/PCM16/IMA-ADPCM/PSG/noise
 * with the DS envelope/LFO/sweep rules, driven by the unchanged
 * NNS_Snd* API (lib/NitroSystem/src/snd_*.c) on top of an SND_* backend.
 */
#include "platform_pc.h"

#include <string.h>

#include <SDL.h>

static SDL_AudioDeviceID s_dev;
static PcAudioMixFn      s_mix;
static void             *s_mix_user;
static volatile bool     s_muted;

static void SDLCALL audio_callback(void *user, Uint8 *stream, int len) {
    int frames = len / (int)(2 * sizeof(int16_t));
    if (s_mix != NULL) {
        s_mix((int16_t *)stream, frames, s_mix_user);
        if (s_muted) {
            memset(stream, 0, (size_t)len);
        }
    } else {
        memset(stream, 0, (size_t)len);
    }
}

static SDL_AudioDeviceID open_device(SDL_AudioSpec *have) {
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq     = PC_AUDIO_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 512;          /* ~15.6 ms at 32768 Hz */
    want.callback = audio_callback;
    return SDL_OpenAudioDevice(NULL, 0, &want, have, 0);
}

bool pc_audio_init(void) {
    SDL_AudioSpec have;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        s_dev = open_device(&have);
        if (s_dev == 0) {
            pc_warn("audio: default driver %s failed: %s", SDL_GetCurrentAudioDriver(), SDL_GetError());
        }
    } else {
        pc_warn("audio: default driver failed: %s", SDL_GetError());
    }

    /* Some setups (e.g. WASAPI with no usable default endpoint) fail the
     * default driver, and that half-initialised driver can then hang the
     * process at exit. Fall back to the other compiled-in drivers.
     * SDL_AUDIODRIVER still forces a specific one. */
    if (s_dev == 0 && SDL_getenv("SDL_AUDIODRIVER") == NULL) {
        const char *failed = SDL_GetCurrentAudioDriver();
        char failed_name[32] = "";
        if (failed != NULL) {
            SDL_strlcpy(failed_name, failed, sizeof failed_name);
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        for (int i = 0; i < SDL_GetNumAudioDrivers() && s_dev == 0; i++) {
            const char *name = SDL_GetAudioDriver(i);
            if (SDL_strcmp(name, failed_name) == 0 || SDL_strcmp(name, "dummy") == 0 ||
                SDL_strcmp(name, "disk") == 0) {
                continue;
            }
            if (SDL_AudioInit(name) == 0) {
                s_dev = open_device(&have);
                if (s_dev == 0) {
                    pc_warn("audio: driver %s failed: %s", name, SDL_GetError());
                    SDL_AudioQuit();
                }
            }
        }
    }
    if (s_dev == 0) {
        pc_warn("audio disabled: no working output driver");
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    pc_log("audio: %d Hz, %d ch, %d-sample buffer (%s)", have.freq, have.channels, have.samples,
           SDL_GetCurrentAudioDriver());
    SDL_PauseAudioDevice(s_dev, 0);
    return true;
}

void pc_audio_shutdown(void) {
    if (s_dev != 0) {
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void pc_audio_set_mixer(PcAudioMixFn fn, void *user) {
    if (s_dev != 0) {
        SDL_LockAudioDevice(s_dev);
    }
    s_mix = fn;
    s_mix_user = user;
    if (s_dev != 0) {
        SDL_UnlockAudioDevice(s_dev);
    }
}

void pc_audio_set_muted(bool muted) {
    s_muted = muted;
}

bool pc_audio_muted(void) {
    return s_muted;
}

bool pc_audio_active(void) {
    return s_dev != 0;
}

/* The mixer callback runs on SDL's audio thread: state it shares with the
 * game thread is touched only between pc_audio_lock() / pc_audio_unlock(). */
void pc_audio_lock(void) {
    if (s_dev != 0) {
        SDL_LockAudioDevice(s_dev);
    }
}

void pc_audio_unlock(void) {
    if (s_dev != 0) {
        SDL_UnlockAudioDevice(s_dev);
    }
}

void pc_audio_pause(bool paused) {
    if (s_dev != 0) {
        SDL_PauseAudioDevice(s_dev, paused ? 1 : 0);
    }
}
