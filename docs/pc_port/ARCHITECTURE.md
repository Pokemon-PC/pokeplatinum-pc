# Architecture du portage PC

Le principe : **le code du jeu ne change pas**. Les 1012 fichiers C décompilés
de Pokémon Platine, le SDK Nintendo décompilé (NitroSDK) et NitroSystem sont
compilés tels quels pour le PC ; ce qui, sur la console, est du matériel
(mémoire, écrans, puce son, cartouche, écran tactile, horloge) est remplacé
par une « console virtuelle » logicielle, écrite en C dans
`src/platform_pc/`.

```
 code du jeu (src/, lib/)  ─┐
 NitroSDK / NitroSystem    ─┼─►  couche Nitro PC (src/platform_pc/nitro/)  ─►  SDL2 + OpenGL
 (sous-projets décompilés)  ─┘     OS, FS, carte, son, 2D, 3D, entrées          (fenêtre, son, manette)
```

## Mémoire : la carte mémoire DS à ses adresses d'origine

Le jeu manipule des adresses en dur (registres à `0x04000000`, VRAM à
`0x06000000`, palettes, OAM) et range souvent des adresses dans des mots de
32 bits. Le portage réserve donc, au démarrage, les plages de la DS **aux
mêmes adresses** dans l'espace du processus (`hw_pc.c`) :

| Plage | Contenu |
|---|---|
| `0x01FF0000`… | ITCM, RAM principale (4 Mio), WRAM, DTCM |
| `0x04000000` | registres d'E/S (lus par les moteurs 2D/3D/son du PC) |
| `0x05000000` | palettes |
| `0x06000000` | VRAM (banques A–I, recâblées selon `VRAMCNT`) |
| `0x07000000` | OAM |

L'exécutable est lié à une base fixe basse (`0x10000000`), sans PIE, et la pile
du jeu est placée sous 4 Gio (`pc_run_on_low_stack`) : des adresses de code,
de données et de pile tiennent ainsi dans 32 bits, comme sur DS. Sous
Windows, si une DLL occupe déjà ces plages, le jeu se relance suspendu pour
les réserver avant tout chargement (`PPL_NO_RELAUNCH=1` pour le désactiver).

Les tas du jeu (`src/heap.c`) sont **quatre fois plus grands** que sur DS : les
structures contenant des pointeurs grossissent en 64 bits.

## Couche Nitro (`src/platform_pc/nitro/`)

| Fichier | Rôle |
|---|---|
| `nitro_pc.c` | boucle principale, VBlank à 60 Hz, crochets de test, captures |
| `os_nitro_pc.c`, `os_extra_pc.c` | `OS_*` : arènes, interruptions, alarmes, ticks, threads |
| `fs_nitro_pc.c` + `../fs_pc.c` | `FS_*` : système de fichiers de la cartouche (NitroFS) lu depuis `pc_data/<langue>`, couches de mods, overlays |
| `card_nitro_pc.c` + `../save_pc.c` | mémoire de sauvegarde (flash 512 Kio) ↔ `save.bin` |
| `ppu_pc.c` | moteurs 2D A et B, ligne par ligne : fonds texte/affines/bitmap, sprites (OBJ), fenêtres, mélanges, luminosité, capture, DMA H-Blank |
| `gpu3d_pc.c` | moteur 3D : FIFO de commandes G3, matrices, lumières, découpage, rastérisation logicielle avec textures, transparence, brouillard, contours |
| `vram_pc.c` | banques de VRAM et leurs affectations (`VRAMCNT`), emplacements cachés (textures, palettes étendues) |
| `snd_pc.c` + `../audio_pc.c` | puce son ARM7 réécrite : séquenceur SSEQ, banques, ondes (PCM8/16, ADPCM, PSG, bruit), enveloppes, LFO, réverbération/capture ; mixage 32 768 Hz stéréo vers SDL |
| `input_nitro_pc.c` + `../input_pc.c` | boutons, écran tactile (souris), manette SDL, raccourcis |
| `rtc_nitro_pc.c` | horloge temps réel (heure du PC) |
| `mi_*`, `sdk_asm_pc.c`, `dgt_pc.c` | copies mémoire, DMA, fonctions du SDK écrites en assembleur ARM (réécrites en C), MD5/SHA-1 |
| `wm_pc.c`, `misc_nitro_pc.c`, `online_data_pc.c` | sans-fil et en ligne : répondent « pas de partenaire » / hors ligne |

