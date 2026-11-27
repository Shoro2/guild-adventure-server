# tools

PowerShell helpers that set up, build and run the server on Windows. Everything they make lives in `..\_work\` (ignored
by git). Step by step: [`docs\server-setup.md`](../docs/server-setup.md).

| Script | Purpose |
|---|---|
| `ga-env.ps1` | Shared paths, ports, database names, toolchain locations, MySQL helpers (dot-sourced by the others) |
| `pins.json` | The pinned upstream core commit and its patches, mod-playerbots and the third-party modules |
| `setup-server.ps1` | Clones the AzerothCore Playerbot branch at the pinned commit and applies `patches\azerothcore`, clones mod-playerbots and the modules at their pins, links `modules\mod-guild-adventure` into the core |
| `apply-playerbots-patches.ps1` | Applies `patches\mod-playerbots` to the mod-playerbots checkout (`setup-server.ps1` runs it) |
| `build-server.ps1` | CMake configure and MSVC build (`RelWithDebInfo`) into `_work\build`, install to `_work\server`; `-Tools maps-only` builds the data extractors too |
| `setup-db.ps1` | Creates the empty databases `GA_auth`, `GA_world`, `GA_characters`, `GA_playerbots` |
| `configure-server.ps1` | Writes `authserver.conf`, `worldserver.conf`, `playerbots.conf`, `mod_guild_adventure.conf` and the module configs from the templates; points realm 1 at the world port |
| `start-server.ps1` | Starts MySQL if needed, then the authserver and the worldserver in their own windows |
| `ga-command.ps1` | Runs a GM console command through SOAP (localhost only), e.g. `"server shutdown 5"` |
| `create-account.ps1` | Creates a game account directly in `GA_auth` (SRP6, optional GM level) |
| `setup-client.ps1` | Copies your 3.3.5a client into `_work\client` (`-Source <client folder>`, the original is not changed), sets its realmlist to this server and links the addon into it |
| `test-gadv-codec.ps1` | Builds and runs the protocol codec test (`modules\mod-guild-adventure\tests`) |
| `gen_milestones.py` | Generates `ga_milestone_template.sql` (the tiered guild achievements) |
| `gen_road_links.py`, `navpath\` | Reads the server's navmesh (`mmaps`) and generates `ga_road_link.sql`: walks the travel node graph of mod-playerbots lacks |
| `analysis\` | Reads the module's activity log (`GuildAdventure.log`): bot activity, dungeon fights and moves |

## Load tests and profiling

`loadtest\` loads a throwaway server instance with hundreds of test guild bots and takes its ticks apart
(`setup-instance.ps1`, `load.ps1`, `profile.ps1`, `analyze_profile.py`, `restart-with.ps1`, `teardown-instance.ps1`).
Recipe and how to read the reports: [`loadtest\README.md`](loadtest/README.md); results:
[`docs\performance.md`](../docs/performance.md).
