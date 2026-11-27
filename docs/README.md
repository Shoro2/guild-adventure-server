# Technical documentation

How Guild Adventure works inside, system by system: for server operators and developers. The documents grew with the
work and keep its notes and test results; the code is the last word where they differ.

**Playing the game?** The [player guide](../guide/README.md) explains every feature from the player's side, without the
technical details.

| File | Content |
|---|---|
| [`server-setup.md`](server-setup.md) | Setting up, building and running the server on Windows, extracting the server data from your client, pinned versions |
| [`core-patches.md`](core-patches.md) | Every change to the AzerothCore core (`patches/azerothcore/`): files, reason, how the module uses it |
| [`guild-management.md`](guild-management.md) | The adventure guild: founding, bot accounts, recruitment, renown and unlocks, the offline window, achievements, tiered achievements, perks, commands |
| [`orders.md`](orders.md) | What the bots do: the orders (autopilot, levelling, follow, professions, dungeon runs, farm orders), the quest planner, travel, upkeep, risk, fights, squads, quest partners, dungeon and raid runs |
| [`take-control.md`](take-control.md) | Playing a guild bot yourself without either character leaving the world; watch mode |
| [`storage.md`](storage.md) | The guild storage, the loot filter, auto loot, crafting from the storage, the guild bank and tax, the shopping list |
| [`bot-settings.md`](bot-settings.md) | Bot behaviour settings the guild leader changes in game |
| [`specs.md`](specs.md) | Talent templates and how bots spend their talent points |
| [`dungeon-roles.md`](dungeon-roles.md) | Tank and healer roles, the dungeon set a bot collects, the change at a dungeon's entrance |
| [`fight-profiles.md`](fight-profiles.md) | Fight profiles: the bots' rotations as profiles the guild leader reads and edits in game |
| [`protocol.md`](protocol.md) | The `GADV` addon protocol between the addon and the module |
| [`protocol-vectors.txt`](protocol-vectors.txt) | Test vectors for the protocol codec (addon tests, `tools\test-gadv-codec.ps1`) |
| [`performance.md`](performance.md) | Server performance with many bots: profiles, findings, the optimisations, capacity tests |
| [`quest-item-gaps.md`](quest-item-gaps.md) | Quest items of levelling quests that have no source the planner knows |

More: [`../addon/README.md`](../addon/README.md) (addon), [`../tools/README.md`](../tools/README.md) (scripts),
[`../modules/mod-guild-adventure/README.md`](../modules/mod-guild-adventure/README.md) (module).

## Notes for readers

- **The design log.** Many places refer to "the design log" with a section number or a decision id (`§7 WG1`, `DQ9`).
  That is the project's internal design and decision log, which is not published; the documents here describe what was
  built and why.
- **Evidence tiers.** `T0` = written, not compiled or run; `T1` = compiles (or the addon loads without Lua errors);
  `T2` = seen working in the world on a test server. "ga-probe" in test notes is an internal harness that drives a
  client for such checks; it is not part of this repository.
- **The owner** is the project's author, who plays the guild leader on the test server; "[quote left out]" marks his
  requests, which were quoted in German.
