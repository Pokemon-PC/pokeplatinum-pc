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
 *   PPL_TEST_GIVE_ITEMS=ID[:QTY],...  bag items (nitro_pc.c calls pc_test_give_item)
 *   PPL_TEST_BADGES=N             the first N badges
 *   PPL_TEST_MOVES=SLOT:MOVE,...  moves of the first party Pokemon; also sets
 *                                 the battle style to "Set" and fast text
 *   PPL_TEST_SPECIES=N            the first party Pokemon becomes species N
 *   PPL_TEST_NO_ENCOUNTERS=1      no wild encounters while walking
 *   PPL_TEST_NO_TRAINERS=1        trainers do not spot the player
 *   PPL_TEST_FLAGS=F[,!F...]      set (or with "!" clear) event flags once the field is idle
 *   PPL_TEST_VARS=V:N[,...]       set event variables once the field is idle
 */
#include <stdlib.h>
#include <string.h>

#include "constants/heap.h"

#include "bag.h"
#include "encounter.h"
#include "field/field_system.h"
#include "field_map_change.h"
#include "field_task.h"
#include "heap.h"
#include "item_use_pokemon.h"
#include "location.h"
#include "party.h"
#include "player_avatar.h"
#include "pokemon.h"
#include "game_options.h"
#include "save_player.h"
#include "trainer_info.h"
#include "vars_flags.h"

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
    Party_HealAllMembers(party);   // HP, status and PP, like a Pokemon Center
}

void pc_test_set_move(int slot, u16 move);

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
        // The player as original trainer: a high-level Pokemon from another
        // trainer disobeys without badges.
        Pokemon_SetCatchData(mon, SaveData_GetTrainerInfo(fs->saveData), 4 /* Poke Ball */, 0, 0, HEAP_ID_FIELD2);
        BOOL ok = Party_AddPokemon(party, mon);
        Heap_Free(mon);
        pc_log("test: give species %d level %d: %s", species, level, ok ? "added" : "party full");
        spec = *next == ',' ? next + 1 : NULL;
    }
    // PPL_TEST_MOVES again, now that the party has its Pokemon.
    const char *m = getenv("PPL_TEST_MOVES");
    while (m != NULL && *m != '\0') {
        char *n;
        int slot = (int)strtol(m, &n, 10);
        if (*n != ':') {
            break;
        }
        u16 move = (u16)strtoul(n + 1, &n, 10);
        pc_test_set_move(slot, move);
        m = *n == ',' ? n + 1 : NULL;
    }
}

// ------------------------------------------------------- items, badges, moves
void pc_test_give_item(u16 item, u16 qty)
{
    FieldSystem *fs = pc_test_field_system();
    if (fs == NULL) {
        return;
    }
    BOOL ok = Bag_TryAddItem(SaveData_GetBag(fs->saveData), item, qty, HEAP_ID_FIELD2);
    pc_log("test: give item %u x%u: %s", (unsigned)item, (unsigned)qty, ok ? "added" : "bag full");
}

void pc_test_set_badges(int n)
{
    FieldSystem *fs = pc_test_field_system();
    if (fs == NULL) {
        return;
    }
    TrainerInfo *info = SaveData_GetTrainerInfo(fs->saveData);
    for (int i = 0; i < n && i < 8; i++) {
        TrainerInfo_SetBadge(info, i);
    }
    pc_log("test: %d badges", n);
}

void pc_test_set_move(int slot, u16 move)
{
    FieldSystem *fs = pc_test_field_system();
    if (fs == NULL || slot < 0 || slot > 3) {
        return;
    }
    Party *party = SaveData_GetParty(fs->saveData);
    if (Party_GetCurrentCount(party) == 0) {
        return;
    }
    Pokemon_ResetMoveSlot(Party_GetPokemonBySlotIndex(party, 0), move, (u8)slot);
    Options *options = SaveData_GetOptions(fs->saveData);
    Options_SetBattleStyle(options, OPTIONS_BATTLE_STYLE_SET);
    Options_SetTextSpeed(options, OPTIONS_TEXT_SPEED_FAST);
    Pokemon *mon0 = Party_GetPokemonBySlotIndex(party, 0);
    pc_log("test: party slot 0 move %d = %u (battle style Set, fast text); moves %u %u %u %u, status %u", slot, (unsigned)move,
        (unsigned)Pokemon_GetValue(mon0, MON_DATA_MOVE1, NULL), (unsigned)Pokemon_GetValue(mon0, MON_DATA_MOVE2, NULL),
        (unsigned)Pokemon_GetValue(mon0, MON_DATA_MOVE3, NULL), (unsigned)Pokemon_GetValue(mon0, MON_DATA_MOVE4, NULL),
        (unsigned)Pokemon_GetValue(mon0, MON_DATA_STATUS, NULL));
}