Le rendu final (`../gfx_pc.c`) envoie les deux écrans 256×192 à OpenGL, avec
dispositions (`large_top`, `stacked`, `side_by_side`) et filtres.

## Overlays

La DS charge à la demande des « overlays » (122 morceaux de code qui se
partagent la même mémoire). Sur PC, tout le code est présent, mais chaque
overlay garde ses variables dans une section à part
(`tools/pc_port/gen_overlay_sections.py`) : `FS_LoadOverlay` les remet à
leur état initial comme le ferait un rechargement sur DS, et rejoue les
initialisations statiques (`NitroStaticInit`).

## Données : jamais dans le dépôt ni le paquet

Au premier lancement, `install_pc.c` cherche la ROM du joueur (chemin
explicite `--rom`, dossier du jeu, puis `rom_dir` de `config.ini`),
reconnue à son en-tête (code `CPU*`), et extrait ses 340 fichiers dans
`pc_data/<langue>`. Le système de fichiers cherche ensuite chaque fichier dans
des couches :

1. `mods/<langue>/`, 2. `mods/`, 3. `pc_data/<langue choisie>/`,
4. `pc_data/<langue de la ROM>/` (quand on joue dans une autre langue).

Les ROM françaises rangent 32 archives sous `resource/fra/...` au lieu des
chemins de la version US : `fs_pc.c` les retrouve par leur nom.

**Choix de langue** : la couche `pc_data/en` est construite depuis le dépôt
(`build_lang_en.py`) — les 724 banques de texte encodées par `msgenc` à
partir de `res/text/*.json` et des banques générées (espèces, attaques,
objets, dresseurs, carte), plus 20 fichiers déjà anglais de `res/prebuilt`.
Le reste (graphismes, sons, cartes) vient de la ROM du joueur.

## Génération des en-têtes sans meson

Le build DS génère de nombreux en-têtes avec meson et des outils
(metang, msgenc, outils `*proc` de pret). `tools/pc_port/gen_headers.py`
refait cela pour CMake : énumérations metang, identifiants de messages,
index NARC (vérifiés contre la ROM), en-têtes des cartes et des sons, tables
de données (espèces, dresseurs, attaques, objets) produites par les outils de
pret compilés pour la machine hôte.

## SDK adapté au 64 bits

Les en-têtes du SDK sont écrits pour Metrowerks/ARM. `make_sdk_override.py`
en fait des copies (`src/platform_pc/sdk_override/`) avec une branche
`PLATFORM_PC` : types `u32`/`s32` en `int`, alignements, plateforme, registres
3D redirigés vers le FIFO PC, champs des structures « plaquées » sur les
fichiers (`PC_PTR32`, pointeurs 32 bits relatifs) ; quelques sources du SDK
contenant de l'assembleur ARM sont copiées sans lui
(`src/platform_pc/sdk_src/`), leurs fonctions réécrites en C.

## Pièges 64 bits corrigés dans le jeu (sous `#ifdef PLATFORM_PC`)

| Symptôme | Cause | Correction |
|---|---|---|
| Tas épuisé, corruption | tailles DS calculées avec des pointeurs de 4 octets | tas ×4 (`heap.c`), tampon de particules ×4 (`particle_system.c`) |
| Jauges de combat en vrac, menu tactile absent | `Sprite.animData` (`u32[29]`) sert de stockage aux structures d'animation NNS qui contiennent des pointeurs : débordement sur `imageProxy` | tableau dimensionné par `sizeof` (`include/sprite.h`) |
| Sauvegarde sans effet, sauts de script faux | ordre d'évaluation + décalage `u32` ajouté à un pointeur (saut arrière = +4 Gio) | lecture signée d'abord (`scrcmd.c`, `script_manager.c`, `frscrcmd.c`) |
| Effets de terrain : assert | la liste des tâches VBlank, vidée par interruption sur DS, se remplit | interruption servie sur place puis nouvel essai (`sys_task.c`) |
| Écran noir, logo sombre | VRAM recâblée sans synchronisation ; ports 3D | `pc_vram_sync()` après chaque écriture de `VRAMCNT`, registres G3 redirigés |
| Division par zéro | l'ARM renvoie 0 | garde (`capture.c`) |
| Tables d'adresses de fonctions en `u32` | adresses 64 bits | `uintptr_t` (menus du Souterrain, Sac) |

La vérification `tools/pc_port/check_ds_sources.py ORIGINAL .` confirme que,
une fois le code `PLATFORM_PC` retiré, les sources sont identiques à la
décompilation d'origine (donc que la ROM DS reste « matching »).
