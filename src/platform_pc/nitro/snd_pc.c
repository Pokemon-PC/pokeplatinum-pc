/*
 * snd_pc.c - the DS sound driver (ARM7 side) and its ARM9 API, in software
 * (milestone 6).
 *
 * On the DS, NitroSystem (NNS_Snd*, lib/NitroSystem/src/snd_*.c, unchanged)
 * sends SND_* commands to the ARM7, whose driver plays SSEQ sequences with
 * SBNK instruments and SWAR waves on 16 hardware channels. Here the SND_*
 * functions act on that driver directly, and the driver runs on SDL's audio
 * thread (pc_audio_set_mixer): the audio clock paces the music. Game-thread
 * calls take pc_audio_lock().
 *
 * Driver (GBATEK "DS Sound", SDAT/SSEQ format):
 *   - 16 players, 32 tracks, sequencer at 192 Hz with tempo/tempo ratio,
 *     all SSEQ commands (notes with tie/note-wait, calls, loops, jumps,
 *     variables, random/variable/conditional prefixes, volumes, pan,
 *     transpose, pitch bend, ADSR overrides, LFO, sweep, portamento);
 *   - instruments: PCM, PSG, noise, drum sets, key splits;
 *   - channels: PCM8, PCM16, IMA-ADPCM (with loop state), PSG duty, noise,
 *     DS envelope (attack/decay/sustain/release), priorities, locking;
 *   - direct channels (SND_SetupChannelPcm, wave out), alarms.
 * Not emulated: sound capture (HGSS never starts it: no reverb/effects).
 */
#include "nitro_pc.h"
#include "snd_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define sinf   __builtin_sinf
#define powf   __builtin_powf
#define log10f __builtin_log10f

#define PLAYERS   16
#define TRACKS    32
#define CHANNELS  16
#define ALARMS    8
#define OUT_RATE  32768.0f
/* ARM7 sound frame: 2728 * 64 cycles of the 33.51 MHz bus -> ~192 Hz */
#define FRAME_SEC (2728.0f * 64.0f / 33513982.0f)
#define ARM7_CLOCK 16756991.0f

/* --------------------------------------------------------------- data */

/* volume 0..127 -> 1/10 dB (SNDi_DecibelSquareTable) */
const s16 gPcSndDecibelSquareTable[128] = {
    -32768, -722, -721, -651, -601, -562, -530, -503, -480, -460, -442, -425, -410, -396, -383, -371,
    -360, -349, -339, -330, -321, -313, -305, -297, -289, -282, -276, -269, -263, -257, -251, -245,
    -239, -234, -229, -224, -219, -214, -210, -205, -201, -196, -192, -188, -184, -180, -176, -173,
    -169, -165, -162, -158, -155, -152, -149, -145, -142, -139, -136, -133, -130, -127, -125, -122,
    -119, -116, -114, -111, -109, -106, -103, -101, -99, -96, -94, -91, -89, -87, -85, -82,
    -80, -78, -76, -74, -72, -70, -68, -66, -64, -62, -60, -58, -56, -54, -52, -50,
    -49, -47, -45, -43, -42, -40, -38, -36, -35, -33, -31, -30, -28, -27, -25, -23,
    -22, -20, -19, -17, -16, -14, -13, -11, -10, -8, -7, -6, -4, -3, -1, 0,
};

static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

/* Wave address in a wave archive (SND_GetWaveDataAddress rules). */
static const u8 *wave_address(const u8 *arc, u32 index) {
    if (arc == NULL || index >= rd32(arc + 0x38)) {
        return NULL;
    }
    u32 off = rd32(arc + 0x3C + index * 4);
    if (off == 0) {
        return NULL;
    }
    return off < 0x2000000 ? arc + off : (const u8 *)(uintptr_t)off;
}

typedef struct {
    u8 type;
    u16 wave, waveArc;
    u8 rootKey, attack, decay, sustain, release, pan;
} Inst;

static void read_inst(const u8 *p, u8 type, Inst *in) {
    in->type = type;
    in->wave = rd16(p);
    in->waveArc = rd16(p + 2);
    in->rootKey = p[4];
    in->attack = p[5];
    in->decay = p[6];
    in->sustain = p[7];
    in->release = p[8];
    in->pan = p[9];
}

static BOOL find_inst(const u8 *bank, int prg, int key, Inst *out) {
    if (bank == NULL || prg < 0 || (u32)prg >= rd32(bank + 0x38)) {
        return FALSE;
    }
    u32 off = rd32(bank + 0x3C + (u32)prg * 4);
    u8 type = (u8)off;
    const u8 *p = bank + (off >> 8);
    switch (type) {
    case 1: case 2: case 3: case 4:
        read_inst(p, type, out);
        return TRUE;
    case 16: {   /* drum set */
        int lo = p[0], hi = p[1];
        if (key < lo || key > hi) return FALSE;
        const u8 *e = p + 2 + (key - lo) * 12;
        if (e[0] == 0) return FALSE;
        read_inst(e + 2, e[0], out);
        return TRUE;
    }
    case 17: {   /* key split */
        for (int i = 0; i < 8; i++) {
            if (p[i] == 0) break;
            if (key <= p[i]) {
                const u8 *e = p + 8 + i * 12;
                if (e[0] == 0) return FALSE;
                read_inst(e + 2, e[0], out);
                return TRUE;
            }
        }
        return FALSE;
    }
    default:
        return FALSE;
    }
}

/* volume 0..127 -> dB, the "decibel square" curve of the driver */
static float vol_db(int v) {
    if (v <= 0) return -96.0f;
    if (v > 127) v = 127;
    return (float)gPcSndDecibelSquareTable[v] / 10.0f;
}

/* --------------------------------------------------------------- state */

typedef struct Channel {
    BOOL active, locked, direct;
    int  type;          /* 1 PCM, 2 PSG, 3 noise */
    /* sample source */
    const u8 *data;
    int  format;        /* 0 PCM8, 1 PCM16, 2 ADPCM */
    BOOL loop;
    u32  loopStart, length;   /* in samples */
    double pos;
    float baseRate;     /* Hz at the root key */
    int  duty;
    u16  lfsr;
    /* ADPCM decoder */
    int  adpcmPred, adpcmIdx, adpcmPos;
    int  adpcmLoopPred, adpcmLoopIdx;
    BOOL adpcmLoopSaved;
    s16  adpcmSample;
    /* note */
    int  key, rootKey, velocity;
    s32  noteLength;    /* seq ticks, -1 = until released */
    int  track;         /* owning track, -1 = none/direct */
    int  priority;
    int  instPan;
    /* envelope (1/128 dB) */
    int  envStage;      /* 0 attack 1 decay 2 sustain 3 release */
    s32  envLevel;
    int  attackRate, decayRate, sustainLevel, releaseRate;
    /* sweep / portamento */
    s32  sweepPitch, sweepLength, sweepCounter;
    /* LFO */
    int  lfoDelay, lfoCounter;
    /* direct channel parameters */
    int  dVolume, dShift, dTimer, dPan;
    /* mixing results (computed per frame) */
    float gainL, gainR, step;
} Channel;

typedef struct {
    const u8 *ptr;
    int count;          /* loop count, 0 = infinite */
} StackEntry;

