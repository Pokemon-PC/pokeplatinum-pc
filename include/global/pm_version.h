#ifndef POKEPLATINUM_PM_VERSION_H
#define POKEPLATINUM_PM_VERSION_H

#include "constants/versions.h"

extern const u8 gGameVersion;
#ifdef PLATFORM_PC
// PC port: the language follows the player's ROM / --lang (src/platform_pc).
#define gGameLanguage gPcGameLanguage
#else
extern const u8 gGameLanguage;
#endif

#endif // POKEPLATINUM_PM_VERSION_H
