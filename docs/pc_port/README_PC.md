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

### Options de lancement

`pokeplatinum.exe [--lang fr|en] [--save FICHIER] [--scale N] [--data DOSSIER]`

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