typedef struct Track {
    BOOL active;
    int  player;
    const u8 *base, *cur;
    StackEntry stack[3];
    int  sp;
    s32  wait;
    BOOL noteWait, tie, mute;
    int  prg, vol, vol2, pan, transpose, bend, bendRange, priority;
    int  attack, decay, sustain, release;   /* 255 = instrument's */
    BOOL portaOn;
    int  portaKey, portaTime;
    int  modDepth, modSpeed, modType, modRange, modDelay;
    s16  sweep;
    BOOL cond;
    u32  chMask;        /* allocatable channels */
    int  extraPitch;    /* SND_SetTrackPitch */
    int  extraPan;      /* SND_SetTrackPan */
    int  extraVol;      /* SND_SetTrackVolume: 1/10 dB, 0 = unchanged */
} Track;

typedef struct Player {
    BOOL active, prepared, paused;
    const u8 *bank;
    int  tracks[16];    /* indexes in s_tracks, -1 */
    int  tempo, tempoRatio, tempoCounter;
    u32  ticks;         /* sequencer ticks run (SND_GetPlayerTickCounter) */
    int  volume;        /* SND_SetPlayerVolume: 1/10 dB (0 = full) */
    int  seqVolume;     /* 0xC2 */
    int  priority;
    s16  vars[16];
} Player;

static Channel s_ch[CHANNELS];
static Track   s_tracks[TRACKS];
static Player  s_players[PLAYERS];
static s16     s_globalVars[16];
static int     s_masterVolume = 127;
static int     s_masterPan = -1;     /* SND_SetMasterPan: -1 = stereo, else every channel's pan (mono option) */
static float   s_frameAccum;
static u32     s_rand = 0x12345678;
static u32     s_tag;

typedef struct {
    BOOL active;
    double period, next;   /* seconds */
    void (*callback)(void *);
    void *arg;
    int pending;           /* callbacks to run on the game thread */
} Alarm;
static Alarm s_alarms[ALARMS];
static double s_time;      /* seconds of audio produced */

/* ------------------------------------------------------------ helpers */

static u32 rnd(void) {
    s_rand = s_rand * 1664525u + 1013904223u;
    return s_rand >> 16;
}

static u32 read_varlen(const u8 **pp) {
    u32 v = 0;
    u8 b;
    do {
        b = *(*pp)++;
        v = (v << 7) | (b & 0x7F);
    } while (b & 0x80);
    return v;
}

static int attack_rate(int a) {
    static const u8 tbl[19] = { 0, 1, 5, 14, 26, 38, 51, 63, 73, 84, 92, 100, 109, 116, 123, 127, 132, 137, 143 };
    /* 109..127 come from a table indexed by 127 - a (127 = instant) */
    if (a >= 109) return tbl[127 - (a > 127 ? 127 : a)];
    return 255 - a;
}

static int decay_rate(int d) {
    if (d == 127) return 0xFFFF;
    if (d == 126) return 0x3C00;
    if (d < 50) return d * 2 + 1;
    return 0x1E00 / (126 - d);
}

static s32 sustain_level(int s) {
    return (s32)(vol_db(s) * 128.0f);
}

#define ENV_MIN (-92544)

/* ------------------------------------------------------------ channels */

static void channel_stop(Channel *c) {
    c->active = FALSE;
    c->track = -1;
}

static void channel_release(Channel *c) {
    if (c->active && c->envStage != 3) {
        c->envStage = 3;
    }
}

