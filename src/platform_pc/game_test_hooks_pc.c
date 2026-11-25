/*
 * game_test_hooks_pc.c - test hooks that need the game's own structures
 * (Platinum field system, party, encounters), for scripted test runs of the
 * PC port. Compiled with the game sources; the Nitro layer calls them through
 * weak references (src/platform_pc/nitro/nitro_pc.c), so they cost nothing
 * when no PPL_TEST_* variable is set.
 *
 *   PPL_TEST_POS=N                log map header and player tile every N frames
 *   PPL_TEST_GIVE_MON=SPECIES:LEVEL[,...]   add Pokemon to the party once the
 *                                 field is idle (test aid: battles from any save)
 *   PPL_TEST_WILD=SPECIES:LEVEL:FRAME       start a wild battle at that frame
 *   PPL_TEST_TRAINER=ID[:FRAME]   start a trainer battle (nitro_pc.c parses it)
 *   PPL_TEST_WARP=MAP:X:Z[:N]     warp (nitro_pc.c parses it)
 *   PPL_TEST_PARTY_LEVEL=N[+]     (nitro_pc.c)
 */
#include <stdlib.h>
#include <string.h>

#include "constants/heap.h"

#include "encounter.h"
#include "field/field_system.h"
#include "field_map_change.h"
#include "field_task.h"
#include "heap.h"
#include "location.h"
#include "party.h"
#include "player_avatar.h"
#include "pokemon.h"

void pc_log(const char *fmt, ...);
FieldSystem *pc_test_field_system(void);   // src/field_system.c (PLATFORM_PC)

// ------------------------------------------------------------- field state
BOOL pc_test_field_ready(void)
{
    FieldSystem *fs = pc_test_field_system();
    return fs != NULL && FieldSystem_IsRunningFieldMapInner(fs) && fs->playerAvatar != NULL && fs->location != NULL;
}

// The player can move: field running and no field task (script, warp...).
BOOL pc_test_field_idle(void)
{
    FieldSystem *fs = pc_test_field_system();
    return pc_test_field_ready() && fs->task == NULL;
}

BOOL pc_test_player_pos(int *mapId, int *x, int *z)
{
    if (!pc_test_field_ready()) {
        return FALSE;
    }
    FieldSystem *fs = pc_test_field_system();
    *mapId = fs->location->mapHeaderID;
    *x = PlayerAvatar_GetXPos(fs->playerAvatar);
    *z = PlayerAvatar_GetZPos(fs->playerAvatar);
    return TRUE;
}

// ------------------------------------------------------------------- party
void pc_test_set_party_level(int level, BOOL almostNext)
{
    FieldSystem *fs = pc_test_field_system();
    if (fs == NULL || level < 1 || level > 100) {
        return;
    }
    Party *party = SaveData_GetParty(fs->saveData);
    for (int i = 0; i < Party_GetCurrentCount(party); i++) {
        Pokemon *mon = Party_GetPokemonBySlotIndex(party, i);
        if (Pokemon_GetValue(mon, MON_DATA_IS_EGG, NULL)) {
            continue;
        }
        u32 species = Pokemon_GetValue(mon, MON_DATA_SPECIES, NULL);
        u32 exp = almostNext && level < 100 ? Pokemon_GetSpeciesBaseExpAt(species, level + 1) - 1
                                            : Pokemon_GetSpeciesBaseExpAt(species, level);
        Pokemon_SetValue(mon, MON_DATA_EXPERIENCE, &exp);
        Pokemon_CalcLevelAndStats(mon);
        u32 hp = Pokemon_GetValue(mon, MON_DATA_MAX_HP, NULL);
        Pokemon_SetValue(mon, MON_DATA_HP, &hp);
        pc_log("test: party slot %d (species %u) set to level %d", i, (unsigned)species, level);
    }
}

static void give_mons(const char *spec)
{
    FieldSystem *fs = pc_test_field_system();
    Party *party = SaveData_GetParty(fs->saveData);
    while (spec != NULL && *spec != '\0') {
        char *next;
        int species = (int)strtol(spec, &next, 10);
        int level = 5;
        if (*next == ':') {
            level = (int)strtol(next + 1, &next, 10);
        }
        Pokemon *mon = Pokemon_New(HEAP_ID_FIELD2);
        Pokemon_InitWith(mon, species, level, 32, FALSE, 0, 0, 0);
        BOOL ok = Party_AddPokemon(party, mon);
        Heap_Free(mon);
        pc_log("test: give species %d level %d: %s", species, level, ok ? "added" : "party full");
        spec = *next == ',' ? next + 1 : NULL;
    }
}

