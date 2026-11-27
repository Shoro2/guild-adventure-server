# mod-guild-adventure

AzerothCore module with the rules of Guild Adventure on top of `mod-playerbots`: the adventure guild and its bot roster,
the orders the bots act on, progression and unlocks, the guild storage and economy, dungeon and raid runs, taking
control of a bot, and the `GADV` protocol of the addon.

## Requirements

- The AzerothCore Playerbot branch (`mod-playerbots/azerothcore-wotlk`, branch `Playerbot`) at the commit pinned in
  `tools/pins.json`, with the core patches of `patches/azerothcore/` applied. The module does not compile without them
  (`docs/core-patches.md`).
- `mod-playerbots` at its pinned commit with `patches/mod-playerbots/`.
- Optional: `mod-ah-bot-plus` (an auction house buyer) and `mod-autolearn-skills` (weapon skills, riding).

`tools\setup-server.ps1` sets all of this up on Windows (`docs/server-setup.md`).

## Layout

| Path | Content |
|---|---|
| `conf/` | `mod_guild_adventure.conf.dist`: every setting with its description |
| `src/` | C++ sources; the loader is `Addmod_guild_adventureScripts()` in `mod_guild_adventure_loader.cpp` |
| `src/Protocol/` | The `GADV` addon protocol: codec and the server side of each part (`docs/protocol.md`) |
| `src/Storage/` | Guild storage, loot filter, auto loot, crafting from the storage, auctions, shopping list (`docs/storage.md`) |
| `src/Perks/` | Guild perks (`docs/guild-management.md`) |
| `data/sql/db-world/base/` | World tables (`ga_zone_template`, `ga_unlock_template`, quest and dungeon tables, talent templates, ...) |
| `data/sql/db-characters/base/` | Characters tables (`ga_guild`, `ga_bot`, orders, storage, settings, ...) |
| `tests/` | Offline tests against stub headers (not part of the server build) |

The SQL files are idempotent; AzerothCore's database updater applies them at the worldserver's start.

## Where to read on

| Part | Main sources | Document |
|---|---|---|
| Guild, roster, recruitment, renown, unlocks, offline play | `GuildAdventureMgr`, `RosterService`, `ProgressionService`, `OfflineService`, `AchievementService`, `MilestoneService` | `docs/guild-management.md` |
| Orders and the planner | `OrderService*`, `Order*.cpp`, `WorldIndex`, `BotDriver`, `RoadMap`, `RiskModel`, `FightEscape`, `QuestInteraction`, `QuestEvents` | `docs/orders.md` |
| Professions, craft orders, farming | `Professions`, `CraftOrders`, `Farming`, `Fishing` | `docs/orders.md` |
| Dungeon and raid runs, roles | `Dungeons`, `DungeonNav`, `OrderDungeons.cpp`, `DungeonRoles`, `DungeonSupplies` | `docs/orders.md`, `docs/dungeon-roles.md` |
| Talent templates, fight profiles | `SpecService`, `FightProfiles`, `FightProfileStore` | `docs/specs.md`, `docs/fight-profiles.md` |
| Bot behaviour settings | `BotSettings` | `docs/bot-settings.md` |
| Take control, watch mode | `GuildBotHolder`, `ControlSwapService`, `ClientChannel` | `docs/take-control.md` |
| Performance counters | `Perf` | `docs/performance.md` |

## Commands

Every command lives under `.ga` (GM accounts; many also from the console and SOAP). `.ga info`, `.ga roster`,
`.ga bot plan <name>` and `.ga settings` are good first looks; each document lists the commands of its part.

## Tests

`tests/test_client_channel.cpp` compiles `src/ClientChannel.cpp` against the stubs in `tests/stubs/`:

```sh
g++ -std=c++20 -Wall -Wextra -I tests/stubs tests/test_client_channel.cpp src/ClientChannel.cpp -o test_cc && ./test_cc
```

`tests/gadv_codec_test.cpp` checks the protocol codec against `docs/protocol-vectors.txt`: `tools\test-gadv-codec.ps1`.
