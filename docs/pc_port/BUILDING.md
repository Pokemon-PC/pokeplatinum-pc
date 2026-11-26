# Construire le portage PC depuis les sources

Ce document explique comment compiler `pokeplatinum` (exécutable natif PC)
depuis ce dépôt, sous Windows et sous Linux. Pour jouer avec un paquet déjà
construit, voir `README_PC.md`.

Le build DS d'origine (meson, ROM « matching ») n'est pas touché : tout le
code propre au PC est dans `src/platform_pc/`, `tools/pc_port/`, `cmake/`, ou
sous `#ifdef PLATFORM_PC` avec le code d'origine dans `#else`.

## Prérequis

| Outil | Windows | Linux (Ubuntu/Debian) |
|---|---|---|
| Compilateur C/C++ | [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang, cible `x86_64-w64-mingw32`) | `clang` (Clang 16 ou plus récent) |
| CMake ≥ 3.20 + Ninja | `winget install Kitware.CMake Ninja-build.Ninja` | `apt install cmake ninja-build` |
| Python ≥ 3.10 | python.org | `apt install python3` |
| Git | Git for Windows | `apt install git` |
| SDL2 | téléchargé et compilé automatiquement (`PPL_FETCH_SDL2=ON`) | idem, ou `apt install libsdl2-dev` ; en-têtes X11/Wayland, ALSA/PulseAudio pour SDL |
| OpenGL 3.3 | pilote de la carte graphique | Mesa (`libgl1-mesa-dev`) |

Aucun outil propriétaire (Metrowerks, Nitro SDK) n'est nécessaire : le SDK
utilisé est celui, décompilé, des sous-projets publics de pret.

## 1. Récupérer les sous-projets

Les bibliothèques Nintendo décompilées (NitroSDK, NitroSystem, NitroDWC,
NitroWiFi, libvct) et les outils de pret (metang, yyjson) sont des
sous-projets meson ; ce script les clone aux révisions des fichiers `.wrap`
et applique leurs correctifs :

```
python tools/pc_port/fetch_subprojects.py
```

## 2. Données du jeu (votre ROM)

La compilation vérifie les index d'archives (`*.naix`) contre les fichiers de
**votre** ROM, et la couche anglaise (choix de langue) se construit à partir
de ces fichiers. Extrayez votre ROM Platine dans `pc_data/<langue>` :

- soit en lançant une première fois le jeu (il extrait la ROM tout seul, voir
  `README_PC.md`, clé `rom_dir` de `config.ini`) ;
- soit à la main : `python tools/pc_port/nitrofs.py extract VOTRE_ROM.nds pc_data/fr`.

`pc_data/`, les ROM et les sauvegardes sont ignorés par git : ils ne doivent
jamais être commités ni distribués.

## 3. Configurer et compiler

Windows (PowerShell, llvm-mingw dans le PATH) :

```
cmake -S . -B build-pc -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-pc
```

Linux :

```
cmake -S . -B build-linux -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-linux
```

Options utiles :

| Option CMake | Défaut | Effet |
|---|---|---|
| `PPL_ROM_DATA_DIR` | `pc_data/fr` | données extraites de votre ROM (index NARC vérifiés, modèle de la couche anglaise) |
| `PPL_FETCH_SDL2` | `ON` | télécharge et compile SDL2 si absent |
| `FETCHCONTENT_SOURCE_DIR_SDL2` | — | sources SDL2 déjà présentes (pas de téléchargement) |
| `PPL_PC_CONSOLE` | `ON` | Windows : garde une console pour le journal |
| `PPL_WARN_PTR_CASTS` | `OFF` | signale les conversions pointeur ↔ entier de taille différente (audit 64 bits) |

Ce que fait la compilation, dans l'ordre :

1. outils hôtes de pret compilés pour votre machine (`speciesproc`,
   `trainerproc`, `msgenc`…, dans `build-*/hosttools/`) ;
2. en-têtes générés sans meson (`tools/pc_port/gen_headers.py`) :
   énumérations metang, identifiants des 724 banques de texte, index NARC,
   cartes, sons, tables de données ;
3. copies adaptées des en-têtes et de quelques sources du SDK
   (`tools/pc_port/make_sdk_override.py` → `src/platform_pc/sdk_override`,
   `sdk_src`) ;
4. les 1012 fichiers C du jeu + SDK + NitroSystem + couche PC, liés en un
   exécutable à adresse fixe (image à `0x10000000`, mémoire DS aux adresses
   natives) ;
5. si `pc_data/fr` existe : la couche anglaise `pc_data/en`
   (cible `ppl_lang_en`, `tools/pc_port/build_lang_en.py`).

Les cibles utiles : `pokeplatinum` (le jeu), `ppl_m2_selftest` (autotest),
`ppl_lang_en` (couche anglaise), `ppl_generated_headers`.

## 4. Tester

```
ctest --test-dir build-pc          # autotest de la couche Nitro (783 vérifications)
```

Puis les tests de jeu scriptés (`TESTING.md`) : parcours de l'histoire,
tournée des lieux, analyse du son.

## 5. Paquet

```
python tools/pc_port/package.py --build build-pc       # Windows
python3 tools/pc_port/package.py --build build-linux   # Linux
```

Produit `dist/PokemonPlatinum-PC-<système>-<arch>.zip` : l'exécutable
(débarrassé des symboles), `README.txt`, un dossier `mods/` vide. Aucune donnée
du jeu (le script refuse les `.nds`, `.sav`, `.narc`, `.bin`). L'exécutable
avec symboles est gardé dans `dist/symbols/` pour décoder les rapports de
plantage (`tools/pc_port/symbolize_crash.py`).

## Linux sous WSL (Windows 11)

Le portage se compile et se teste tel quel dans Ubuntu sous WSL2 (WSLg
fournit l'affichage OpenGL via Mesa et le son via PulseAudio). Garder les
sources sur le disque Windows et le dossier de build dans le système de
fichiers Linux est plus rapide :

```
wsl -d Ubuntu
cmake -S /mnt/d/.../pokeplatinum -B ~/ppl-linux -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build ~/ppl-linux
```

Dans `~/ppl-linux/config.ini`, `rom_dir = /mnt/d/.../roms` permet au jeu de
trouver la ROM.

## Problèmes fréquents

- **L'antivirus bloque l'exécutable fraîchement compilé** (AVG, Avast…) :
  ajouter une exception pour le dossier de build.
- **« Permission denied » à l'édition de liens** : le jeu (ou un test) tourne
  encore et verrouille `pokeplatinum.exe`.
- **Index NARC différents de la ROM** à la génération des en-têtes : la ROM
  n'est pas une Platine révision 0/1 attendue, ou `PPL_ROM_DATA_DIR` pointe
  ailleurs.
- **Écran noir / OpenGL 3.3 requis** : mettre à jour le pilote graphique ; sous
  WSL, vérifier que WSLg est actif.
