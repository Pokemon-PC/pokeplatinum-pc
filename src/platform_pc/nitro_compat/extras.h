/*
 * extras.h - stand-in for the Metrowerks MSL header of the same name, which
 * NitroDWC's GameSpy layer (gs/nonport.h) includes on the DS. It declares a
 * few non-standard string helpers; on PC the host C library provides what
 * the game uses, and the online code is not run (no servers).
 */
#pragma once

#include <stdlib.h>
#include <string.h>
