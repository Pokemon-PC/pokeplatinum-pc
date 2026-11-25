# Modder le portage PC / Modding the PC port

Le portage PC garde la structure de fichiers de la cartouche (NitroFS) : le
code ouvre des chemins comme `fielddata/script/scr_seq.narc` ou
`poketool/personal/pl_personal.narc`, exactement comme sur DS. On peut donc
modifier le jeu de deux façons.

## 1. Mods de données (sans recompiler)

Le jeu cherche chaque fichier dans des « couches », de la plus prioritaire à
la moins prioritaire :

1. `mods/<langue>/` (ex. `mods/fr/`)
2. `mods/`
3. `pc_data/<langue>/` (fichiers extraits de votre ROM au premier lancement,
   ou couche anglaise `pc_data/en` construite depuis le dépôt)
4. `pc_data/<langue de la ROM>/` quand la langue choisie n'est pas celle de la ROM

Un mod ne contient **que les fichiers qu'il change**, au même chemin que dans
la ROM. Exemple : remplacer les scripts du terrain →
`mods/fielddata/script/scr_seq.narc`.

Fichiers utiles (chemin ROM → source dans le dépôt, d'après
`platinum.us/filesys.csv`) :

| Chemin ROM | Contenu | Source |
|---|---|---|
| `poketool/personal/pl_personal.narc` | statistiques de base | `res/pokemon/*/data.json` |
| `poketool/personal/evo.narc` | évolutions | `res/pokemon` |
| `poketool/personal/wotbl.narc` | attaques par niveau | `res/pokemon` |
| `poketool/waza/pl_waza_tbl.narc` | attaques | `res/moves` |
| `poketool/trainer/trdata.narc` | dresseurs | `res/trainers` |
| `fielddata/script/scr_seq.narc` | scripts du terrain | `res/field/scripts` |
| `fielddata/eventdata/zone_event.narc` | évènements des cartes | `res/field/events` |
| `fielddata/encountdata/pl_enc_data.narc` | rencontres | `res/field/encounters` |
| `msgdata/pl_msg.narc` | textes (724 banques) | `res/text/*.json` |

Pour produire ces fichiers à partir des sources éditables du dépôt, utilisez
la chaîne de construction DS du projet (`make`, meson) et copiez les NARC
obtenus (dans `build/res/...`) dans `mods/`. Une ROM modifiée fonctionne
aussi : placez-la à côté de l'exécutable après avoir supprimé `pc_data/` pour
la ré-extraire.

Langue : les sources du dépôt sont celles de la version américaine. Un mod de
**texte** construit depuis le dépôt est en anglais ; pour un mod en français,
partez des textes de votre ROM. La couche anglaise (`pc_data/en`) se
reconstruit avec la cible CMake `ppl_lang_en` (`tools/pc_port/build_lang_en.py`).

## 2. Mods de code (recompiler)

Tout le code du jeu (`src/`, `lib/`) est compilé tel quel pour PC. Modifiez-le
puis recompilez (voir `PORTING_ROADMAP.md`, section construction) :

```
cmake -S . -B build-pc -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-pc
```

Règles à respecter :

- le code spécifique au PC va sous `#ifdef PLATFORM_PC` (ou dans
  `src/platform_pc/`) pour que la ROM DS reste identique (« matching ») ;
- les pointeurs font 8 octets sur PC : n'utilisez pas de tailles DS en dur
  (`0x1C`, `index * 4`, tableaux `u32` qui servent de stockage pour des
  structures à pointeurs…), préférez `sizeof` ;
- les données de l'IA des dresseurs (`src/battle/trainer_ai/script.s`) sont
  assemblées pour le PC par `tools/pc_port/asm_data.py`.

Outils de test (variables d'environnement, voir
`src/platform_pc/nitro/nitro_pc.c` et `src/platform_pc/game_test_hooks_pc.c`) :
`PPL_TURBO=1`, `PPL_MUTE=1`, `PPL_EXIT_AFTER=N`, `PPL_TEST_KEYS`,
`PPL_TEST_TOUCH`, `PPL_TEST_WARP`, `PPL_TEST_GIVE_MON=ESPÈCE:NIVEAU`,
`PPL_TEST_WILD=ESPÈCE:NIVEAU:IMAGE`, `PPL_DUMP_FRAMES=N,M`,
`PPL_DUMP_VIDEO=1` (registres, VRAM et textures avec chaque image),
`PPL_LAYERS=hex` (couches 2D affichées), `PPL_HEAPLOG=N`…

---

## English summary

Files are looked up in `mods/<lang>/`, `mods/`, `pc_data/<lang>/` (extracted
from your ROM, or the English layer built from the repository), then the
ROM's own language. A mod only contains the files it changes, at the same
NitroFS path as in the ROM (e.g. `mods/fielddata/script/scr_seq.narc` for
field scripts). Build those files with the project's regular DS build from
the editable sources, or simply use a modified ROM (delete `pc_data/` so that
it is extracted again). Code mods: edit `src/` and rebuild with CMake; keep
PC-only changes under `#ifdef PLATFORM_PC`, and never assume 4-byte pointers.

Never share `pc_data/`, ROMs or anything extracted from them.
