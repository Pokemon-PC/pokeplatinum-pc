Pokémon Version Platine / Platinum — portage PC natif / native PC port
=====================================================================

(English below)

FRANÇAIS
--------

Ce programme fait tourner Pokémon Version Platine nativement sur PC, à partir
du code source décompilé du jeu. Il ne contient AUCUNE donnée du jeu : vous
devez fournir la ROM de VOTRE PROPRE cartouche.

### Installation

1. Décompressez ce dossier où vous voulez.
2. Copiez la ROM de votre cartouche Pokémon Platine (fichier `.nds`, code de
   jeu CPU…) dans ce dossier, à côté de `pokeplatinum.exe`.
3. Lancez `pokeplatinum.exe`. Au premier lancement, les fichiers du jeu sont
   extraits dans `pc_data/<langue>` (quelques secondes). La langue suit votre
   ROM (française, anglaise…). Vous pouvez ensuite retirer la ROM.

La ROM peut aussi rester dans un autre dossier : indiquez-le dans
`config.ini`, section `[options]`, clé `rom_dir` (par ex.
`rom_dir = D:\Jeux\roms`). Le jeu cherche dans cet ordre : le chemin donné
par `--rom` (fichier ou dossier), le dossier du jeu, puis `rom_dir`. La bonne
ROM est reconnue par son en-tête (code de jeu CPU…), pas par son nom : le
dossier peut contenir les ROM d'autres jeux.

Votre partie est enregistrée dans `save.bin` (même format que la mémoire de
sauvegarde de la cartouche, 512 Kio).

### Langue

Le jeu prend la langue de votre ROM. Pour jouer en anglais avec une ROM
française, réglez `language = en` dans `config.ini` (section `[options]`), ou
lancez avec `--lang en` : les textes anglais viennent du code source du jeu
(`pc_data/en`), le reste de votre ROM. Le logo du titre et quelques images
(par ex. « PV » dans les jauges de combat) restent dans la langue de la ROM.

### Commandes par défaut

| DS | Clavier | Manette |
|---|---|---|
| Croix | Flèches | Croix / stick gauche |
| A / B / X / Y | X / Z / S / A | positions Nintendo (droite = A, bas = B) |
| L / R | Q / W | gâchettes hautes |
| Start / Select | Entrée / Maj droite ou Retour arrière | Start / Back |
| Écran tactile | souris (clic gauche) | — |

Raccourcis : **Tab** avance rapide (maintenue), **F8** avance rapide on/off,
**M** couper le son, **F12** capture d'écran (dossier `screenshots`), **F9**
disposition des écrans, **F10** fermer/ouvrir le couvercle (veille), **F11**
plein écran, **Échap** quitter. Réinitialisation : L+R+Start+Select, comme
sur la console.

Tout se change dans `config.ini` (touches, manette, raccourcis, vitesse de
l'avance rapide, plein écran, disposition `large_top` / `stacked` /
`side_by_side`, étirement, taille de fenêtre, filtre d'image).

### Le fichier `config.ini`

Créé à côté du jeu au premier lancement, commenté. Plusieurs touches peuvent
être liées à un bouton (séparées par des virgules). Supprimez le fichier pour
retrouver les réglages par défaut.

| Section | Clé | Valeurs |
|---|---|---|
| `[keyboard]` | `A`, `B`, `X`, `Y`, `L`, `R`, `Start`, `Select`, `Up`, `Down`, `Left`, `Right` | noms de touches SDL (`X`, `Return`, `Left Shift`, `F1`…) |
| `[gamepad]` | mêmes boutons | `a b x y back start leftshoulder rightshoulder dpup dpdown dpleft dpright leftstick rightstick lefttrigger righttrigger` |
| `[hotkeys]` | `Quit`, `SwapScreens`, `Lid`, `Fullscreen`, `FastForward`, `FastForwardToggle`, `Mute`, `Screenshot`, `Filter` | touches |
| `[options]` | `fast_forward_speed` | 2 à 10 fois, 0 = sans limite |
| | `analog_stick_dpad` | 1 = le stick gauche sert de croix |
| | `language` | `auto` (langue de la ROM), `fr`, `en` |
| | `rom_dir` | dossier où chercher la ROM (vide = dossier du jeu seulement) |
| `[display]` | `fullscreen` | 1 = plein écran au démarrage |
| | `layout` | `large_top` (grand écran du haut, écran tactile à droite, 16:9), `stacked` (comme la DS), `side_by_side` |
| | `stretch` | 1 = remplit la fenêtre en déformant |
| | `filter` | `sharp`, `nearest`, `smooth`, `scanlines`, `lcd` |
| | `window_scale` | taille de la fenêtre en multiples de 256×192 |

### Options de lancement

`pokeplatinum.exe [--lang fr|en] [--rom FICHIER|DOSSIER] [--save FICHIER] [--scale N] [--data DOSSIER]`

| Option | Effet |
|---|---|
| `--lang fr\|en` | langue du jeu (prioritaire sur `config.ini`) |
| `--rom` | ROM à utiliser, ou dossier où la chercher |
| `--save FICHIER` | fichier de sauvegarde (une partie par fichier) |
| `--scale N` | taille de la fenêtre |
| `--data DOSSIER` | données du jeu déjà extraites (développement) |

### Sauvegardes

La partie est dans `save.bin` (ou le fichier de `--save`), 512 Kio, au format
de la mémoire de la cartouche : la même sauvegarde fonctionne sous Windows et
sous Linux. Pour la protéger, copiez simplement ce fichier. Une sauvegarde
extraite d'une vraie cartouche (fichier `.sav` de 512 Kio) devrait pouvoir être
renommée en `save.bin` (non vérifié faute de sauvegarde de cartouche).

### Problèmes fréquents

- **« Données du jeu introuvables »** : la ROM n'est ni à côté du jeu ni dans
  `rom_dir`, ou ce n'est pas une ROM de Pokémon Platine (le jeu la reconnaît à
  son en-tête, code CPU…). Une ROM de Diamant, Perle ou HeartGold est refusée.
