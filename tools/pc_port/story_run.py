#!/usr/bin/env python3
"""Scripted run through the main story of the PC port: the 8 gyms, the
Elite Four, the Champion and the Hall of Fame, one game process per step,
each step starting from the previous step's save.

Each step warps the player in front of the trainer (coordinates read from
res/field/maps/<map>/events.json), faces them, talks (A), then presses A
whenever the player is not free (PPL_TEST_MASH): the leader's real script
runs (dialogue, battle, badge, TM), then the game is saved. A step passes
when its check holds (a new badge, prize money, a map reached) and the
game did not crash; screenshots of each step are kept.

    python tools/pc_port/story_run.py --save START.bin --out DIR [--exe EXE]
           [--lang fr|en] [--only NAME,...] [--from NAME]
    python tools/pc_port/story_run.py --places --save START.bin --out DIR
           (warps through ~45 key locations, a screenshot of each)

START.bin: a save in the field (e.g. the player's room). The first step
gives a test team (PPL_TEST_GIVE_MON) unless --no-team.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]

# name, map header, object graphics id (the trainer to talk to), check, frames
STEPS = [
    ("gym1_roark", "MAP_HEADER_OREBURGH_CITY_GYM", "OBJ_EVENT_GFX_ROARK", "badge", 14000),
    ("gym2_gardenia", "MAP_HEADER_ETERNA_CITY_GYM", "OBJ_EVENT_GFX_GARDENIA", "badge", 14000),
    ("gym3_fantina", "MAP_HEADER_HEARTHOME_CITY_GYM_LEADER_ROOM", "OBJ_EVENT_GFX_FANTINA", "badge", 14000),
    ("gym4_maylene", "MAP_HEADER_VEILSTONE_CITY_GYM", "OBJ_EVENT_GFX_MAYLENE", "badge", 14000),
    ("gym5_wake", "MAP_HEADER_PASTORIA_CITY_GYM", "OBJ_EVENT_GFX_CRASHER_WAKE", "badge", 14000),
    ("gym6_byron", "MAP_HEADER_CANALAVE_CITY_GYM", "OBJ_EVENT_GFX_BYRON", "trainer:TRAINER_LEADER_BYRON", 14000),
    ("gym7_candice", "MAP_HEADER_SNOWPOINT_CITY_GYM", "OBJ_EVENT_GFX_CANDICE", "badge", 14000),
    ("gym8_volkner", "MAP_HEADER_SUNYSHORE_CITY_GYM_ROOM_3", "OBJ_EVENT_GFX_VOLKNER", "badge", 14000),
    ("e4_aaron", "MAP_HEADER_POKEMON_LEAGUE_AARON_ROOM", "OBJ_EVENT_GFX_AARON", "trainer:TRAINER_ELITE_FOUR_AARON", 24000),
    ("e4_bertha", "MAP_HEADER_POKEMON_LEAGUE_BERTHA_ROOM", "OBJ_EVENT_GFX_BERTHA", "trainer:TRAINER_ELITE_FOUR_BERTHA", 24000),
    ("e4_flint", "MAP_HEADER_POKEMON_LEAGUE_FLINT_ROOM", "OBJ_EVENT_GFX_FLINT", "trainer:TRAINER_ELITE_FOUR_FLINT", 24000),
    ("e4_lucian", "MAP_HEADER_POKEMON_LEAGUE_LUCIAN_ROOM", "OBJ_EVENT_GFX_LUCIAN", "trainer:TRAINER_ELITE_FOUR_LUCIAN", 24000),
    ("champion_cynthia", "MAP_HEADER_POKEMON_LEAGUE_CHAMPION_ROOM", "OBJ_EVENT_GFX_CYNTHIA", "hall_of_fame", 40000),
]

# --places: key locations visited by warps in one process (screenshot each).
PLACES = [
    "TWINLEAF_TOWN", "SANDGEM_TOWN", "JUBILIFE_CITY", "OREBURGH_CITY", "FLOAROMA_TOWN",
    "ETERNA_FOREST", "ETERNA_CITY", "HEARTHOME_CITY", "AMITY_SQUARE", "SOLACEON_TOWN",
    "VEILSTONE_CITY", "PASTORIA_CITY", "PASTORIA_CITY_DP_GREAT_MARSH", "GREAT_MARSH_1",
    "CELESTIC_TOWN", "CANALAVE_CITY", "IRON_ISLAND", "SNOWPOINT_CITY", "SNOWPOINT_TEMPLE_1F",
    "SUNYSHORE_CITY", "POKEMON_LEAGUE", "VICTORY_ROAD_1F", "MT_CORONET_1F_SOUTH",
    "SPEAR_PILLAR", "GALACTIC_HQ_1F", "LAKE_VERITY", "LAKE_VALOR", "LAKE_ACUITY",
    "OLD_CHATEAU", "FUEGO_IRONWORKS_OUTSIDE", "VALLEY_WINDWORKS_OUTSIDE", "ROUTE_201",
    "ROUTE_210_SOUTH", "ROUTE_216", "ROUTE_217", "BATTLE_TOWER",
    "STARK_MOUNTAIN_OUTSIDE", "TURNBACK_CAVE_ENTRANCE", "DISTORTION_WORLD_1F",
    "JUBILIFE_CITY_POKECENTER_1F", "JUBILIFE_CITY_MART", "VEILSTONE_STORE_1F",
    "CONTEST_HALL_LOBBY", "POKEMON_DAY_CARE", "RESORT_AREA", "SURVIVAL_AREA", "FIGHT_AREA",
]

TITLE_KEYS = "1000:START,1300:START,1600:START,1900:A,2200:A"   # title -> CONTINUE
WARP_AT = 2300


def map_ids():
    names = [l.strip() for l in open(ROOT / "generated" / "map_headers.txt", encoding="utf-8") if l.strip()]
    return {n: i for i, n in enumerate(names)}


def trainer_tile(map_const, gfx):
    """(x, z, hidden flag name or '0') of the trainer's object."""
    folder = ROOT / "res" / "field" / "maps" / map_const.replace("MAP_HEADER_", "").lower()
    ev = json.load(open(folder / "events.json", encoding="utf-8"))
    for o in ev["object_events"]:
        if o["graphics_id"] == gfx:
            return o["x"], o["z"], str(o.get("hidden_flag", "0"))
    raise SystemExit(f"{gfx} not found in {folder}/events.json")


