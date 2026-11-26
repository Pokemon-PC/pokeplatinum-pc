# Portage PC natif de Pokémon Platine — documentation

| Document | Pour qui | Contenu |
|---|---|---|
| [README_PC.md](README_PC.md) | joueurs | installation, ROM (`rom_dir`), langue, commandes, options, limites (FR/EN) ; copié dans le paquet sous le nom `README.txt` |
| [MODDING.md](MODDING.md) | moddeurs | mods de données (couches de fichiers, chemins utiles), mods de code |
| [LAUNCHER.md](LAUNCHER.md) | auteur du launcher | ce que le jeu accepte (arguments, variables, `config.ini`), où sont les données |
| [BUILDING.md](BUILDING.md) | développeurs | compiler sous Windows et Linux, options CMake, paquet, problèmes fréquents |
| [TESTING.md](TESTING.md) | développeurs | autotest, parcours scripté de l'histoire, tournée des lieux, analyse du son, toutes les variables `PPL_*` |
| [ARCHITECTURE.md](ARCHITECTURE.md) | développeurs | console virtuelle, carte mémoire, couche Nitro, overlays, données et langues, pièges 64 bits |

État détaillé, décisions et journal : [`PORTING_ROADMAP.md`](../../PORTING_ROADMAP.md)
à la racine du dépôt.

Règle absolue : ni ROM, ni données extraites (`pc_data/`), ni sauvegarde ne
doivent être commitées ou distribuées. Le joueur fournit la ROM de sa propre
cartouche.
