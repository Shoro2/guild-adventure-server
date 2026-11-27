# Guild Adventure

A guild management game on a World of Warcraft 3.3.5a (build 12340) private server. You play the guild leader. Up
to 39 guild members are bots ([mod-playerbots](https://github.com/mod-playerbots/mod-playerbots)) that you manage:
where they level, which professions they learn, their talents, their groups and dungeon runs. At any time you can take
over a bot and play it yourself. The guild starts with three bots and unlocks more as it grows; it keeps playing for a
while after you log out.

> **Disclaimer.** Guild Adventure is a fan project. It is not affiliated with, endorsed by or sponsored by Blizzard
> Entertainment. World of Warcraft and Blizzard Entertainment are trademarks or registered trademarks of Blizzard
> Entertainment, Inc. This repository contains no Blizzard game files. You need your own legally obtained World of
> Warcraft 3.3.5a (build 12340) client; extract the server data (maps, vmaps, mmaps, DBC) from it yourself. For private
> servers only. Do not use any part of this project with official Blizzard services.

For a single-player installation on one Windows PC there is a separate repository with a one-click installer:
`guild-adventure-local`.

## Contents

| Path | What it is |
|---|---|
| `modules/mod-guild-adventure` | The AzerothCore module (C++): guild, roster, orders, the order planner, storage, dungeon runs, the addon protocol |
| `addon/GuildAdventure` | The client addon (Interface 30300): the management UI |
| `patches/azerothcore` | One patch file against `mod-playerbots/azerothcore-wotlk`, branch `Playerbot`, at the commit in `tools/pins.json` (what each part does: `docs/core-patches.md`) |
| `patches/mod-playerbots` | Patches for the pinned `mod-playerbots` commit |
| `tools` | Windows PowerShell scripts that set up, build, configure and start a server; generators and analysis scripts |
| `guide` | The player guide: every feature explained from the player's side |
| `docs` | The technical documentation: how each system works inside, setup, core patches, the addon protocol |

Not included: any client, client file or server data, and the client-side tools of the private development setup
(a client DLL, a client patcher, an automated client test harness). Without them a bot is taken over through the
character screen, where you select the bot and enter the world.

## Documentation

The documentation has two parts:

- **[Player guide](guide/README.md)**: how to play, every tab of the window, orders, taking control, progression,
  storage and gold, professions, talents and fight profiles, dungeons and raids, bot behaviour.
- **[Technical documentation](docs/README.md)**: how the server works inside, setting up a server, the core patches,
  the addon protocol, performance.

## State

Alpha. Developed and tested by one player on one server; the server is built for several players, each with their
own guild, but that is untested. Classic content up to level 60.

## Setting up a server

Windows: [docs/server-setup.md](docs/server-setup.md). Elsewhere the module is an ordinary AzerothCore module: apply
`patches/azerothcore/0001-guild-adventure-core.patch` to the pinned core commit, add `mod-playerbots` (with the patches
in `patches/mod-playerbots`), the modules listed in `tools/pins.json` and `modules/mod-guild-adventure` under the
core's `modules/` folder, and build as AzerothCore documents.

## License

AGPL-3.0-or-later, see [LICENSE](LICENSE). The patch files in `patches/` are under the license of the code they change
(GPL-2.0-or-later). Third-party components and sources: [NOTICE.md](NOTICE.md).
