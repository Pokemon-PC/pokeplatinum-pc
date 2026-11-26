# Tester le portage PC

Trois niveaux de tests, tous sans intervention humaine :

1. **Autotest de la couche Nitro** (`ctest`) — sans lancer le jeu ;
2. **Tests de jeu scriptés** — le vrai jeu, piloté par des variables
   d'environnement `PPL_TEST_*` (touches, téléportations, combats…), avec
   captures d'écran et journal ;
3. **Analyse du son** — enregistrement de la sortie audio et rapport
   automatique (niveaux, coupures, justesse, spectrogramme).

Seules l'écoute réelle du son et une vraie manette demandent un humain.

## 1. Autotest (`ppl_m2_selftest`)

```
ctest --test-dir build-pc --output-on-failure
```

783 vérifications : tas du jeu, archives NARC relues octet pour octet depuis
`pc_data/<langue>`, manette, sauvegarde, horloge, VBlank, 2D/3D.

## 2. Tests de jeu scriptés

### Parcours de l'histoire : `tools/pc_port/story_run.py`

```
python tools/pc_port/story_run.py --save DEPART.bin --out DOSSIER [--lang fr|en] [--exe EXE]
```

Une étape par dresseur majeur : les 8 champions d'arène, le Conseil 4, la
Maîtresse Cynthia, puis le Panthéon. Chaque étape :

1. charge la sauvegarde de l'étape précédente (« CONTINUER ») ;
2. téléporte le joueur devant le dresseur (position lue dans
   `res/field/maps/<carte>/events.json`), le fait se tourner et lui parler ;
3. appuie sur A dès que le joueur n'a pas la main (`PPL_TEST_MASH`) : le
   **vrai script** du dresseur tourne (dialogues, combat, badge, CT) ;
4. sauvegarde la partie, qui sert de départ à l'étape suivante.

Une étape réussit si son effet est constaté dans le journal : badge en plus
(arènes), prime de combat (Conseil 4), arrivée au Panthéon (Maîtresse) — et
aucun plantage. Les captures (`frame_*.png`) et le journal de chaque étape
sont dans `DOSSIER/<étape>/`, le bilan dans `DOSSIER/report.txt`.

La première étape donne une équipe de test (`--team`, par défaut trois
Carchacrok N.100 avec Dracogriffe). Options : `--only NOM,...`,
`--from NOM`, `--no-team`, `--move`.

### Tournée des lieux : `story_run.py --places`

```
python tools/pc_port/story_run.py --places --save DEPART.bin --out DOSSIER [--lang en]
```

Téléporte le joueur dans ~45 lieux clés (villes, Monde Distorsion, Colonnes
Lances, QG Galaxie, Grand Marais, Zone de Combat, Tour de Combat, lacs,
Château Vieillot…), une capture par lieu (`place_<lieu>.png`).

### Planche contact

```
python tools/pc_port/contact_sheet.py planche.png DOSSIER/*/frame_*.png --cols 7
```

### Variables d'environnement (jamais actives sans elles)

Pilotage :

| Variable | Effet |
|---|---|
| `PPL_EXIT_AFTER=N` | quitte proprement après N images |
| `PPL_TURBO=1` | sans limite de vitesse |
| `PPL_MUTE=1` | son calculé mais haut-parleurs muets |
| `PPL_TEST_KEYS=N:TOUCHES[*D],...` | maintient TOUCHES (A B X Y L R START SELECT UP DOWN LEFT RIGHT, jointes par `+`) pendant D images (6 par défaut) à partir de l'image N |
| `PPL_TEST_TOUCH=N:X:Y[>X2:Y2],...` | touche l'écran du bas en (X, Y) ; `>` : glisser |
| `PPL_TEST_MASH=P[:DEPUIS]` | appuie sur A toutes les P images tant que le joueur n'a pas la main |
| `PPL_TEST_RESET_AT=N[,M]` | L+R+Start+Select (réinitialisation) à l'image N |
| `PPL_TEST_LID=DE:A` | couvercle fermé entre ces images (veille) |

État du jeu (appliqué dès que le joueur est libre sur le terrain) :

