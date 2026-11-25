# Portage PC natif de Pokémon Platine — feuille de route

Objectif : exécuter le code décompilé de Pokémon Platine (pret/pokeplatinum)
nativement sur PC (Windows `.exe` d'abord, puis Linux), sans émulateur, avec
**CMake + Clang + SDL2 + OpenGL**, au même niveau de finition que le port
HeartGold : titre → nouvelle partie → monde → combat → sauvegarde/rechargement,
son, manette, paquet, choix de la langue. Le build DS (meson, ROM « matching »)
reste intact : tout le code PC est dans `src/platform_pc/`, `tools/pc_port/`,
`cmake/`, ou sous `#ifdef PLATFORM_PC` avec le code d'origine dans `#else`.

La ROM du joueur (Platine FR, code `CPUF`) fournit les données au premier
lancement ; ni ROM, ni données extraites (`pc_data/`), ni sauvegardes ne vont
dans git ni dans le paquet.

## Jalons

| Jalon | Contenu | Critère de sortie | État |
|---|---|---|---|
| **M0** Audit | Inventaire du code (C / asm), des bibliothèques, de la génération des en-têtes | Ce document, §1 | ✅ |
| **M1** Squelette | CMake, couche PC reprise de HGSS, en-têtes générés sans meson ni Metrowerks | `cmake --build` produit tous les en-têtes ; index NARC vérifiés contre la ROM | ✅ |
| **M2** Couche Nitro | En-têtes SDK ntrtwl en LP64, carte mémoire DS, adaptateurs OS/FS/PAD/CARD/RTC | Autotest CTest vert | ✅ 783/783 |
| **M3** Tout le jeu | Les 1012 fichiers du jeu + SDK + NitroSystem compilés et liés | `NitroMain()` tourne jusqu'au titre | ✅ |
| **M4** 2D | Moteur 2D logiciel (repris de HGSS) | Intro et écran titre affichés | ✅ |
| **M5** 3D | Rastériseur logiciel (repris de HGSS) | Monde 3D, combats | ✅ |
| **M6** Son | Pilote son ARM7 réécrit (repris de HGSS) | Musique du titre | ⏳ séquenceur actif, pas encore écouté |
| **M7** Jeu | Nouvelle partie → monde → combat → sauvegarde → rechargement, langue, manette | Parcours scripté vert | ✅ (manette non testée physiquement) |
| **M8** Finitions | Paquet Windows, Linux (WSL), docs joueur | Paquet + test Linux | ✅ (Windows + Linux) |

## 1. Audit (M0, 2026-11-25)

Dépôt : pret/pokeplatinum, commit `485c400f9`.

### 1.1 Ce qui est en C, ce qui est en assembleur

Contrairement à ce que laissait penser l'état passé de la décomp, **le code du
jeu de Platine est aujourd'hui entièrement en C** : `platinum.us/main.lsf.in`
lie 1013 objets, dont 1012 sources `.c` de `src/` (122 overlays). Il ne reste
que trois fichiers `.s` :

| Fichier | Nature | Choix pour le PC |
|---|---|---|
| `src/battle/trainer_ai/script.s` | **Données** : scripts de l'IA des dresseurs (macros, `gTrainerAITable`), liées dans l'ARM9 | (b) assemblé pour l'hôte : ce ne sont que des données (voir §1.4) |
| `lib/crypto/asm/rc4*.s` | Chiffrement RC4 (en ligne uniquement) | non compilé : stubs hors ligne |
| `subprojects/libsyscall/asm/_secure.s` | Appels BIOS | remplacé par la couche Nitro PC |

### 1.2 Bibliothèques Nintendo

Le build DS ne contient pas le SDK : meson télécharge les décomps publiques du
projet ntrtwl aux révisions des `subprojects/*.wrap` (NitroSDK 4.2.30001,
NitroSystem 071126.1, NitroDWC 2.2.30008, NitroWiFi 2.1.30003, libvct 1.3.1)
et `metang` (générateur d'énumérations en Python). Le PC utilise **les mêmes
sources, aux mêmes révisions** : `python tools/pc_port/fetch_subprojects.py`.
Le compilateur Metrowerks (metroskrew) n'est **pas** téléchargé et n'est pas
nécessaire.

| Module | Choix PC (a = C d'une décomp sœur, b = portage à la main, c = recompilation statique) |
|---|---|
| NitroSDK fx/gx/math/mi/std (C) | Recompilé tel quel, sauf les fichiers en ARM ou matériels remplacés par la couche PC (`gxasm`, `mi_memory`, `mi_swap`, `mi_uncompress`, `mi_dma*`, `qsort`…) |
| NitroSDK os/fs/card/pad/rtc/snd/spi/wm/mb/ctrdg | Remplacés par la couche Nitro PC reprise de HGSS (`src/platform_pc/nitro`) |
| NitroSDK **dgt** (MD5/SHA-1) | Seulement une bibliothèque ARM précompilée (`libdgt.a`) → **(b)** réécrit en C : `src/platform_pc/nitro/dgt_pc.c` (algorithmes standard) |
| NitroSystem g2d/g3d/gfd/snd/fnd (C) | Recompilé tel quel |
| NitroDWC, NitroWiFi, libvct, ppwlobby (C++) | Stubs hors ligne (serveurs fermés) |
| lib/spl (particules), lib/gds, lib/crypto (C) | Recompilés (sauf RC4 asm) |

La recompilation statique (c), mise au point pour Noire 2, **n'est nécessaire
pour aucun module** de Platine.

### 1.3 En-têtes générés sans meson

Le code inclut ~280 en-têtes produits par le build DS. `tools/pc_port/gen_headers.py`
(appelé par CMake) les produit sans meson ni Metrowerks :

| En-têtes | Source | Méthode |
|---|---|---|
| `generated/*.h` (104 énumérations) | `generated/*.txt` | `metang.py` du sous-projet, exécuté dans le même processus |
| `res/text/bank/*.h` (723) | `res/text/*.json` + JSON produits par les outils | format de `msgenc -H` |
| `*.naix` (61 index NARC) | fichiers `.order`, listes calculées (espèces, dresseurs, formes, icônes), `scripts.order`… | format de `nitroarc --index`, **chaque index vérifié contre le nombre de membres de l'archive de la ROM du joueur** |
| `res/sound/pl_sound_data.naix` | `res/sound/pl_sound_data.json` | format de `nitrosfx -naix` |
| `res/field/maps/headers.h`, `map_matrix/zone_event/pl_enc_data/scr_seq.naix` | `res/field/maps/*/header.json`, `groups.json`, `*.order` | réécriture Python de la partie de `mapproc` qui les écrit (mapproc utilise `regex.h`, absent sous Windows) |
| tables d'espèces, objets, dresseurs, attaques, Pokédex, Frontière | JSON de `res/` | outils `tools/dataproc` du dépôt (`speciesproc`, `itemproc`…), compilés pour l'hôte par `cmake/PplHostTools.cmake` (petite compatibilité `open_memstream` pour Windows) |
| `nitro/fx/fx_const.h` | NitroSDK `gen/nitro/fx` | script Python du SDK |

### 1.4 Différences ROM FR / dépôt US

- 32 archives localisées sont rangées sous `resource/fra/…` dans la ROM FR au
  lieu de leur chemin US (ex. `demo/title/titledemo.narc` →
  `resource/fra/title/titledemo.narc`), même nom de fichier. La couche
  fichiers PC les retrouve par leur nom.
- La ROM FR est la révision 0 (octet 0x1E de l'en-tête) ; le dépôt compile la
  révision 1 par défaut (`PPL_REVISION`).

## 2. Décisions

| Décision | Choix | Raison |
|---|---|---|
| Compilateur | Clang (llvm-mingw sous Windows) | `__ptr32`, sections par overlay, comme HGSS |
| Couche PC | Copie adaptée de `pokeheartgold/src/platform_pc` (préfixe `PPL_`) | même moteur NitroSDK/NitroSystem ; pas de dépendance entre dépôts au build |
| En-têtes SDK modifiés | Copies dans `src/platform_pc/sdk_override/` faites par `tools/pc_port/make_sdk_override.py` (types 32 bits, DTCM, IDs d'overlay, diviseur) | les sous-projets restent intacts |
| Langue | `GAME_LANGUAGE` → variable `gPcGameLanguage` sur PC (ROM du joueur / `--lang`) | un seul exécutable FR/EN |
| Données | ROM du joueur extraite au premier lancement (`pc_data/fr`) | légal ; rien de Nintendo dans le dépôt |

## 3. Adaptations faites pour compiler le SDK ntrtwl et le jeu avec Clang

Le port HGSS utilisait une version « pokediamond » des en-têtes du SDK ;
Platine utilise les en-têtes réels du SDK (ntrtwl), écrits pour Metrowerks.

| Problème | Solution (outil) |
|---|---|
| En-têtes conditionnés par `SDK_CW`/`__MWERKS__` (alignement, `weak`, plateforme) | branche `PLATFORM_PC` dans les copies (`make_sdk_override.py`) |
| `u32`/`s32` en `long` | `int` sous `PLATFORM_PC` (`nitro/types.h`) |
| En-têtes DWC/GameSpy qui testent `_WIN32` (inclusion de `<windows.h>`) | `defined(_WIN32) && !defined(PLATFORM_PC)` ; `_NITRO` défini comme pour le DS |
| Fonctions `asm` ARM dans des sources C du SDK (matrices, flux de décompression, FIFO) | copies sans ces fonctions (`src/platform_pc/sdk_src`), équivalents C dans `nitro/sdk_asm_pc.c` |
| `libdgt.a` précompilé (MD5/SHA-1) | `nitro/dgt_pc.c` |
| En-têtes MSL absents (`extras.h`, `null.h`, `wchar_t.h`, `<cstdlib>`) | équivalents minimaux dans `nitro_compat/` |
| `NitroStaticInit` (initialisations au chargement d'un overlay, ex. applis du Pokétch) | enregistrées par un constructeur, rejouées par `FS_LoadOverlay`/`FS_StartOverlay` |
| En-tête précompilé `global_pch.h` du build DS | forcé (`-include`) sur les sources du jeu |
| `wchar_t` 16 bits sur DS | `-fshort-wchar` |
| Structures NitroSystem plaquées sur les fichiers (pointeurs « dépaquetés ») | champs `PC_PTR32` dans `nnsys/g2d/fmt/*.h`, `nnsys/snd/sndarc.h` |
| Casts utilisés comme lvalues, adresses de fonctions dans des `u32` en initialiseur statique, chaînes `\u` de Shift-JIS, variable définie dans un en-tête (gds) | corrigés au cas par cas sous `PLATFORM_PC` (`tools/pc_port/pcfix.py`) |
| Appels de fonctions non déclarées | tolérés (comme Metrowerks) **sauf** celles qui renvoient un pointeur, déclarées sous `PLATFORM_PC` (`tools/pc_port/implicit_decls.py`) |
| Fonctions en ligne / matériel absent (DWC, Wi-Fi, chat vocal, slot GBA, micro : 155) | stubs hors ligne générés (`tools/pc_port/gen_stubs.py` → `generated/stubs`) |
| Tas dimensionnés pour des pointeurs de 4 octets | tas ×4 sous `PLATFORM_PC` (`src/heap.c`), vérificateur `PPL_HEAPCHECK=1` |
| Tampon de travail des particules (lib/spl) dimensionné pour le DS | tampon ×4 possédé par le système de particules (`src/particle_system.c`) |
| `Sprite.animData` : `u32[29]` qui sert de stockage aux structures d'animation NNS (pointeurs) — débordait sur `imageProxy` (tuiles OBJ lues à l'adresse 0 : jauges de combat en vrac, menu tactile du combat absent) | tableau `u64` dimensionné par `sizeof` de l'union des trois structures (`include/sprite.h`) |
| Ordre d'évaluation et décalages `u32` ajoutés à des pointeurs (sauts de script) | lecture signée du décalage d'abord (`src/scrcmd.c`, `script_manager.c`, `overlay104/frscrcmd.c`) |
| Liste de tâches VBlank pleine (le DS la vide par interruption) | interruption VBlank servie sur place puis nouvel essai (`src/sys_task.c`) |
| Écritures `VRAMCNT` / ports G3 | `pc_vram_sync()` après chaque écriture, registres G3 redirigés vers le FIFO PC (`make_sdk_override.py`) |
| Division par zéro (l'ARM renvoie 0) | garde sous `PLATFORM_PC` (`capture.c`) |

## 4. Journal

- **2026-11-25** — Audit. Sous-projets récupérés aux révisions des `.wrap`.
  Couche PC de HGSS copiée. `gen_headers.py` : 104 énumérations, 723 banques
  de texte, 61 index NARC (tous égaux à la ROM FR), en-têtes des cartes et des
  sons, outils `dataproc` compilés pour Windows. ROM FR extraite (340
  fichiers). Configuration CMake : 1012 fichiers du jeu, 1144 au total,
  122 overlays (661 sources réinitialisées au chargement).
- **2026-11-25 (suite)** — Tout le jeu compile et se lie (`pokeplatinum.exe`).
  Autotest M2 vert (783 vérifications : tas du jeu, 187 NARC relues octet pour
  octet depuis la ROM FR, manette, sauvegarde, horloge, VBlank, 2D/3D).
  Premiers lancements : intro Game Freak (« PRESENTE »), écran titre
  (« APPUYEZ SUR START »), menu, intro du Prof. Sorbier en français
  (tutoriel tactile, Poké Ball, Laporeille, sexe, nom, rival), puis entrée
  sur le terrain (émission TV de la chambre). Corrigé en route : tas ×4,
  structures NitroSystem `PC_PTR32`, membres d'archives absents de la ROM FR,
  division par zéro de la réverbération (capture son), `NitroStaticInit`.
  Restant noté : logo du titre trop sombre et Giratina 3D absent du titre ;
  asserts au chargement des effets de terrain.
- **2026-11-25 (suite 2)** — Terrain en 3D (chambre du joueur), menu
  Start, sauvegarde (« Sauvegarde de beaucoup de données… ») et rechargement
  par CONTINUER, combat sauvage complet (Tiplouf N10 contre Keunotor N3,
  victoire, retour sur le terrain). Corrigé : écran noir et logo sombre
  (synchronisation des banques VRAM, ports G3), sauvegarde sans effet
  (ordre d'évaluation des sauts de script), tas du combat épuisé (tampon des
  particules), asserts des effets de terrain (liste VBlank).
- **2026-11-25 (suite 3)** — Rendu des combats corrigé : `Sprite.animData`
  trop petit en 64 bits écrasait l'adresse VRAM des sprites (diagnostic :
  options `PPL_LAYERS` et `PPL_DUMP_VIDEO`, qui écrit maintenant aussi les
  textures 3D). Vérifié : Tiplouf de dos, Keunotor, les deux jauges, menu
  tactile ATTAQUE / SAC / FUITE / POKÉMON. **Choix de langue** : couche
  `pc_data/en` construite depuis le dépôt (`tools/pc_port/build_lang_en.py`,
  cible `ppl_lang_en`, `msgenc` compilé comme outil hôte) — 724 banques de
  texte encodées depuis `res/text/*.json` + banques générées (espèces,
  attaques, objets, dresseurs, carte) + 20 fichiers de `res/prebuilt` ;
  `language = auto|fr|en`, `--lang`, `PPL_LANG`. Vérifié en anglais avec la
  ROM FR : menu principal, combat (BIDOOF, PIPLUP, FIGHT/BAG/RUN). Limite :
  images construites depuis des PNG (« PV » des jauges, logo) dans la langue
  de la ROM. Docs joueur `docs/pc_port/README_PC.md`, `MODDING.md`,
  `LAUNCHER.md`. Paquet `tools/pc_port/package.py` (3,6 Mio, aucune donnée
  du jeu) testé comme un joueur : ROM posée à côté, extraction au premier
  lancement, intro. Assert `SaveBlockInfo_Init` au démarrage : comptage
  arrondi par secteur (33 > 32) alors que les blocs sont placés à l'octet
  (127 464 octets ≤ 128 Kio) ; toutes les tailles de pages sont celles d'une
  compilation 32 bits — sans effet, gardé tel quel.
  Reste : écoute du son, manette physique, build Linux (WSL), partie longue.
- **2026-11-25 (suite 4)** — **Linux** (Ubuntu sous WSL2, Clang 21, Mesa
  llvmpipe, PulseAudio) : configuration et compilation natives sans aucune
  modification du code (1606 cibles, sources lues depuis le disque Windows,
  dossier de build `~/ppl-linux`), autotest CTest vert. Vérifié avec la ROM :
  combat sauvage avec une sauvegarde faite sous Windows, puis cycle complet
  sous Linux (Pokémon donné → sauvegarde → rechargement → combat), et la
  sauvegarde Linux relue sous Windows (format identique). Paquet Linux
  `PokemonPlatinum-PC-linux-x86_64.zip` (3,9 Mio). Corrigé : le crochet de
  test `PPL_TEST_SAVE_AT` appelait les fonctions HGSS (`SaveGameNormal`) →
  `FieldSystem_Save` / `SaveData_Save` de Platine. Crochets HGSS sans
  équivalent Platine (non branchés) : `PPL_TEST_GIVE_ITEMS`, `PPL_TEST_BADGES`,
  `PPL_TEST_MOVES`, `PPL_TEST_EGG_READY`, vidage de carte.
  Reste : écoute du son, manette physique, partie longue.
- **2026-11-25 (suite 5)** — ROM dans un dossier commun : clé `rom_dir` de
  `config.ini` (`[options]`, vide par défaut, commentée), option `--rom
  FICHIER|DOSSIER` et variable `PPL_ROM`. Ordre : chemin explicite, dossier
  de l'exe, `rom_dir`. Sélection par l'en-tête (code `CPU*`) seulement — le
  repli « première .nds trouvée » est supprimé, le dossier commun contenant
  aussi HeartGold, SoulSilver, Noire 2… ; une ROM explicite d'un autre jeu est
  refusée avec un message (`IPKF` testé). Machine de développement :
  `rom_dir = D:\LUC\pokemonall\roms` (build-pc/config.ini) et
  `/mnt/d/LUC/pokemonall/roms` (WSL, `~/ppl-linux/config.ini`). Vérifié :
  paquet vierge Windows et exe Linux extraient la ROM FR depuis ce dossier.