- **L'antivirus bloque ou supprime `pokeplatinum.exe`** : faux positif
  fréquent avec les exécutables non signés ; ajoutez une exception pour le
  dossier du jeu.
- **Fenêtre noire / message « OpenGL 3.3 required »** : mettez à jour le
  pilote de la carte graphique.
- **Pas de son** : vérifiez la touche M (couper le son) et le périphérique de
  sortie par défaut du système.
- **Changer de ROM** (autre langue, ROM modifiée) : supprimez `pc_data/` pour
  qu'elle soit extraite de nouveau.
- **Journal** : la console (Windows) affiche le journal ; en cas de plantage,
  il contient une ligne `[ppl:crash]` utile pour un rapport de bug.

### Limites

Les fonctions sans fil et en ligne (échanges et combats sans fil, Wi-Fi,
GTS, Souterrain à plusieurs, Wi-Fi Plaza) ne sont pas disponibles.

### Mods

Voir `docs/pc_port/MODDING.md` dans le dépôt source. En bref : un fichier
placé dans `mods/` (ou `mods/<langue>/`) au même chemin que dans la ROM
remplace l'original.


ENGLISH
-------

This program runs Pokémon Platinum natively on PC, built from the game's
decompiled source code. It contains NO game data: you must provide the ROM
dumped from YOUR OWN cartridge.

### Setup

1. Unzip this folder anywhere.
2. Copy your Platinum cartridge ROM (`.nds` file, game code CPU…) into this
   folder, next to `pokeplatinum.exe`.
3. Run `pokeplatinum.exe`. On first run the game files are extracted to
   `pc_data/<language>` (a few seconds); the language follows your ROM. You
   can remove the ROM afterwards.

The ROM can also stay in another folder: set it in `config.ini`, `[options]`,
key `rom_dir` (e.g. `rom_dir = D:\Games\roms`). Search order: the path given
with `--rom` (file or folder), the game's folder, then `rom_dir`. The right ROM
is recognized by its header (game code CPU…), not by its name, so the folder
may hold other games' ROMs.

Your game is saved in `save.bin` (same format as the cartridge's 512 KiB
backup memory).

### Language

The game uses your ROM's language. To play in English with a French ROM, set
`language = en` in `config.ini` (`[options]`) or start with `--lang en`:
English text comes from the game's source code (`pc_data/en`), everything else
from your ROM. The title logo and a few pictures (e.g. the "PV" label of the
battle HP bars) keep the ROM's language.

### Default controls

Arrows = D-pad, X/Z/S/A = A/B/X/Y, Q/W = L/R, Enter = Start, Right Shift or
Backspace = Select, mouse = touch screen. Gamepads use Nintendo positions
(east = A, south = B). Hotkeys: Tab fast-forward (hold), F8 fast-forward
toggle, M mute, F12 screenshot, F9 screen layout, F10 lid (sleep), F11
fullscreen, Esc quit. Soft reset: L+R+Start+Select.

Everything is configurable in `config.ini`.

### Limitations

Wireless and online features (trades, wireless battles, Wi-Fi, GTS,
multiplayer Underground, Wi-Fi Plaza) are not available.