| Variable | Effet |
|---|---|
| `PPL_TEST_GIVE_MON=ESPÈCE:NIVEAU,...` | ajoute des Pokémon (le joueur en est le dresseur d'origine, donc ils obéissent) |
| `PPL_TEST_SPECIES=N` | le 1er Pokémon devient l'espèce N |
| `PPL_TEST_PARTY_LEVEL=N[+]` | équipe au niveau N (`+` : à 1 point du niveau suivant) |
| `PPL_TEST_MOVES=EMPL:ATTAQUE,...` | attaques du 1er Pokémon ; règle aussi le style de combat « Défini » et le texte rapide |
| `PPL_TEST_GIVE_ITEMS=ID[:QTÉ],...` | objets dans le Sac |
| `PPL_TEST_BADGES=N` | les N premiers badges |
| `PPL_TEST_FLAGS=F,...` / `PPL_TEST_VARS=V:N,...` | drapeaux et variables de scénario |
| `PPL_TEST_WARP=CARTE:X:Z[:N],...` | téléportations (au plus tôt à l'image N), jusqu'à 64 |
| `PPL_TEST_WILD=ESPÈCE:NIVEAU:IMAGE` | combat sauvage |
| `PPL_TEST_TRAINER=ID[:IMAGE],...` | combat contre un dresseur (sans son script) |
| `PPL_TEST_NO_ENCOUNTERS=1` | pas de rencontres sauvages en marchant |
| `PPL_TEST_NO_TRAINERS=1` | les dresseurs ne repèrent plus le joueur |
| `PPL_TEST_SAVE_AT=N` | sauvegarde à l'image N (dès que le joueur est libre) |
| `PPL_TEST_HOUR=H`, `PPL_TEST_DAY_OFFSET=N` | heure fixe ; N jours plus tard |

Observation :

| Variable | Effet |
|---|---|
| `PPL_DUMP_FRAMES=N,M,...` | captures `frame_NNNNNN.png` (écran du haut au-dessus de celui du bas) |
| `PPL_DUMP_VIDEO=1` | avec chaque capture, `frame_NNNNNN.video` : registres, palettes, OAM, VRAM, textures 3D |
| `PPL_DUMP_WINDOW=N` | `window.png` : la fenêtre telle qu'affichée |
| `PPL_LAYERS=hex` | couches 2D dessinées (bits 0-3 BG0-3 du moteur A, 4 ses OBJ, 5-9 moteur B) |
| `PPL_TEST_POS=N` | carte et case du joueur toutes les N images |
| `PPL_AUDIO_DUMP=fichier` | sortie audio brute (32 768 Hz, stéréo, 16 bits) |
| `PPL_HEAPCHECK=1` | vérifie tous les tas à chaque image (lent) |
| `PPL_HEAPLOG=N` | journalise chaque allocation du tas N |
| `PPL_BACKTRACE_AT=N` | pile d'appels du jeu à l'image N (blocages) |
| `PPL_WATCH=ADRESSE` | Windows : point d'arrêt matériel en écriture |

Le journal contient aussi, pendant les tests, des lignes `progress:`
(badges, argent, carte) et `test:` (actions des crochets).

### Plantages

Un plantage écrit `[ppl:crash]` et une pile d'adresses ; une vérification du
jeu qui échoue écrit `[ppl:assert]`. Pour les traduire en fonctions et lignes :

```
python tools/pc_port/symbolize_crash.py log.txt build-pc/pokeplatinum.exe
```

## 3. Analyse du son : `tools/pc_port/audio_report.py`

```
PPL_MUTE=1 PPL_AUDIO_DUMP=son.raw PPL_EXIT_AFTER=2700 build-pc/pokeplatinum.exe
python tools/pc_port/audio_report.py son.raw --png spectre.png [--start S] [--len S]
```

Rapport (Python pur, sans numpy) : durée, crête et saturation, décalage
continu, niveau par 100 ms, silences, **coupures** (silence court au milieu
de la musique), **craquements** (sauts d'échantillon), corrélation
gauche/droite, fréquence dominante et centroïde spectral par seconde,
**justesse** (écart moyen des pics à la gamme tempérée, en cents : une
mauvaise fréquence de sortie décalerait toutes les notes), et un
**spectrogramme** PNG (0–8 kHz, bas en haut). Code de sortie 1 si silence,
saturation ou coupures.

## Outils de diagnostic du rendu

- `PPL_DUMP_VIDEO=1` + scripts d'analyse : un `.video` contient, dans l'ordre,
  les registres E/S (0x2000 octets), les palettes (0x800), l'OAM (0x800), la
  VRAM telle que vue par le processeur (0x8A4000), puis les emplacements
  texture (512 Kio) et palettes de texture (96 Kio) de la 3D.
- `PPL_LAYERS` permet d'isoler une couche fautive.