// ----------------------------------------------------------------- battles
static int sPcBattleResult;
static u16 sPcWildSpecies;
static u8 sPcWildLevel;

static BOOL PcTestTask_Wild(FieldTask *task)
{
    int *state = FieldTask_GetState(task);
    if (*state == 0) {
        *state = 1;
        Encounter_NewVsSpeciesAtLevel(task, sPcWildSpecies, sPcWildLevel, &sPcBattleResult, FALSE);
        return FALSE;
    }
    pc_log("test: wild battle over (result %d)", sPcBattleResult);
    return TRUE;
}

static u32 sPcTrainerId;

static BOOL PcTestTask_Trainer(FieldTask *task)
{
    int *state = FieldTask_GetState(task);
    if (*state == 0) {
        *state = 1;
        Encounter_NewVsTrainer(task, (int)sPcTrainerId, 0, 0, HEAP_ID_FIELD2, &sPcBattleResult);
        return FALSE;
    }
    pc_log("test: trainer %u battle over (result %d)", (unsigned)sPcTrainerId, sPcBattleResult);
    return TRUE;
}

void pc_test_trainer_battle(u32 trainerId)
{
    FieldSystem *fs = pc_test_field_system();
    if (fs == NULL) {
        return;
    }
    sPcTrainerId = trainerId;
    pc_log("test: trainer battle %u", (unsigned)trainerId);
    FieldSystem_CreateTask(fs, PcTestTask_Trainer, NULL);
}

// ------------------------------------------------------------------- warps
static int sPcWarp[3];

static BOOL PcTestTask_Warp(FieldTask *task)
{
    int *state = FieldTask_GetState(task);
    if (*state == 0) {
        *state = 1;
        FieldTask_ChangeMapToLocation(task, (enum MapHeaderID)sPcWarp[0], -1, sPcWarp[1], sPcWarp[2], 1);
        return FALSE;
    }
    return TRUE;
}

void pc_test_warp(u32 mapId, u32 x, u32 z)
{
    FieldSystem *fs = pc_test_field_system();
    if (fs == NULL) {
        return;
    }
    sPcWarp[0] = (int)mapId;
    sPcWarp[1] = (int)x;
    sPcWarp[2] = (int)z;
    pc_log("test: warp to map %u (%u, %u)", (unsigned)mapId, (unsigned)x, (unsigned)z);
    FieldSystem_CreateTask(fs, PcTestTask_Warp, NULL);
}

// ------------------------------------------------------- per-frame driver
// Called every V-Blank by the Nitro layer (weak reference in nitro_pc.c).
void pc_test_frame(u32 frame)
{
    static int init;
    static u32 posEvery, wildFrame;
    static const char *giveMons;
    if (!init) {
        init = 1;
        const char *p = getenv("PPL_TEST_POS");
        posEvery = p != NULL ? (u32)strtoul(p, NULL, 10) : 0;
        giveMons = getenv("PPL_TEST_GIVE_MON");
        const char *w = getenv("PPL_TEST_WILD");
        if (w != NULL) {
            char *n;
            sPcWildSpecies = (u16)strtoul(w, &n, 10);
            sPcWildLevel = *n == ':' ? (u8)strtoul(n + 1, &n, 10) : 5;
            wildFrame = *n == ':' ? (u32)strtoul(n + 1, &n, 10) : 1;
        }
    }
    int map, x, z;
    if (posEvery != 0 && frame % posEvery == 0 && pc_test_player_pos(&map, &x, &z)) {
        pc_log("pos: frame %u map %d x=%d z=%d", (unsigned)frame, map, x, z);
    }
    if (giveMons != NULL && pc_test_field_idle()) {
        give_mons(giveMons);
        giveMons = NULL;
    }
    if (wildFrame != 0 && frame >= wildFrame && pc_test_field_idle()) {
        wildFrame = 0;
        pc_log("test: wild battle species %u level %u", (unsigned)sPcWildSpecies, (unsigned)sPcWildLevel);
        FieldSystem_CreateTask(pc_test_field_system(), PcTestTask_Wild, NULL);
    }
}
