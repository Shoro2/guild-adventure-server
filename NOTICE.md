# Third-party components and sources

Guild Adventure's own code is under AGPL-3.0-or-later (LICENSE). It builds on, and is distributed separately from,
the projects below. None of their code is contained in this repository except where a row says so.

| Component | License | How it is used |
|---|---|---|
| [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk), Playerbot branch of [mod-playerbots/azerothcore-wotlk](https://github.com/mod-playerbots/azerothcore-wotlk) | GPL-2.0-or-later | The server core, cloned at the commit in `tools/pins.json`. `patches/azerothcore/` changes it and is under the same license. |
| [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) | GPL-2.0-or-later | The bot AI, cloned at the pinned commit. `patches/mod-playerbots/` changes it and is under the same license. |
| [mod-ah-bot-plus](https://github.com/NathanHandley/mod-ah-bot-plus) | GPL-2.0-or-later (file headers) | Auction house buyer, cloned at the pinned commit. |
| [mod-autolearn-skills](https://github.com/poorhatsoap/mod-autolearn-skills) | AGPL-3.0 | Weapon skills and riding on level-up, cloned at the pinned commit. |
| [mod-aoe-loot](https://github.com/azerothcore/mod-aoe-loot) | AGPL-3.0 | The auto loot of `modules/mod-guild-adventure/src/Storage/AutoLoot.cpp` descends from it (through the author's own mod-auto-loot). |
| [Recast/Detour](https://github.com/recastnavigation/recastnavigation) | zlib | `tools/navpath` builds against the copy inside the AzerothCore checkout; nothing of it is in this repository. |
| wowbot335 (the author's own project) | MIT | The talent orders in `modules/mod-guild-adventure/data/sql/db-world/base/ga_spec_template.sql` were generated from its talent profiles. |

World of Warcraft data (spell, item, quest, creature and zone IDs, names and coordinates) appears in the SQL files and
in the documentation as facts needed to run the game. No Blizzard file is part of this repository.