def trainer_ids():
    names = [l.strip() for l in open(ROOT / "generated" / "trainers.txt", encoding="utf-8") if l.strip()]
    return {n: i for i, n in enumerate(names)}


def flag_values(exe):
    """FLAG_* -> number, from the build's generated vars_flags.h."""
    h = pathlib.Path(exe).resolve().parent / "generated_root" / "generated" / "vars_flags.h"
    vals = {}
    if h.exists():
        for m in re.finditer(r"#define (FLAG_\w+)\s+(\d+)", h.read_text(encoding="utf-8", errors="replace")):
            vals[m.group(1)] = int(m.group(2))
    return vals


def arrival_tile(map_const):
    """A tile to arrive on: the map's first warp (door), else its first object."""
    folder = ROOT / "res" / "field" / "maps" / map_const.replace("MAP_HEADER_", "").lower()
    ev = json.load(open(folder / "events.json", encoding="utf-8"))
    if ev.get("warp_events"):
        w = ev["warp_events"][0]
        return w["x"], w["z"]
    if ev.get("object_events"):
        o = ev["object_events"][0]
        return o["x"], o["z"] + 1
    return 8, 8


def run_places(a, ids, out, save):
    """One process per batch of 16 places (the warp list holds 64)."""
    results = []
    names = [p for p in PLACES if "MAP_HEADER_" + p in ids]
    missing = [p for p in PLACES if "MAP_HEADER_" + p not in ids]
    if missing:
        print("unknown maps skipped:", missing)
    every = 700
    for b in range(0, len(names), 16):
        batch = names[b:b + 16]
        warps, dumps = [], []
        for i, p in enumerate(batch):
            x, z = arrival_tile("MAP_HEADER_" + p)
            at = WARP_AT + i * every
            warps.append(f"{ids['MAP_HEADER_' + p]}:{x}:{z}:{at}")
            dumps.append(at + every - 50)
        frames = WARP_AT + len(batch) * every + 1500   # margin: late warps (scenes on arrival)
        env = {"PPL_TEST_KEYS": TITLE_KEYS, "PPL_TEST_WARP": ",".join(warps), "PPL_TEST_MASH": f"20:{WARP_AT}"}
        if a.lang:
            env["_args"] = ["--lang", a.lang]
        wd = out / f"places_{b // 16}"
        rc, secs, log = run_step(pathlib.Path(a.exe), wd, save, env, frames, dumps)
        _, _, _, _, maps = progress(log)
        crashed = "[ppl:crash]" in log
        for i, p in enumerate(batch):
            ok = ids["MAP_HEADER_" + p] in maps and not crashed
            shot = wd / ("frame_%06d.png" % dumps[i])
            if shot.exists():
                shutil.copyfile(shot, out / f"place_{p.lower()}.png")
            results.append((p, ok, f"map {ids['MAP_HEADER_' + p]}{' reached' if ok else ' NOT reached'}"))
            print(f"{'PASS' if ok else 'FAIL'} {p}", flush=True)
        if crashed:
            print(f"crash in batch {b // 16} (see {wd / 'log.txt'})")
    return results


