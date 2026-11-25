/*
 * snd_pc.h - software DS sound driver (see snd_pc.c).
 */
#ifndef PPL_SND_PC_H
#define PPL_SND_PC_H

#include <nitro/snd.h>
#include <nnsys/snd.h>

/* Game-thread work once per V-Blank: alarm callbacks, and running the driver
 * when there is no audio device. */
void pc_snd_vblank(void);
/* Power-on state (soft reset). */
void pc_snd_reset(void);

#endif
