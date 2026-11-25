# Intégration avec le launcher (à lire avant de toucher au choix de langue)

Comme pour le portage HeartGold / SoulSilver, le choix de la langue se fera
**dans le launcher de l'utilisateur**, pas dans le jeu. Ne pas ajouter de menu
de langue dans le jeu : exposer seulement des réglages simples que le launcher
pilote, et adapter cette page quand le code du launcher arrive.

## Ce que le jeu offre au launcher

| Quoi | Comment | Remarques |
|---|---|---|
| Jeu | `pokeplatinum(.exe)` | |
| Langue | argument `--lang fr\|en` | priorité la plus haute |
| | variable d'environnement `PPL_LANG=fr\|en` | |
| | `config.ini` → `[options] language = auto\|fr\|en` | `auto` = langue de la ROM du joueur |
| ROM | `--rom FICHIER\|DOSSIER` | priorité la plus haute (variable `PPL_ROM`) |
| | dossier de l'exe | |
| | `config.ini` → `[options] rom_dir = DOSSIER` | vide par défaut ; à remplir par le launcher |
| | | ROM choisie par son en-tête (code `CPU*`), jamais par son nom ; extraite une seule fois dans `pc_data/<langue>` |
| Sauvegarde | `--save FICHIER` | défaut : `save.bin` à côté de l'exe |
| Taille de fenêtre | `--scale N` | sinon `[display] window_scale` |
| Données | `--data DOSSIER` (développement) | sinon détection automatique (ci-dessous) |

Codes de sortie : 0 normal ; 1 = données introuvables (une boîte de dialogue a
déjà expliqué au joueur quoi faire).

## Où sont les données (dossier de l'exe)

```
pokeplatinum.exe
config.ini                 créé au premier lancement
<ROM>.nds                  ROM du joueur (code de jeu CPU*), ou dans rom_dir
pc_data/<langue>/          ROM extraite au 1er lancement (langue de la ROM)
pc_data/en/                couche anglaise construite depuis le dépôt
mods/, mods/<langue>/
```

- La couche anglaise ne contient que ce qui dépend de la langue (textes
  `msgdata/pl_msg.narc` + 20 fichiers de `res/prebuilt`) ; le reste vient
  toujours de la ROM extraite, quelle que soit sa langue. Elle est produite
  par `tools/pc_port/build_lang_en.py` (cible CMake `ppl_lang_en`, lancée
  automatiquement si `pc_data/fr` existe au moment de la configuration).
- **À trancher avec le launcher** : `tools/pc_port/package.py` ne met **pas**
  `pc_data/en` dans le paquet (elle vient des sources de la décomp, même
  statut que le dépôt pret public, pas de la ROM). Soit le paquet l'inclut
  (option à ajouter), soit le launcher la fournit.
- Limites de l'anglais : logo du titre et images construites depuis des PNG
  (ex. « PV » des jauges de combat) restent dans la langue de la ROM.

## Ce qui ne doit jamais passer par le launcher

La ROM du joueur et les fichiers extraits (`pc_data/<langue de la ROM>`) : le
joueur fournit sa propre ROM. Un launcher qui distribue la ROM distribue le jeu.