def run_step(exe, workdir, save_in, env_extra, frames, dumps):
    workdir.mkdir(parents=True, exist_ok=True)
    for f in workdir.glob("frame_*.png"):
        f.unlink()
    save = workdir / "save.bin"
    shutil.copyfile(save_in, save)
    env = dict(os.environ)
    env.update({"PPL_TURBO": "1", "PPL_MUTE": "1", "PPL_EXIT_AFTER": str(frames),
                "PPL_TEST_NO_ENCOUNTERS": "1", "PPL_TEST_NO_TRAINERS": "1",
                "PPL_DUMP_FRAMES": ",".join(str(d) for d in dumps)})
    env.update(env_extra)
    t0 = time.time()
    with open(workdir / "log.txt", "w", encoding="utf-8", errors="replace") as log:
        r = subprocess.run([str(exe), "--save", str(save)] + env_extra.get("_args", []), cwd=workdir,
                           env={k: v for k, v in env.items() if not k.startswith("_")},
                           stdout=log, stderr=subprocess.STDOUT)
    return r.returncode, time.time() - t0, (workdir / "log.txt").read_text(encoding="utf-8", errors="replace")


def progress(log):
    badges = money = None
    first_badges = first_money = None
    maps = []
    for m in re.finditer(r"progress: frame (\d+) badges (\d+) money (\d+)", log):
        b, mo = int(m.group(2)), int(m.group(3))
        if first_badges is None:
            first_badges, first_money = b, mo
        badges, money = b, mo
    for m in re.finditer(r"progress: frame \d+ map (\d+)", log):
        maps.append(int(m.group(1)))
    return first_badges, badges, first_money, money, maps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--save", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--exe", default=str(ROOT / "build-pc" / ("pokeplatinum.exe" if os.name == "nt" else "pokeplatinum")))
    ap.add_argument("--lang")
    ap.add_argument("--only")
    ap.add_argument("--from", dest="start")
    ap.add_argument("--no-team", action="store_true")
    ap.add_argument("--places", action="store_true", help="visit the key locations (PLACES) instead of the story")
    ap.add_argument("--team", default="445:100,445:100,445:100", help="PPL_TEST_GIVE_MON for the first step")
    ap.add_argument("--move", default="0:337", help="PPL_TEST_MOVES (default Dragon Claw)")
    a = ap.parse_args()

    ids = map_ids()
    out = pathlib.Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    save = pathlib.Path(a.save).resolve()
    if a.places:
        results = run_places(a, ids, out, save)
        with open(out / "report.txt", "w", encoding="utf-8") as f:
            for name, ok, detail in results:
                f.write(f"{'PASS' if ok else 'FAIL'} {name}: {detail}\n")
        print(f"{sum(1 for r in results if r[1])}/{len(results)} places reached")
        return 0 if all(r[1] for r in results) else 1
    steps = STEPS
    if a.start:
        steps = steps[[s[0] for s in steps].index(a.start):]
    if a.only:
        steps = [s for s in steps if s[0] in a.only.split(",")]
    results = []
    first = True
    hof_map = ids["MAP_HEADER_POKEMON_LEAGUE_HALL_OF_FAME"]
    flags = flag_values(a.exe)
    for name, map_const, gfx, check, frames in steps:
        x, z, hidden = trainer_tile(map_const, gfx)
        mid = ids[map_const]
        talk = WARP_AT + 400
        env = {
            "PPL_TEST_KEYS": TITLE_KEYS,
            "PPL_TEST_WARP": f"{mid}:{x}:{z + 1}:{WARP_AT}",
            "PPL_TEST_TALK": "1",          # once the warp is done: face north, A
            "PPL_TEST_MASH": f"10:{WARP_AT}",  # A whenever the player is not free
            "PPL_TEST_SAVE_AT": str(frames - 900),
            "PPL_TEST_MOVES": a.move,
            "PPL_TEST_PARTY_LEVEL": "100",
        }
        # The trainer may be hidden at this point of the story (e.g. Gardenia
        # is in Eterna Forest): clear its "hide" flag.
        if check.startswith("trainer:"):
            # The trainer cannot be reached on foot here: battle started
            # directly (PPL_TEST_TRAINER), no script, no badge.
            env.pop("PPL_TEST_TALK")
            env["PPL_TEST_TRAINER"] = f"{trainer_ids()[check[8:]]}:{WARP_AT}"
            env["PPL_TEST_WARP"] = f"{mid}:{x}:{z + 1}:0"
            if name.startswith("gym"):
                env["PPL_TEST_BADGES"] = str(int(name[3]))   # the badge the script would give
        if hidden != "0" and hidden in flags:
            env["PPL_TEST_FLAGS"] = f"!{flags[hidden]}"
        if first and not a.no_team:
            env["PPL_TEST_GIVE_MON"] = a.team
        if a.lang:
            env["_args"] = ["--lang", a.lang]
        dumps = [talk + 200, talk + 1500, frames // 2, frames - 1000, frames - 10]
        wd = out / name
        rc, secs, log = run_step(pathlib.Path(a.exe), wd, save, env, frames, dumps)
        b0, b1, m0, m1, maps = progress(log)
        crashed = "[ppl:crash]" in log
        asserts = len(re.findall(r"\[ppl:assert\] game check failed", log))
        saved = "test: FieldSystem_Save -> 1" in log or "test: SaveData_Save -> 1" in log
        if check == "badge":
            ok = b0 is not None and b1 is not None and b1 > b0
        elif check == "money":
            ok = m0 is not None and m1 is not None and m1 > m0
        elif check.startswith("trainer:"):
            m = re.search(r"test: trainer \d+ battle over \(result (\d+)\)", log)
            ok = m is not None and int(m.group(1)) & 1 == 1 and not (int(m.group(1)) & 2)
        else:
            ok = hof_map in maps
        ok = ok and not crashed and rc == 0
        detail = f"badges {b0}->{b1}, money {m0}->{m1}, maps {maps[-3:]}, asserts {asserts}, saved {saved}, {secs:.0f} s"
        print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}", flush=True)
        results.append((name, ok, detail))
        if saved:
            save = wd / "save.bin"
        first = False
        if not ok and not saved:
            print("stopping: no save to continue from")
            break
    with open(out / "report.txt", "w", encoding="utf-8") as f:
        for name, ok, detail in results:
            f.write(f"{'PASS' if ok else 'FAIL'} {name}: {detail}\n")
    print(f"{sum(1 for r in results if r[1])}/{len(results)} steps passed; report: {out / 'report.txt'}")
    return 0 if results and all(r[1] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