static int alloc_channel(u32 mask, int type, int prio) {
    if (type == 2) mask &= 0x3F00;       /* PSG: 8-13 */
    else if (type == 3) mask &= 0xC000;  /* noise: 14-15 */
    int best = -1;
    for (int i = 0; i < CHANNELS; i++) {
        if (!(mask & (1u << i)) || s_ch[i].locked) continue;
        if (!s_ch[i].active) return i;
    }
    /* steal the lowest priority (released first) */
    int bestScore = 0x7FFFFFFF;
    for (int i = 0; i < CHANNELS; i++) {
        if (!(mask & (1u << i)) || s_ch[i].locked || s_ch[i].direct) continue;
        int score = s_ch[i].priority * 4 + (s_ch[i].envStage == 3 ? 0 : 2);
        if (score < bestScore && s_ch[i].priority <= prio) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

static void adpcm_reset(Channel *c) {
    c->adpcmPred = (s16)rd16(c->data);
    c->adpcmIdx = c->data[2] & 0x7F;
    c->adpcmPos = 0;
    c->adpcmSample = (s16)c->adpcmPred;
    c->adpcmLoopSaved = FALSE;
}

static const s16 s_adpcmStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};
static const s8 s_adpcmIndex[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static void adpcm_step(Channel *c) {
    if (c->loop && !c->adpcmLoopSaved && (u32)c->adpcmPos == c->loopStart) {
        c->adpcmLoopPred = c->adpcmPred;
        c->adpcmLoopIdx = c->adpcmIdx;
        c->adpcmLoopSaved = TRUE;
    }
    u8 b = c->data[4 + c->adpcmPos / 2];
    int n = (c->adpcmPos & 1) ? b >> 4 : b & 15;
    int step = s_adpcmStep[c->adpcmIdx];
    int diff = step >> 3;
    if (n & 1) diff += step >> 2;
    if (n & 2) diff += step >> 1;
    if (n & 4) diff += step;
    if (n & 8) { c->adpcmPred -= diff; if (c->adpcmPred < -32767) c->adpcmPred = -32767; }
    else { c->adpcmPred += diff; if (c->adpcmPred > 32767) c->adpcmPred = 32767; }
    c->adpcmIdx += s_adpcmIndex[n & 7];
    if (c->adpcmIdx < 0) c->adpcmIdx = 0;
    if (c->adpcmIdx > 88) c->adpcmIdx = 88;
    c->adpcmSample = (s16)c->adpcmPred;
    c->adpcmPos++;
}

/* One output sample of a channel, in -1..1. */
static float channel_sample(Channel *c) {
    float v = 0;
    if (c->type == 2) {          /* PSG square, 8 steps */
        int phase = (int)c->pos & 7;
        v = phase <= c->duty ? 1.0f : -1.0f;
        c->pos += c->step;
        if (c->pos >= 8.0) c->pos -= 8.0 * (int)(c->pos / 8.0);
        return v * 0.5f;
    }
    if (c->type == 3) {          /* noise */
        int n = (int)c->pos;
        c->pos += c->step;
        int adv = (int)c->pos - n;
        while (adv-- > 0) {
            u16 bit = c->lfsr & 1;
            c->lfsr >>= 1;
            if (bit) c->lfsr ^= 0x6000;
        }
        return (c->lfsr & 1) ? 0.5f : -0.5f;
    }
    u32 i = (u32)c->pos;
    if (i >= c->length) {
        if (!c->loop || c->length <= c->loopStart) {
            channel_stop(c);
            return 0;
        }
        u32 span = c->length - c->loopStart;
        double over = c->pos - (double)c->length;
        c->pos = (double)c->loopStart + (over - span * (int)(over / span));
        i = (u32)c->pos;
        if (c->format == 2 && c->adpcmLoopSaved) {
            c->adpcmPred = c->adpcmLoopPred;
            c->adpcmIdx = c->adpcmLoopIdx;
            c->adpcmPos = (int)c->loopStart;
        }
    }
    switch (c->format) {
    case 0: v = (float)(s8)c->data[i] / 128.0f; break;
    case 1: v = (float)(s16)rd16(c->data + i * 2) / 32768.0f; break;
    default:
        if ((int)i < c->adpcmPos - 1) {   /* went backwards without loop state */
            adpcm_reset(c);
        }
        while (c->adpcmPos <= (int)i) adpcm_step(c);
        v = (float)c->adpcmSample / 32768.0f;
        break;
    }
    c->pos += c->step;
    return v;
}

/* ------------------------------------------------------------ notes */

static void start_note(Track *t, Player *pl, int key, int vel, s32 length) {
    Inst in;
    if (!find_inst(pl->bank, t->prg, key, &in)) {
        return;
    }
    int type = in.type == 4 ? 1 : in.type;
    if (type < 1 || type > 3) return;

    Channel *c = NULL;
    int ti = (int)(t - s_tracks);
    if (t->tie) {
        for (int i = 0; i < CHANNELS; i++) {
            if (s_ch[i].active && s_ch[i].track == ti && s_ch[i].envStage != 3) {
                c = &s_ch[i];
                c->key = key;
                c->velocity = vel;
                c->noteLength = length;
                return;
            }
        }
    }
    int prio = pl->priority + t->priority;
    if (prio > 255) prio = 255;
    int idx = alloc_channel(t->chMask, type, prio);
    if (idx < 0) return;
    c = &s_ch[idx];
    memset(c, 0, sizeof *c);
    c->active = TRUE;
    c->type = type;
    c->track = ti;
    c->priority = prio;
    c->key = key;
    c->rootKey = in.rootKey;
    c->velocity = vel;
    c->noteLength = length;
    c->instPan = in.pan;
    c->lfsr = 0x7FFF;
    if (type == 1) {
        const u8 *arcLink = pl->bank + 0x18 + (u32)in.waveArc * 8;
        const u8 *arc = (const u8 *)(uintptr_t)rd32(arcLink);
        const u8 *w = wave_address(arc, in.wave);
        if (w == NULL) { c->active = FALSE; return; }
        c->format = w[0];
        c->loop = w[1];
        u16 timer = rd16(w + 4);
        u32 loopStartWords = rd16(w + 6);
        u32 loopLenWords = rd32(w + 8);
        c->data = w + 12;
        /* the wave's timer is the sample period in ARM7 clocks */
        c->baseRate = ARM7_CLOCK / (float)(timer ? timer : 1);
        u32 words = loopStartWords + loopLenWords;
        if (c->format == 0) { c->length = words * 4; c->loopStart = loopStartWords * 4; }
        else if (c->format == 1) { c->length = words * 2; c->loopStart = loopStartWords * 2; }
        else { c->length = (words - 1) * 8; c->loopStart = loopStartWords > 0 ? (loopStartWords - 1) * 8 : 0; adpcm_reset(c); }
    } else {
        /* PSG / noise: one duty step (or LFSR clock) every 8006 ARM7 clocks at the
           root key, i.e. a 261.6 Hz square wave (C4) */
        if (type == 2) c->duty = in.wave;
        c->baseRate = ARM7_CLOCK / 8006.0f;
    }
    int a = t->attack != 255 ? t->attack : in.attack;
    int d = t->decay != 255 ? t->decay : in.decay;
    int s = t->sustain != 255 ? t->sustain : in.sustain;
    int r = t->release != 255 ? t->release : in.release;
    c->attackRate = attack_rate(a);
    c->decayRate = decay_rate(d);
    c->sustainLevel = sustain_level(s);
    c->releaseRate = decay_rate(r);
    c->envStage = 0;
    c->envLevel = ENV_MIN;
    c->lfoDelay = t->modDelay;
    /* sweep: explicit sweep pitch plus portamento from the previous key */
    c->sweepPitch = t->sweep;
    if (t->portaOn) {
        c->sweepPitch += (t->portaKey - key) * 64;
    }
    c->sweepLength = t->portaTime > 0 ? t->portaTime * t->portaTime * (c->sweepPitch < 0 ? -c->sweepPitch : c->sweepPitch) >> 11
                                      : length;
    c->sweepCounter = 0;
    t->portaKey = key;
}

/* ------------------------------------------------------------ tracks */

static void track_init(Track *t, int player, const u8 *base, const u8 *cur) {
    memset(t, 0, sizeof *t);
    t->active = TRUE;
    t->player = player;
    t->base = base;
    t->cur = cur;
    t->noteWait = TRUE;
    t->vol = 127;
    t->vol2 = 127;
    t->pan = 64;
    t->bendRange = 2;
    t->priority = 64;
    t->attack = t->decay = t->sustain = t->release = 255;
    t->modSpeed = 16;
    t->modRange = 1;
    t->portaKey = 60;
    t->chMask = 0xFFFF;
}

static int alloc_track(void) {
    for (int i = 0; i < TRACKS; i++) {
        if (!s_tracks[i].active) return i;
    }
    return -1;
}

static void track_release_channels(int ti, BOOL stop) {
    for (int i = 0; i < CHANNELS; i++) {
        if (s_ch[i].active && s_ch[i].track == ti) {
            if (stop) channel_stop(&s_ch[i]);
            else channel_release(&s_ch[i]);
        }
    }
}

static s16 *var_ptr(Player *pl, int n) {
    return n < 16 ? &pl->vars[n] : &s_globalVars[n & 15];
}

/* Runs the track until it waits. Returns FALSE when it ended. */
static BOOL track_step(Track *t, Player *pl) {
    int guard = 0;
    while (t->wait <= 0) {
        if (++guard > 10000) return FALSE;   /* runaway data */
        const u8 *p = t->cur;
        u8 cmd = *p++;
        int argMode = 0;    /* 0 normal, 1 random, 2 variable, 3 if */
        if (cmd == 0xA0 || cmd == 0xA1 || cmd == 0xA2) {
            argMode = cmd == 0xA0 ? 1 : cmd == 0xA1 ? 2 : 3;
            cmd = *p++;
            if (argMode == 3 && !t->cond) {
                argMode = 4;   /* skip the command */
            }
        }
#define LAST_ARG(normal_expr, type_)                                                     \
    (argMode == 1 ? (type_)(s16)(rd16(p) + rnd() % ((s16)rd16(p + 2) - (s16)rd16(p) + 1)) \
     : argMode == 2 ? (type_)(*var_ptr(pl, p[0]))                                         \
                    : (type_)(normal_expr))
#define SKIP_LAST(n) (p += argMode == 1 ? 4 : argMode == 2 ? 1 : (n))

        if (cmd < 0x80) {   /* note */
            int vel = *p++;
            const u8 *q = p;
            s32 len;
            if (argMode == 1 || argMode == 2) { len = LAST_ARG(0, s32); SKIP_LAST(0); }
            else { len = (s32)read_varlen(&q); p = q; }
            t->cur = p;
            if (argMode == 4) continue;
            int key = cmd + t->transpose;
            if (key < 0) key = 0;
            if (key > 127) key = 127;
            if (!t->mute) start_note(t, pl, key, vel, len > 0 ? len : -1);
            if (t->noteWait) t->wait = len;
            continue;
        }
        switch (cmd) {
        case 0x80: case 0x81: {
            s32 v;
            if (argMode == 1 || argMode == 2) { v = LAST_ARG(0, s32); SKIP_LAST(0); }
            else v = (s32)read_varlen(&p);
            t->cur = p;
            if (argMode == 4) break;
            if (cmd == 0x80) t->wait = v;
            else t->prg = v;
            break;
        }
        case 0x93: {
            int tn = p[0];
            u32 off = p[1] | p[2] << 8 | p[3] << 16;
            p += 4;
            t->cur = p;
            if (argMode == 4) break;
            if (tn < 16 && pl->tracks[tn] < 0) {
                int ni = alloc_track();
                if (ni >= 0) {
                    track_init(&s_tracks[ni], t->player, t->base, t->base + off);
                    pl->tracks[tn] = ni;
                }
            } else if (tn < 16) {
                Track *o = &s_tracks[pl->tracks[tn]];
                o->cur = t->base + off;
            }
            break;
        }
        case 0x94: {
            u32 off = p[0] | p[1] << 8 | p[2] << 16;
            p += 3;
            t->cur = argMode == 4 ? p : t->base + off;
            break;
        }
        case 0x95: {
            u32 off = p[0] | p[1] << 8 | p[2] << 16;
            p += 3;
            if (argMode == 4 || t->sp >= 3) { t->cur = p; break; }
            t->stack[t->sp].ptr = p;
            t->stack[t->sp].count = 0;
            t->sp++;
            t->cur = t->base + off;
            break;
        }
        case 0xFD:
            t->cur = p;
            if (argMode == 4) break;
            if (t->sp > 0) t->cur = t->stack[--t->sp].ptr;
            break;
        case 0xD4: {
            int n = LAST_ARG(p[0], int);
            SKIP_LAST(1);
            t->cur = p;
            if (argMode == 4 || t->sp >= 3) break;
            t->stack[t->sp].ptr = p;
            t->stack[t->sp].count = n;
            t->sp++;
            break;
        }
        case 0xFC:
            t->cur = p;
            if (argMode == 4 || t->sp == 0) break;
            {
                StackEntry *e = &t->stack[t->sp - 1];
                if (e->count == 0) { t->cur = e->ptr; break; }   /* infinite */
                if (--e->count > 0) t->cur = e->ptr;
                else t->sp--;
            }
            break;
        case 0xFE:
            p += 2;
            t->cur = p;
            break;
        case 0xFF:
            t->cur = p;
            if (argMode == 4) break;
            return FALSE;
        case 0xE0: case 0xE1: case 0xE3: {
            s16 v = LAST_ARG((s16)rd16(p), s16);
            SKIP_LAST(2);
            t->cur = p;
            if (argMode == 4) break;
            if (cmd == 0xE0) t->modDelay = v;
            else if (cmd == 0xE1) pl->tempo = (u16)v;
            else t->sweep = v;
            break;
        }
        default:
            if (cmd >= 0xB0 && cmd <= 0xBD) {
                int vn = *p++;
                s16 v = LAST_ARG((s16)rd16(p), s16);
                SKIP_LAST(2);
                t->cur = p;
                if (argMode == 4) break;
                s16 *var = var_ptr(pl, vn);
                switch (cmd) {
                case 0xB0: *var = v; break;
                case 0xB1: *var = (s16)(*var + v); break;
                case 0xB2: *var = (s16)(*var - v); break;
                case 0xB3: *var = (s16)(*var * v); break;
                case 0xB4: if (v != 0) *var = (s16)(*var / v); break;
                case 0xB5: *var = (s16)(v >= 0 ? *var << v : *var >> -v); break;
                case 0xB6: *var = (s16)(v >= 0 ? (s16)(rnd() % (v + 1)) : -(s16)(rnd() % (-v + 1))); break;
                case 0xB8: t->cond = *var == v; break;
                case 0xB9: t->cond = *var >= v; break;
                case 0xBA: t->cond = *var > v; break;
                case 0xBB: t->cond = *var <= v; break;
                case 0xBC: t->cond = *var < v; break;
                case 0xBD: t->cond = *var != v; break;
                }
                break;
            }
            if (cmd >= 0xC0 && cmd <= 0xDF) {
                int v = LAST_ARG(p[0], int);
                SKIP_LAST(1);
                t->cur = p;
                if (argMode == 4) break;
                switch (cmd) {
                case 0xC0: t->pan = v & 0x7F; break;
                case 0xC1: t->vol = v & 0x7F; break;
                case 0xC2: pl->seqVolume = v & 0x7F; break;
                case 0xC3: t->transpose = (s8)v; break;
                case 0xC4: t->bend = (s8)v; break;
                case 0xC5: t->bendRange = v; break;
                case 0xC6: t->priority = v; break;
                case 0xC7: t->noteWait = v & 1; break;
                case 0xC8: t->tie = v & 1; if (!t->tie) track_release_channels((int)(t - s_tracks), FALSE); break;
                case 0xC9: t->portaKey = v + t->transpose; t->portaOn = TRUE; break;
                case 0xCA: t->modDepth = v; break;
                case 0xCB: t->modSpeed = v; break;
                case 0xCC: t->modType = v; break;
                case 0xCD: t->modRange = v; break;
                case 0xCE: t->portaOn = v & 1; break;
                case 0xCF: t->portaTime = v; break;
                case 0xD0: t->attack = v; break;
                case 0xD1: t->decay = v; break;
                case 0xD2: t->sustain = v; break;
                case 0xD3: t->release = v; break;
                case 0xD5: t->vol2 = v & 0x7F; break;
                case 0xD6: break;   /* print variable */
                case 0xD7: t->mute = v & 1; break;
                default: break;
                }
                break;
            }
            /* unknown command: stop the track rather than run wild */
            return FALSE;
        }
#undef LAST_ARG
#undef SKIP_LAST
    }
    return TRUE;
}

static void player_stop(int pn) {
    Player *pl = &s_players[pn];
    for (int i = 0; i < 16; i++) {
        if (pl->tracks[i] >= 0) {
            track_release_channels(pl->tracks[i], FALSE);
            s_tracks[pl->tracks[i]].active = FALSE;
            pl->tracks[i] = -1;
        }
    }
    pl->active = FALSE;
    pl->prepared = FALSE;
}

/* One sequencer tick (tempo-scaled). */
static void player_seq_tick(int pn) {
    Player *pl = &s_players[pn];
    BOOL any = FALSE;
    for (int i = 0; i < 16; i++) {
        int ti = pl->tracks[i];
        if (ti < 0) continue;
        Track *t = &s_tracks[ti];
        if (!t->active) continue;
        /* note lengths */
        for (int c = 0; c < CHANNELS; c++) {
            Channel *ch = &s_ch[c];
            if (ch->active && ch->track == ti && ch->noteLength > 0) {
                if (--ch->noteLength == 0) channel_release(ch);
            }
        }
        if (t->wait > 0) t->wait--;
        if (t->wait <= 0 && !track_step(t, pl)) {
            track_release_channels(ti, FALSE);
            t->active = FALSE;
            continue;
        }
        any = TRUE;
    }
    if (!any) {
        player_stop(pn);
    }
}

/* ----------------------------------------------------------- 192 Hz frame */

static void channel_frame(Channel *c) {
    /* envelope */
    switch (c->envStage) {
    case 0:
        c->envLevel = (c->attackRate * c->envLevel) / 256;
        if (c->envLevel >= -1) { c->envLevel = 0; c->envStage = 1; }
        break;
    case 1:
        c->envLevel -= c->decayRate;
        if (c->envLevel <= c->sustainLevel) { c->envLevel = c->sustainLevel; c->envStage = 2; }
        break;
    case 2:
        break;
    case 3:
        c->envLevel -= c->releaseRate;
        if (c->envLevel <= ENV_MIN) { channel_stop(c); return; }
        break;
    }
    if (c->direct) {
        /* SND_SetupChannelPcm: volume 0..127 with shift (0,1,2,4 -> /1,/2,/4,/16) */
        static const float shiftDiv[4] = { 1.0f, 2.0f, 4.0f, 16.0f };
        float g = (float)c->dVolume / 127.0f / shiftDiv[c->dShift & 3];
        float pan = (float)c->dPan / 127.0f;
        float master = (float)s_masterVolume / 127.0f;
        c->gainL = g * (1.0f - pan) * master;
        c->gainR = g * pan * master;
        if (s_masterPan >= 0) {
            float m = (c->gainL + c->gainR) * 0.5f;
            c->gainL = c->gainR = m;
        }
        c->step = (ARM7_CLOCK / (float)(c->dTimer > 0 ? c->dTimer : 1)) / OUT_RATE;   /* period */
        return;
    }
    Track *t = c->track >= 0 ? &s_tracks[c->track] : NULL;
    Player *pl = t != NULL ? &s_players[t->player] : NULL;
    if (t == NULL || pl == NULL) { channel_stop(c); return; }

    /* LFO */
    float lfo = 0;
    if (t->modDepth > 0) {
        if (c->lfoDelay > 0) c->lfoDelay--;
        else {
            c->lfoCounter += t->modSpeed << 6;
            float ph = (float)(c->lfoCounter & 0xFFFF) / 65536.0f * 6.2831853f;
            lfo = sinf(ph) * (float)t->modDepth / 127.0f * (float)t->modRange;
        }
    }
    /* volume (dB) */
    float db = vol_db(c->velocity) + vol_db(t->vol) + vol_db(t->vol2) + (float)pl->volume / 10.0f + vol_db(pl->seqVolume) +
               (float)t->extraVol / 10.0f +
               (float)c->envLevel / 128.0f;
    if (t->modType == 1) db += lfo * 6.0f;
    float gain = db <= -90.0f ? 0.0f : powf(10.0f, db / 20.0f);
    gain *= (float)s_masterVolume / 127.0f;
    /* pan */
    int pan = t->pan + (c->instPan - 64) + t->extraPan;
    float fpan = (float)pan;
    if (t->modType == 2) fpan += lfo * 64.0f;
    if (fpan < 0) fpan = 0;
    if (fpan > 127) fpan = 127;
    fpan /= 127.0f;
    c->gainL = gain * (1.0f - fpan);
    c->gainR = gain * fpan;
    /* pitch, in 1/64 semitone */
    float pitch = (float)((c->key - c->rootKey) * 64) + (float)(t->bend * t->bendRange) / 2.0f + (float)t->extraPitch;
    if (c->sweepPitch != 0 && c->sweepLength > 0) {
        if (c->sweepCounter < c->sweepLength) c->sweepCounter++;
        pitch += (float)c->sweepPitch * (1.0f - (float)c->sweepCounter / (float)c->sweepLength);
    }
    if (t->modType == 0) pitch += lfo * 64.0f;
    float ratio = powf(2.0f, pitch / 768.0f);
    c->step = c->baseRate * ratio / OUT_RATE;
    if (s_masterPan >= 0) {   /* mono */
        float g = (c->gainL + c->gainR) * 0.5f;
        c->gainL = c->gainR = g;
    }
}

static void sound_frame(void) {
    for (int i = 0; i < CHANNELS; i++) {
        if (s_ch[i].active) channel_frame(&s_ch[i]);
    }
    for (int pn = 0; pn < PLAYERS; pn++) {
        Player *pl = &s_players[pn];
        if (!pl->active || pl->paused || pl->prepared) continue;   /* prepared: waits for the start */
        pl->tempoCounter += pl->tempo * pl->tempoRatio / 256;
        while (pl->tempoCounter >= 240 && pl->active) {
            pl->tempoCounter -= 240;
            pl->ticks++;
            player_seq_tick(pn);
        }
    }
}

static void mix(int16_t *out, int frames, void *user) {
    (void)user;
    for (int f = 0; f < frames; f++) {
        s_frameAccum += 1.0f / OUT_RATE;
        if (s_frameAccum >= FRAME_SEC) {
            s_frameAccum -= FRAME_SEC;
            sound_frame();
        }
        float l = 0, r = 0;
        for (int i = 0; i < CHANNELS; i++) {
            Channel *c = &s_ch[i];
            if (!c->active) continue;
            float s = channel_sample(c);
            l += s * c->gainL;
            r += s * c->gainR;
        }
        l *= 0.7f;
        r *= 0.7f;
        if (l > 1) l = 1;
        if (l < -1) l = -1;
        if (r > 1) r = 1;
        if (r < -1) r = -1;
        if (out != NULL) {
            out[f * 2] = (int16_t)(l * 32767.0f);
            out[f * 2 + 1] = (int16_t)(r * 32767.0f);
        }
        s_time += 1.0 / OUT_RATE;
    }
    /* PPL_AUDIO_DUMP=file: raw 32768 Hz stereo s16 output, for tests */
    static FILE *dump;
    static int dump_state;
    if (dump_state == 0) {
        const char *d = getenv("PPL_AUDIO_DUMP");
        dump = d != NULL ? fopen(d, "wb") : NULL;
        dump_state = 1;
    }
    if (dump != NULL && out != NULL) {
        fwrite(out, 4, (size_t)frames, dump);
        fflush(dump);
    }
    /* PPL_MUTE=1: the driver runs (and PPL_AUDIO_DUMP records) but the
     * speakers stay silent - for automated tests. */
    static int mute = -1;
    if (mute < 0) {
        const char *m = getenv("PPL_MUTE");
        mute = m != NULL && m[0] != '\0' && m[0] != '0';
    }
    if (mute && out != NULL) {
        memset(out, 0, (size_t)frames * 4);
    }
    for (int i = 0; i < ALARMS; i++) {
        Alarm *a = &s_alarms[i];
        while (a->active && a->period > 0 && s_time >= a->next) {
            a->pending++;
            a->next += a->period;
        }
    }
}

/* ============================================================ public API */

#define LOCK()   pc_audio_lock()
#define UNLOCK() pc_audio_unlock()

void SND_Init(void) {
    LOCK();
    memset(s_ch, 0, sizeof s_ch);
    memset(s_tracks, 0, sizeof s_tracks);
    for (int i = 0; i < CHANNELS; i++) s_ch[i].track = -1;
    for (int p = 0; p < PLAYERS; p++) {
        memset(&s_players[p], 0, sizeof s_players[p]);
        for (int i = 0; i < 16; i++) s_players[p].tracks[i] = -1;
        s_players[p].volume = 0;
        s_players[p].tempoRatio = 256;
    }
    memset(s_globalVars, 0, sizeof s_globalVars);
    memset(s_alarms, 0, sizeof s_alarms);
    s_masterVolume = 127;
    UNLOCK();
    pc_audio_set_mixer(mix, NULL);
}

/* Commands complete immediately. */
const SNDCommand *SND_RecvCommandReply(u32 flags) { (void)flags; return NULL; }
BOOL SND_FlushCommand(u32 flags) { (void)flags; s_tag++; return TRUE; }
void SND_WaitForCommandProc(u32 tag) { (void)tag; }
u32 SND_GetCurrentCommandTag(void) { return s_tag; }
BOOL SND_IsFinishedCommandTag(u32 tag) { (void)tag; return TRUE; }

/* Driver info snapshot: our own layout inside SndDriverInfo (0x11E0 bytes). */
typedef struct {
    u16 prg;
    u8 vol, vol2, range, pan, chCount, valid;
    s8 bend, transpose;
} TrackSnap;

typedef struct {
    SNDChannelInfo ch[CHANNELS];
    TrackSnap track[PLAYERS][16];
} DriverSnapshot;
_Static_assert(sizeof(DriverSnapshot) <= 0x11E0, "driver snapshot too large");

void SND_ReadDriverInfo(SNDDriverInfo *info) {
    DriverSnapshot snap;
    memset(&snap, 0, sizeof snap);
    LOCK();
    for (int i = 0; i < CHANNELS; i++) {
        snap.ch[i].activeFlag = s_ch[i].active;
        snap.ch[i].lockFlag = s_ch[i].locked;
        snap.ch[i].volume = (u16)(s_ch[i].gainL * 127.0f + s_ch[i].gainR * 127.0f);
        snap.ch[i].pan = 64;
        snap.ch[i].envStatus = (SNDEnvStatus)s_ch[i].envStage;
    }
    for (int p = 0; p < PLAYERS; p++) {
        for (int t = 0; t < 16; t++) {
            int ti = s_players[p].tracks[t];
            if (ti < 0 || !s_tracks[ti].active) continue;
            Track *tr = &s_tracks[ti];
            TrackSnap *o = &snap.track[p][t];
            o->valid = 1;
            o->prg = (u16)tr->prg;
            o->vol = (u8)tr->vol;
            o->vol2 = (u8)tr->vol2;
            o->bend = (s8)tr->bend;
            o->range = (u8)tr->bendRange;
            o->pan = (u8)tr->pan;
            o->transpose = (s8)tr->transpose;
            for (int c = 0; c < CHANNELS; c++) {
                if (s_ch[c].active && s_ch[c].track == ti) o->chCount++;
            }
        }
    }
    UNLOCK();
    memcpy(info, &snap, sizeof snap);
}

BOOL SND_ReadChannelInfo(const SNDDriverInfo *driverInfo, int chNo, SNDChannelInfo *chInfo) {
    if (chNo < 0 || chNo >= CHANNELS) return FALSE;
    const DriverSnapshot *s = (const DriverSnapshot *)driverInfo;
    *chInfo = s->ch[chNo];
    return TRUE;
}

BOOL SND_ReadTrackInfo(const SNDDriverInfo *driverInfo, int playerNo, int trackNo, SNDTrackInfo *trackInfo) {
    if (playerNo < 0 || playerNo >= PLAYERS || trackNo < 0 || trackNo >= 16) return FALSE;
    const DriverSnapshot *s = (const DriverSnapshot *)driverInfo;
    const TrackSnap *o = &s->track[playerNo][trackNo];
    if (!o->valid) return FALSE;
    memset(trackInfo, 0, sizeof *trackInfo);
    trackInfo->prgNo = o->prg;
    trackInfo->volume = o->vol;
    trackInfo->volume2 = o->vol2;
    trackInfo->pitchBend = o->bend;
    trackInfo->bendRange = o->range;
    trackInfo->pan = o->pan;
    trackInfo->transpose = o->transpose;
    trackInfo->chCount = o->chCount;
    return TRUE;
}

u32 SND_GetPlayerStatus(void) {
    u32 m = 0;
    LOCK();
    for (int p = 0; p < PLAYERS; p++) {
        if (s_players[p].active) m |= 1u << p;
    }
    UNLOCK();
    return m;
}

u32 SND_GetChannelStatus(void) {
    u16 m = 0;
    LOCK();
    for (int i = 0; i < CHANNELS; i++) {
        if (s_ch[i].active) m |= (u16)(1u << i);
    }
    UNLOCK();
    return m;
}

s16 SND_GetPlayerLocalVariable(int playerNo, int varNo) {
    if ((u32)playerNo >= PLAYERS || varNo < 0 || varNo >= 16) return 0;
    LOCK();
    int v = s_players[playerNo].vars[varNo];
    UNLOCK();
    return v;
}

/* SND_CalcChannelVolume, SND_CalcTimer, decibel tables: NitroSDK snd_util.c (compiled as is). */

void SND_SetMasterVolume(s32 volume) { LOCK(); s_masterVolume = volume < 0 ? 0 : volume > 127 ? 127 : volume; UNLOCK(); }
void SND_SetMasterPan(s32 pan) { LOCK(); s_masterPan = pan < 0 ? 0 : pan > 127 ? 127 : pan; UNLOCK(); }
void SND_ResetMasterPan(void) { LOCK(); s_masterPan = -1; UNLOCK(); }
void SND_SetOutputSelector(SNDOutput left, SNDOutput right, SNDChannelOut channel1, SNDChannelOut channel3) { (void)left; (void)right; (void)channel1; (void)channel3; }

void SND_StartTimer(u32 chBitMask, u32 capBitMask, u32 alarmBitMask, u32 flags) {
    (void)capBitMask; (void)flags;
    LOCK();
    for (int i = 0; i < CHANNELS; i++) {
        if ((chBitMask & (1u << i)) && s_ch[i].direct) {
            s_ch[i].active = TRUE;
            channel_frame(&s_ch[i]);
        }
    }
    for (int i = 0; i < ALARMS; i++) {
        if (alarmBitMask & (1u << i)) {
            s_alarms[i].active = TRUE;
        }
    }
    UNLOCK();
}

void SND_StopTimer(u32 chBitMask, u32 capBitMask, u32 alarmBitMask, u32 flags) {
    (void)capBitMask; (void)flags;
    LOCK();
    for (int i = 0; i < CHANNELS; i++) {
        if (chBitMask & (1u << i)) channel_stop(&s_ch[i]);
    }
    for (int i = 0; i < ALARMS; i++) {
        if (alarmBitMask & (1u << i)) { s_alarms[i].active = FALSE; s_alarms[i].pending = 0; }
    }
    UNLOCK();
}

void SND_SetupAlarm(s32 alarmNo, u32 tick, u32 period, void (*callback)(void *arg), void *arg) {
    if (alarmNo < 0 || alarmNo >= ALARMS) return;
    LOCK();
    Alarm *a = &s_alarms[alarmNo];
    /* tick/period in ARM7 timer units (64 cycles of 33.51 MHz) */
    a->period = (double)period * 64.0 / 33513982.0;
    a->next = s_time + (double)tick * 64.0 / 33513982.0;
    a->callback = callback;
    a->arg = arg;
    a->pending = 0;
    a->active = FALSE;
    UNLOCK();
}

void SND_LockChannel(u32 chBitMask, u32 flags) {
    (void)flags;
    LOCK();
    for (int i = 0; i < CHANNELS; i++) {
        if (chBitMask & (1u << i)) {
            if (s_ch[i].active && !s_ch[i].direct) channel_stop(&s_ch[i]);
            s_ch[i].locked = TRUE;
        }
    }
    UNLOCK();
}

void SND_UnlockChannel(u32 chBitMask, u32 flags) {
    (void)flags;
    LOCK();
    for (int i = 0; i < CHANNELS; i++) {
        if (chBitMask & (1u << i)) {
            s_ch[i].locked = FALSE;
            s_ch[i].direct = FALSE;
        }
    }
    UNLOCK();
}

void SND_SetupChannelPcm(int chNo, SNDWaveFormat format, const void *data, SNDChannelLoop loopMode, int loopStart, int dataLen,
                         int volume, SNDChannelDataShift shift, int timer, int pan) {
    if (chNo < 0 || chNo >= CHANNELS) return;
    LOCK();
    Channel *c = &s_ch[chNo];
    BOOL locked = c->locked;
    memset(c, 0, sizeof *c);
    c->locked = locked;
    c->direct = TRUE;
    c->track = -1;
    c->type = 1;
    c->format = format;
    c->data = data;
    c->loop = loopMode == 1;
    /* loopStart / dataLen in words */
    if (format == 0) { c->loopStart = (u32)loopStart * 4; c->length = (u32)(loopStart + dataLen) * 4; }
    else if (format == 1) { c->loopStart = (u32)loopStart * 2; c->length = (u32)(loopStart + dataLen) * 2; }
    else { c->loopStart = loopStart > 0 ? (u32)(loopStart - 1) * 8 : 0; c->length = (u32)(loopStart + dataLen - 1) * 8; adpcm_reset(c); }
    c->envStage = 2;
    c->envLevel = 0;
    c->dVolume = volume;
    c->dShift = shift;
    c->dTimer = timer;
    c->dPan = pan;
    c->active = FALSE;   /* SND_StartTimer starts it */
    UNLOCK();
}

void SND_SetChannelTimer(u32 chBitMask, s32 timer) {
    LOCK();
    for (int i = 0; i < CHANNELS; i++) if (chBitMask & (1u << i)) s_ch[i].dTimer = timer;
    UNLOCK();
}

void SND_SetChannelVolume(u32 chBitMask, int volume, SNDChannelDataShift shift) {
    LOCK();
    for (int i = 0; i < CHANNELS; i++) if (chBitMask & (1u << i)) { s_ch[i].dVolume = volume; s_ch[i].dShift = shift; }
    UNLOCK();
}

void SND_SetChannelPan(u32 chBitMask, s32 pan) {
    LOCK();
    for (int i = 0; i < CHANNELS; i++) if (chBitMask & (1u << i)) s_ch[i].dPan = pan;
    UNLOCK();
}

void SND_PrepareSeq(int playerNo, const void *seqBase, u32 seqOffset, const struct SNDBankData *bank) {
    if (playerNo < 0 || playerNo >= PLAYERS) return;
    LOCK();
    player_stop(playerNo);
    Player *pl = &s_players[playerNo];
    int prio = pl->priority, vol = pl->volume, ratio = pl->tempoRatio;
    memset(pl, 0, sizeof *pl);
    for (int i = 0; i < 16; i++) pl->tracks[i] = -1;
    pl->priority = prio;
    pl->volume = vol;
    pl->tempoRatio = ratio ? ratio : 256;
    pl->tempo = 120;
    pl->seqVolume = 127;
    pl->bank = bank;
    for (int i = 0; i < 16; i++) pl->vars[i] = -1;
    int ti = alloc_track();
    if (ti >= 0) {
        const u8 *base = seqBase;
        const u8 *cur = base + seqOffset;
        track_init(&s_tracks[ti], playerNo, base, cur);
        pl->tracks[0] = ti;
        /* 0xFE: allocate tracks mask (opened later by 0x93) */
        if (cur[0] == 0xFE) {
            s_tracks[ti].cur = cur + 3;
        }
        /* a prepared player already counts as active (SND_GetPlayerStatus) */
        pl->prepared = TRUE;
        pl->active = TRUE;
    }
    UNLOCK();
}

void SND_StartPreparedSeq(s32 playerNo) {
    if (playerNo < 0 || playerNo >= PLAYERS) return;
    LOCK();
    Player *pl = &s_players[playerNo];
    if (pl->prepared) {
        pl->prepared = FALSE;
        pl->tempoCounter = 240;   /* the first tick runs at once */
    }
    UNLOCK();
}

void SND_StopSeq(s32 playerNo) {
    if (playerNo < 0 || playerNo >= PLAYERS) return;
    LOCK();
    player_stop(playerNo);
    UNLOCK();
}

void SND_PauseSeq(s32 playerNo, BOOL flag) {
    if (playerNo < 0 || playerNo >= PLAYERS) return;
    LOCK();
    s_players[playerNo].paused = flag;
    if (flag) {
        for (int i = 0; i < 16; i++) {
            if (s_players[playerNo].tracks[i] >= 0) track_release_channels(s_players[playerNo].tracks[i], FALSE);
        }
    }
    UNLOCK();
}

void SND_SetPlayerVolume(s32 playerNo, s32 volume) {
    if (playerNo < 0 || playerNo >= PLAYERS) return;
    LOCK(); s_players[playerNo].volume = volume < -7230 ? -7230 : volume > 0 ? 0 : volume; UNLOCK();
}

void SND_SetPlayerTempoRatio(s32 playerNo, s32 ratio) {
    if (playerNo < 0 || playerNo >= PLAYERS) return;
    LOCK(); s_players[playerNo].tempoRatio = ratio; UNLOCK();
}

void SND_SetPlayerChannelPriority(s32 playerNo, s32 prio) {
    if (playerNo < 0 || playerNo >= PLAYERS) return;
    LOCK(); s_players[playerNo].priority = prio; UNLOCK();
}

static void for_tracks(s32 playerNo, u32 mask, void (*fn)(Track *, s32), s32 v) {
    if (playerNo < 0 || playerNo >= PLAYERS) return;
    LOCK();
    for (int i = 0; i < 16; i++) {
        int ti = s_players[playerNo].tracks[i];
        if ((mask & (1u << i)) && ti >= 0) fn(&s_tracks[ti], v);
    }
    UNLOCK();
}
static void set_pitch(Track *t, s32 v) { t->extraPitch = v; }
static void set_pan(Track *t, s32 v) { t->extraPan = v; }
static void set_mask(Track *t, s32 v) { t->chMask = (u32)v; }

void SND_SetTrackPitch(s32 playerNo, u32 trackBitMask, s32 pitch) { for_tracks(playerNo, trackBitMask, set_pitch, pitch); }
void SND_SetTrackPan(s32 playerNo, u32 trackBitMask, s32 pan) { for_tracks(playerNo, trackBitMask, set_pan, pan); }
static void set_vol(Track *t, s32 v) { t->extraVol = v; }
static void set_mute(Track *t, s32 v) { t->mute = v != 0; }
static void set_mod_depth(Track *t, s32 v) { t->modDepth = v; }
static void set_mod_speed(Track *t, s32 v) { t->modSpeed = v; }
static void set_pan_range(Track *t, s32 v) { (void)t; (void)v; }   /* pan range: not modelled */
void SND_SetTrackVolume(int playerNo, u32 trackBitMask, int volume) { for_tracks(playerNo, trackBitMask, set_vol, volume); }
void SND_SetTrackMute(int playerNo, u32 trackBitMask, BOOL flag) { for_tracks(playerNo, trackBitMask, set_mute, flag); }
void SND_SetTrackModDepth(int playerNo, u32 trackBitMask, int depth) { for_tracks(playerNo, trackBitMask, set_mod_depth, depth); }
void SND_SetTrackModSpeed(int playerNo, u32 trackBitMask, int speed) { for_tracks(playerNo, trackBitMask, set_mod_speed, speed); }
void SND_SetTrackPanRange(int playerNo, u32 trackBitMask, int panRange) { for_tracks(playerNo, trackBitMask, set_pan_range, panRange); }

s16 SND_GetPlayerGlobalVariable(int varNo) {
    if (varNo < 0 || varNo >= 16) return 0;
    LOCK();
    s16 v = s_globalVars[varNo];
    UNLOCK();
    return v;
}

void SND_SetPlayerGlobalVariable(int varNo, s16 var) {
    if (varNo < 0 || varNo >= 16) return;
    LOCK();
    s_globalVars[varNo] = var;
    UNLOCK();
}

void SND_SetPlayerLocalVariable(int playerNo, int varNo, s16 var) {
    if (playerNo < 0 || playerNo >= PLAYERS || varNo < 0 || varNo >= 16) return;
    LOCK();
    s_players[playerNo].vars[varNo] = var;
    UNLOCK();
}

/* Ticks the sequencer has run for a player (tempo-driven), read by the
 * game to synchronise animations with music. */
u32 SND_GetPlayerTickCounter(int playerNo) {
    if (playerNo < 0 || playerNo >= PLAYERS) return 0;
    LOCK();
    u32 t = s_players[playerNo].ticks;
    UNLOCK();
    return t;
}

BOOL SND_ReadPlayerInfo(const SNDDriverInfo *driverInfo, int playerNo, SNDPlayerInfo *playerInfo) {
    (void)driverInfo;
    if (playerNo < 0 || playerNo >= PLAYERS) return FALSE;
    memset(playerInfo, 0, sizeof *playerInfo);
    LOCK();
    const Player *pl = &s_players[playerNo];
    playerInfo->activeFlag = pl->active;
    playerInfo->pauseFlag = pl->paused;
    playerInfo->tempo = (u16)pl->tempo;
    playerInfo->volume = (u8)pl->seqVolume;
    for (int t = 0; t < 16; t++) {
        if (pl->tracks[t] >= 0 && s_tracks[pl->tracks[t]].active) playerInfo->trackBitMask |= 1u << t;
    }
    UNLOCK();
    return TRUE;
}

void SND_StopUnlockedChannel(u32 chBitMask, u32 flags) {
    (void)flags;
    LOCK();
    for (int i = 0; i < CHANNELS; i++) {
        if ((chBitMask & (1u << i)) && !s_ch[i].locked) s_ch[i].active = FALSE;
    }
    UNLOCK();
}

/* Sound capture (reverb / effects) and surround: not emulated. */
void SND_SetupCapture(SNDCapture capture, SNDCaptureFormat format, void *buffer_p, u32 length, BOOL loopFlag,
                      SNDCaptureIn in, SNDCaptureOut out) {
    (void)capture; (void)format; (void)buffer_p; (void)length; (void)loopFlag; (void)in; (void)out;
}
void SNDi_SetSurroundDecay(int decay) { (void)decay; }

void SND_SetTrackAllocatableChannel(s32 playerNo, u32 trackBitMask, u32 chBitMask) {
    for_tracks(playerNo, trackBitMask, set_mask, (s32)chBitMask);
}

/* ----------------------------------------------------------- banks / waves */

/* The wave archive links inside SBNK/SWAR files hold 32-bit pointers. */
void SND_AssignWaveArc(struct SNDBankData *bank, int index, struct SNDWaveArc *waveArc) {
    u8 *b = bank;
    LOCK();
    u8 *link = b + 0x18 + (u32)index * 8;
    u32 old = rd32(link);
    if (old != 0) {
        /* unlink from the previous archive's list */
        u8 *arc = (u8 *)(uintptr_t)old;
        u32 *pp = (u32 *)(arc + 0x18);
        while (*pp != 0) {
            if (*pp == (u32)(uintptr_t)link) { *pp = rd32(link + 4); break; }
            pp = (u32 *)(uintptr_t)(*pp + 4);
        }
    }
    *(u32 *)link = (u32)(uintptr_t)waveArc;
    if (waveArc != NULL) {
        u8 *arc = waveArc;
        *(u32 *)(link + 4) = rd32(arc + 0x18);
        *(u32 *)(arc + 0x18) = (u32)(uintptr_t)link;
    } else {
        *(u32 *)(link + 4) = 0;
    }
    UNLOCK();
}

void SND_DestroyBank(struct SNDBankData *bank) {
    for (int i = 0; i < 4; i++) {
        if (rd32((u8 *)bank + 0x18 + i * 8) != 0) SND_AssignWaveArc(bank, i, NULL);
    }
}

void SND_DestroyWaveArc(struct SNDWaveArc *waveArc) {
    u8 *arc = waveArc;
    LOCK();
    u32 link = rd32(arc + 0x18);
    while (link != 0) {
        u8 *l = (u8 *)(uintptr_t)link;
        u32 next = rd32(l + 4);
        *(u32 *)l = 0;
        *(u32 *)(l + 4) = 0;
        link = next;
    }
    *(u32 *)(arc + 0x18) = 0;
    UNLOCK();
}

u32 SND_GetWaveDataCount(const struct SNDWaveArc *waveArc) { return rd32((const u8 *)waveArc + 0x38); }

const SNDWaveData *SND_GetWaveDataAddress(const struct SNDWaveArc *waveArc, int index) { return wave_address(waveArc, (u32)index); }

void SND_SetWaveDataAddress(struct SNDWaveArc *waveArc, int index, const SNDWaveData *waveData) {
    LOCK();
    *(u32 *)((u8 *)waveArc + 0x3C + (u32)index * 4) = (u32)(uintptr_t)waveData;
    UNLOCK();
}

SNDInstPos SND_GetFirstInstDataPos(const struct SNDBankData *bank) {
    (void)bank;
    SNDInstPos p = { 0, 0 };
    return p;
}

BOOL SND_GetNextInstData(const struct SNDBankData *bank, SNDInstData *instData, SNDInstPos *pos) {
    const u8 *b = bank;
    u32 count = rd32(b + 0x38);
    for (; pos->prgNo < count; pos->prgNo++, pos->index = 0) {
        u32 off = rd32(b + 0x3C + pos->prgNo * 4);
        u8 type = (u8)off;
        const u8 *p = b + (off >> 8);
        if (type >= 1 && type <= 5) {
            if (pos->index == 0) {
                instData->type = type;
                instData->padding_ = 0;
                memcpy(&instData->param, p, 10);
                pos->index = 1;
                return TRUE;
            }
            continue;
        }
        if (type == 16) {
            u32 n = (u32)(p[1] - p[0] + 1);
            if (pos->index < n) {
                const u8 *e = p + 2 + pos->index * 12;
                instData->type = e[0];
                instData->padding_ = 0;
                memcpy(&instData->param, e + 2, 10);
                pos->index++;
                return TRUE;
            }
            continue;
        }
        if (type == 17) {
            u32 n = 0;
            while (n < 8 && p[n] != 0) n++;
            if (pos->index < n) {
                const u8 *e = p + 8 + pos->index * 12;
                instData->type = e[0];
                instData->padding_ = 0;
                memcpy(&instData->param, e + 2, 10);
                pos->index++;
                return TRUE;
            }
            continue;
        }
    }
    return FALSE;
}

/* Data caches: nothing to do on PC. */
void SND_InvalidateSeqData(const void *start, const void *end) { (void)start; (void)end; }
void SND_InvalidateBankData(const void *start, const void *end) { (void)start; (void)end; }
void SND_InvalidateWaveData(const void *start, const void *end) { (void)start; (void)end; }

/* ----------------------------------------------------------- game thread */

void pc_snd_vblank(void) {
    if (!pc_audio_active()) {
        /* no output device: run the driver anyway so that sequences end */
        mix(NULL, (int)(OUT_RATE / 59.8261f), NULL);
    }
    /* alarm callbacks run on the game thread, like PXI callbacks on the DS */
    for (int i = 0; i < ALARMS; i++) {
        void (*cb)(void *) = NULL;
        void *arg = NULL;
        int n = 0;
        LOCK();
        if (s_alarms[i].pending > 0) {
            n = s_alarms[i].pending;
            s_alarms[i].pending = 0;
            cb = s_alarms[i].callback;
            arg = s_alarms[i].arg;
        }
        UNLOCK();
        while (cb != NULL && n-- > 0) cb(arg);
    }
}

void pc_snd_reset(void) {
    SND_Init();
}