static void set_species(int species)
{
    FieldSystem *fs = pc_test_field_system();
    Party *party = SaveData_GetParty(fs->saveData);
    if (Party_GetCurrentCount(party) == 0) {
        return;
    }
    Pokemon *mon = Party_GetPokemonBySlotIndex(party, 0);
    u32 sp = (u32)species;
    Pokemon_SetValue(mon, MON_DATA_SPECIES, &sp);
    Pokemon_CalcLevelAndStats(mon);
    u32 hp = Pokemon_GetValue(mon, MON_DATA_MAX_HP, NULL);
    Pokemon_SetValue(mon, MON_DATA_HP, &hp);
    pc_log("test: party slot 0 is now species %d", species);
}

static void set_flags_vars(const char *flags, const char *vars)
{
    FieldSystem *fs = pc_test_field_system();
    VarsFlags *vf = SaveData_GetVarsFlags(fs->saveData);
    while (flags != NULL && *flags != '\0') {
        char *n;
        BOOL clear = *flags == '!';
        u16 f = (u16)strtoul(flags + (clear ? 1 : 0), &n, 0);
        if (clear) {
            VarsFlags_ClearFlag(vf, f);
        } else {
            VarsFlags_SetFlag(vf, f);
        }
        pc_log("test: flag %u %s", (unsigned)f, clear ? "cleared" : "set");
        flags = *n == ',' ? n + 1 : NULL;
    }
    while (vars != NULL && *vars != '\0') {
        char *n;
        u16 v = (u16)strtoul(vars, &n, 0);
        u16 val = *n == ':' ? (u16)strtoul(n + 1, &n, 0) : 0;
        u16 *addr = VarsFlags_GetVarAddress(vf, v);
        if (addr != NULL) {
            *addr = val;
        }
        pc_log("test: var %u = %u", (unsigned)v, (unsigned)val);
        vars = *n == ',' ? n + 1 : NULL;
    }
}

static int sPcNoEncounters = -1, sPcNoTrainers = -1;

int pc_test_no_encounters(void)
{
    if (sPcNoEncounters < 0) {
        const char *e = getenv("PPL_TEST_NO_ENCOUNTERS");
        sPcNoEncounters = e != NULL && atoi(e) != 0;
    }
    return sPcNoEncounters;
}

int pc_test_no_trainers(void)
{
    if (sPcNoTrainers < 0) {
        const char *e = getenv("PPL_TEST_NO_TRAINERS");
        sPcNoTrainers = e != NULL && atoi(e) != 0;
    }
    return sPcNoTrainers;
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
        FieldTask_StartMapChangeFull(task, (enum MapHeaderID)sPcWarp[0], -1, sPcWarp[1], sPcWarp[2], 0);   // facing north
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
    static const char *giveMons, *flags, *vars;
    static int species;
    if (!init) {
        init = 1;
        const char *p = getenv("PPL_TEST_POS");
        posEvery = p != NULL ? (u32)strtoul(p, NULL, 10) : 0;
        giveMons = getenv("PPL_TEST_GIVE_MON");
        flags = getenv("PPL_TEST_FLAGS");
        vars = getenv("PPL_TEST_VARS");
        const char *sp = getenv("PPL_TEST_SPECIES");
        species = sp != NULL ? atoi(sp) : 0;
        const char *w = getenv("PPL_TEST_WILD");
        if (w != NULL) {
            char *n;
            sPcWildSpecies = (u16)strtoul(w, &n, 10);
            sPcWildLevel = *n == ':' ? (u8)strtoul(n + 1, &n, 10) : 5;
            wildFrame = *n == ':' ? (u32)strtoul(n + 1, &n, 10) : 1;
        }
    }
    // Progress journal for scripted runs: badges, money, party, map changes.
    static int lastBadges = -1, lastMap = -1;
    static u32 lastMoney;
    FieldSystem *fs = pc_test_field_system();
    if (fs != NULL && fs->saveData != NULL && frame % 30 == 0) {
        TrainerInfo *info = SaveData_GetTrainerInfo(fs->saveData);
        int badges = TrainerInfo_BadgeCount(info);
        u32 money = TrainerInfo_Money(info);
        if (badges != lastBadges || money != lastMoney) {
            pc_log("progress: frame %u badges %d money %u", (unsigned)frame, badges, (unsigned)money);
            lastBadges = badges;
            lastMoney = money;
        }
        if (pc_test_field_ready() && fs->location->mapHeaderID != lastMap) {
            lastMap = fs->location->mapHeaderID;
            pc_log("progress: frame %u map %d", (unsigned)frame, lastMap);
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
    if (species > 0 && giveMons == NULL && pc_test_field_idle()) {
        set_species(species);
        species = 0;
    }
    if ((flags != NULL || vars != NULL) && pc_test_field_idle()) {
        set_flags_vars(flags, vars);
        flags = vars = NULL;
    }
    if (wildFrame != 0 && frame >= wildFrame && pc_test_field_idle()) {
        wildFrame = 0;
        pc_log("test: wild battle species %u level %u", (unsigned)sPcWildSpecies, (unsigned)sPcWildLevel);
        FieldSystem_CreateTask(pc_test_field_system(), PcTestTask_Wild, NULL);
    }
}
