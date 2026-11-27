# Server setup (Windows)

The scripts in `tools\` set up, build and run a Guild Adventure server on one Windows PC. Everything they make lives in
`_work\` beside this repository (git-ignored); only the databases sit in a MySQL server. Paths and ports are set in
`tools\ga-env.ps1`, pinned versions in `tools\pins.json`.

## Requirements

| Item | Notes |
|---|---|
| Windows 10 or 11, x64 | PowerShell 5.1 (built in) |
| Git | on `PATH` |
| Visual Studio 2022 | workload "Desktop development with C++" |
| CMake 3.27 or newer | on `PATH` |
| Boost 1.81 (core needs at least 1.78) | default `C:\local\boost_1_81_0`, else set `GA_BOOST_ROOT` |
| OpenSSL 3 (Win64, full) | default `C:\OpenSSL-Win64`, else set `GA_OPENSSL_ROOT` |
| MySQL Server 8.4 | default `C:\Program Files\MySQL\MySQL Server 8.4` (headers and `libmysql` are needed for the build), service name `MySQL84`; else set `GA_MYSQL_ROOT` |
| A World of Warcraft 3.3.5a client, build 12340 | your own copy; the server data is extracted from it |
| Python 3 | only for the generators and analysis scripts |

## Steps

1. **Sources.** `.\tools\setup-server.ps1` clones `mod-playerbots/azerothcore-wotlk` (branch `Playerbot`) to
   `_work\azerothcore-wotlk`, checks out the pinned commit on a branch `guild-adventure` and applies
   `patches\azerothcore\0001-guild-adventure-core.patch`; it clones `mod-playerbots` and the extra modules of
   `tools\pins.json` at their pinned commits, applies `patches\mod-playerbots`, and links
   `modules\mod-guild-adventure` into the core.
2. **Database login.** Create `_work\mysql-client.cnf` (it is never committed):

   ```
   [client]
   host=127.0.0.1
   port=3306
   user=<a MySQL user that may create databases>
   password=<its password>
   ```

   Then `.\tools\setup-db.ps1` creates `GA_auth`, `GA_characters`, `GA_world` and `GA_playerbots`; the worldserver
   fills them on its first start.
3. **Build.** `.\tools\build-server.ps1 -Reconfigure -Tools maps-only` configures, builds and installs the server and
   the data extractors into `_work\server` (about 15 to 30 minutes). Later builds: `.\tools\build-server.ps1`.
4. **Server data from your client.** In PowerShell, with `<client>` your client folder:

   ```
   md _work\server\Data; cd _work\server\Data
   ..\map_extractor.exe -i "<client>"
   ..\vmap4_extractor.exe -d "<client>\Data"
   md vmaps; ..\vmap4_assembler.exe Buildings vmaps
   ..\mmaps_generator.exe
   Remove-Item -Recurse Buildings
   ```

   This writes `Cameras`, `dbc`, `maps`, `vmaps` and `mmaps` into `_work\server\Data` and leaves the client untouched.
   The mmaps step takes one to three hours.
5. **Configuration.** `.\tools\configure-server.ps1` writes `authserver.conf`, `worldserver.conf` and the module
   configs from their `.dist` files (`-Force` writes them again).
6. **Start.** `.\tools\start-server.ps1` starts the MySQL service if needed, then the authserver and the worldserver.
   The first start fills the databases and takes a few minutes.
7. **Account.** `.\tools\create-account.ps1 -Username <name> -GmLevel 0` (asks for the password). The first character
   you log in with on that account becomes the guild leader.
8. **Client.** Copy `addon\GuildAdventure` to `<client>\Interface\AddOns\GuildAdventure` and set
   `<client>\Data\<locale>\realmlist.wtf` to `set realmlist 127.0.0.1:3726` (or the server's address).

## Ports

| Item | Value |
|---|---|
| authserver | 3726 |
| worldserver | 8087 |
| SOAP (GM commands, `tools\ga-command.ps1`) | 127.0.0.1:7880 |
| Realm | id 1, "Guild Adventure" |

To host for other players, open 3726 and 8087 (TCP) in the firewall and set the realm's address in `GA_auth.realmlist`
to the server's public address. SOAP stays on 127.0.0.1.

## Optional: auction house buyer

`mod-ah-bot-plus` buys player auctions at fair prices; it never lists anything. It needs an account `GAAHBOT1` with a
character `Tradewind` that never enters the world (create the account with `create-account.ps1`, the character once
with a client). Run `configure-server.ps1 -Force` afterwards; without the character the buyer stays off.

## Updating

`.\tools\setup-server.ps1` again after a pull, then `build-server.ps1` (with `-Reconfigure` when source files were
added) and `configure-server.ps1`; a new `.conf.dist` key needs `-Force` or a hand edit of the `.conf`.
