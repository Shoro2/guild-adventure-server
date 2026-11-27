# Core patches

Every change Guild Adventure makes to the AzerothCore core. The core lives in its own private
repo, `Shoro2/azerothcore-ga`, based on `mod-playerbots/azerothcore-wotlk@Playerbot`
(the design log §3.1, decision D13). Rules (the design log §9):

- A core patch exists only where a module hook cannot do the job.
- One topic per commit, Conventional Commit title ending in `(GuildAdventure CP-n)`.
- Every changed spot carries a `// GuildAdventure:` comment, so `git grep "GuildAdventure"`
  in the core lists them all.
- This file lists every patch: file, reason, the module code that uses it, upstream status.

Design background: [take-control.md](take-control.md) §7 (the proposal these patches come from).

## Overview

| Id | State | Topic | Core commit | Used by (module) |
|---|---|---|---|---|
| CP-1 | ✅ merged, T2 (swaps, logout while controlling a bot) | Logout-detach session hook | `0ebd095` | `ControlSwapService::OnLogoutDetach` |
| CP-2 | ✅ merged, T1 (GCC + MSVC; runs on the integration server, not tested on its own) | Instance lock times saved under the character's own account | `5e2f56f` | — (core-internal fix) |
| CP-3 | ✅ merged, T2 (Path R: the target shows on the owner's character screen) | Character-list session hook | `dfb47d0` | `ControlSwapService::OnCharEnum` |
| CP-4 | ✅ merged, T1 (GCC + MSVC, reported) | `SendInitialPacketsAfterAddToMap(bool loginEffect = true)` | `6e62df6` | — (for the Path S resync, not written yet) |
| CP-5 | ✅ merged, T2 (client channel, PLAYER_RECORD) | Two custom opcodes, `0x521` (client → server) and `0x522` (server → client) | `17cba41` | `ClientChannel` (take-control.md §5.10) |
| CP-6 | ⏳ not needed so far | Socket-close hook | — | — (the module polls the core's offline sessions each tick and hands a controlled bot back at once, take-control.md §5.7) |
| CP-7 | ✅ merged into `ga/main` (`c50488d`), T2 (`storage` dev instance and the integration server) | Reagents from outside the bags: `OnPlayerCheckReagent`, `OnPlayerTakeReagent` | `dfc48d88a` | `StorageService::OnCheckReagent` / `OnTakeReagent`: crafting from the guild storage (storage.md §5) |
| CP-8 | 🚧 in the combined core PR `Shoro2/azerothcore-ga#5` (CP-8 + CP-9 + CP-10, supersedes #3 and #4); on the integration server since 2026-11-12, T2 | Player hooks for the guild perks: `OnPlayerUpdateSpeed`, `OnPlayerSpellCastTime`, `OnPlayerCheckWaterBreathing`, `OnPlayerFallDamage`; macro `GUILDADVENTURE_CP8_PERK_HOOKS` | `ee055f5ce` | `PerkService` (guild-management.md "Guild perks"): movement speed, crafting and gathering speed, breathing under water, no fall damage |
| CP-9 | 🚧 in the combined core PR `Shoro2/azerothcore-ga#5` (CP-8 + CP-9 + CP-10, supersedes #3 and #4); on the integration server since 2026-11-12, T2 | Quest eligibility hook `OnPlayerCanTakeQuest` at the end of `Player::CanTakeQuest` | `d87634132` | `QuestSupport::MayTake`: AI-played characters do not take quests the order planner cannot finish (orders.md "Quests the bots leave alone") |
| CP-10 | 🚧 in the combined core PR `Shoro2/azerothcore-ga#5` (CP-8 + CP-9 + CP-10, supersedes #3 and #4); on the integration server since 2026-11-12, T2 | Point movement hook `OnPlayerCanStartPointMovement` in `PointMovementGenerator` | `72e3392a5` | `GroundGuard::MayMove`: AI-played characters do not start a movement that leaves the ground (orders.md "Paths through the ground") |
| CP-11 | 🚧 branch `claude/perf-cp11-cp12-v818j3` of `Shoro2/azerothcore-ga` (2026-11-21, performance plan PF-5), T2 on the `pf` load test instance | A character without a client is updated at a phase of its own every `Bots.UpdateInterval` ms (default 75) | `e9b1cb5dd`, reworked in `8575b417a` | — (performance, docs/performance.md §10) |
| CP-12 | 🚧 same branch (PF-7), measured on the `pf` instance: off by default (it made the ticks slower) | A smaller grid activation range for a character without a client on a continent (`Bots.GridActivationRange`, default 0 = off) | `bdb6f8e19`, default off in `0fd0b6a82` | — (performance, docs/performance.md §10) |
| CP-13 | 🚧 branch `claude/core-chase-guard-lnm8xu` of `Shoro2/azerothcore-ga` (2026-11-21, on `0fd0b6a82`), T1 (MSVC, the gaps instance runs on it) | A player's chase asks the point movement hook of CP-10 before its spline | `23297a835` | `GroundGuard::MayMove`: the chase of a character the AI plays does not go through the ground |

All five written patches were written on branch `claude/take-control-od9n2y` of
`Shoro2/azerothcore-ga` on top of `ga/main` = `e7e2e288e` (upstream `Playerbot` of 2026-11-05).
**the owner merged them into `ga/main` on 2026-11-10** (Shoro2/azerothcore-ga#1, merge commit
`b495f91`); `ga/main` now has the same tree as `17cba41`. Evidence: the patched
translation units compile with GCC 13 (`-Wall -Wextra`, no warnings) in a Linux cloud build.
With CP-1 to CP-3 the whole server builds, links and installs with MSVC 2022 on the operator
box. With all five patches (`17cba41`) it does too, according to the M2 thread: its integration
build `_work\build` → `_work\server` (module PR #5 rebased onto `75bd0f9`) built with 0 errors
and 0 warnings, and the worldserver starts on it. In-world (T2) since 2026-11-09/10: Path R
swaps and logout while controlling a bot (CP-1, CP-3, the owner at the client and the probe), the
client channel and PLAYER_RECORD (CP-5), take-control.md §5.7-§5.10 and §11.

The diff against `ga/main`: 11 files, +121 / -6 lines.

**A module build needs the core at the same state.** The module uses the hooks of CP-1 and
CP-3 and, since CP-5, the opcode names `CMSG_GA_CLIENT_MESSAGE` / `SMSG_GA_SERVER_MESSAGE`:
against a core without CP-5 it no longer compiles. Update the core checkout of a build tree
(`17cba41` or later, e.g. `ga/main` from `b495f91` on) before building module commits that
contain `ClientChannel`.

Path W (take-control.md §5.8, added 2026-11-09) needs no further patch: it uses the stock far
teleport (`Player::TeleportTo` with `newInstance = true`, `WorldSession::HandleMoveWorldportAck`)
and the stock `ServerScript::CanPacketReceive` hook (for `CMSG_UPDATE_ACCOUNT_DATA`), plus CP-1
for a logout during its loading screen.

## CP-1 — logout-detach session hook

| | |
|---|---|
| Files | `src/server/game/Server/WorldSession.cpp` (`LogoutPlayer`, right after the pending far teleport is finished); `Scripting/ScriptDefines/SessionScript.h/.cpp` (hook `SESSIONHOOK_ON_LOGOUT_DETACH`, virtual `OnSessionLogoutDetach`); `Scripting/ScriptMgr.h` |
| Hook | `[[nodiscard]] virtual bool SessionScript::OnSessionLogoutDetach(WorldSession* session, Player* player)`; default `false`; the first script that returns `true` wins (`CALL_ENABLED_BOOLEAN_HOOKS_WITH_DEFAULT_FALSE`) |
| Core behaviour when a script returns `true` and has moved the player to another session | Send `SMSG_LOGOUT_COMPLETE`, reset `m_playerLogout`, `m_playerSave`, set `m_playerRecentlyLogout`, clear the logout timer, return. The player is **not** saved, removed from the map or deleted. |
| Safety net | If a script returns `true` but the player is still attached, the core logs an error and logs the player out normally. Not called while redirecting (ToCloud9). |
| Why a core patch | `LogoutPlayer` is the single exit of a player from a client session (logout request, kick, disconnect after the 60 s tolerance, session deletion). No existing hook can stop it from removing the player from the world, and `m_playerRecentlyLogout` (needed so delayed packets of the old character are dropped silently) has no public setter. |
| Module use | Path R step 2 (the swap's own logout), logout and disconnect while the owner controls a guild bot (the bot goes back to its AI, D12), the owner's offline session being reclaimed. Returns `false` for headless sessions, during shutdown (`World::IsStopped()`) and for every character the module does not manage. |
| Upstream | Not proposed. Generic enough to offer upstream later ("let a module keep a player in the world on logout"). |

## CP-2 — instance lock times under the character's account

| | |
|---|---|
| Files | `src/server/game/Entities/Player/Player.h` (`GetCharacterAccountId()`, member `m_characterAccountId`); `Player.cpp` (constructor, `Create`, `GetCharacterAccountId`, `_SaveInstanceTimeRestrictions`); `PlayerStorage.cpp` (`LoadFromDB` stores `characters.account`) |
| Change | The player remembers the account it belongs to (`characters.account`, already read by `LoadFromDB` for its wrong-account check; for a new character the creating session's account). `_SaveInstanceTimeRestrictions` deletes and writes `account_instance_times` for that account instead of `GetSession()->GetAccountId()`. `GetCharacterAccountId()` falls back to the session's account if it was never set. |
| Why | While the owner plays a guild bot, the bot sits on the owner's client session. Without the patch, saving the bot would replace the owner's "instances per hour" list with the bot's and the other way round (take-control.md §6.4). |
| Module use | `GuildBotHolder` and `ControlSwapService` use `GetCharacterAccountId()` to tell the leader (owner's account) from bots (bot accounts). |
| Behaviour without Guild Adventure | Unchanged: a character always plays on its own account, so both ids are equal. |
| Upstream | Not proposed. |

## CP-3 — character-list session hook

| | |
|---|---|
| Files | `src/server/game/Handlers/CharacterHandler.cpp` (`HandleCharEnum`, before the count is written); `Scripting/ScriptDefines/SessionScript.h/.cpp` (hook `SESSIONHOOK_ON_CHAR_ENUM`, virtual `OnSessionCharEnum`); `Scripting/ScriptMgr.h` |
| Hook | `virtual void SessionScript::OnSessionCharEnum(WorldSession* session, WorldPacket& data, uint8& characterCount)`; a script may append rows built with `Player::BuildEnumData` and must increase `characterCount` for each |
| Why a core patch | `ServerScript::CanPacketSend` gets the outgoing packet as `WorldPacket const&`, so no module can add a row to `SMSG_CHAR_ENUM`. |
| Module use | Path R step 3: while a swap waits at the character screen, the target bot (on a bot account) is appended to the owner's list. At most 10 rows in total (client limit). |
| Difference to the proposal | The proposal also marked the extra row as a "legitimate login" (`_legitCharacters`). Not needed: the module intercepts `CMSG_PLAYER_LOGIN` for guild characters in `CanPacketReceive` before the core's `IsLegitCharacterForAccount` check runs, so the core never sees that login. |
| Upstream | Not proposed. |

## CP-4 — optional login sparkle

| | |
|---|---|
| Files | `src/server/game/Entities/Player/Player.h` (declaration with the default), `Player.cpp` (`SendInitialPacketsAfterAddToMap`) |
| Change | New parameter `bool loginEffect = true`; the cast of spell 836 (`LOGINEFFECT`) runs only when it is `true`. Every existing caller keeps the default, so nothing changes without a caller that passes `false`. |
| Why a core patch | The sparkle is cast inside the function, and the Path S resync (take-control.md §5.3 step 7) has to call the function for a character that did not log in. |
| Module use | None yet: the Path S resync is not written. Path W's sparkle (take-control.md §5.8) does not go away with CP-4 alone: it comes from `WorldSession::HandleMoveWorldportAck` (`Handlers/MovementHandler.cpp:215`), which keeps the default. Removing it there would need one more small change in that handler (for example a per-player "skip the next login effect" flag); not planned while Path W is an experiment. |
| Upstream | Not proposed. |

## CP-5 — custom opcodes for the take-control client DLL

| | |
|---|---|
| Files | `src/server/game/Server/Protocol/Opcodes.h` (two enum values, `NUM_MSG_TYPES` `0x521` → `0x523`), `Opcodes.cpp` (two table entries), `Server/WorldSession.h/.cpp` (`HandleGuildAdventureClientMessage`) |
| Opcodes | `CMSG_GA_CLIENT_MESSAGE = 0x521`: `STATUS_AUTHED`, `PROCESS_THREADUNSAFE`. `SMSG_GA_SERVER_MESSAGE = 0x522`: server only (`STATUS_NEVER` as a client opcode). `0x51F`/`0x520` stay ToCloud9's. |
| Payload | Defined by the module: a `uint8` message type and its fields (take-control.md §5.10). |
| Why `STATUS_AUTHED` | The channel must work at the character screen, before any character is in the world; `STATUS_AUTHED` packets are also accepted while a character is in the world. The core calls `ServerScript::CanPacketReceive` for them (`WorldSession.cpp:506`). |
| Why `PROCESS_THREADUNSAFE` | The packet is handled in the world thread's pass over the session (`WorldSessionFilter`), where all of the module's swap state lives (take-control.md invariant I5). |
| Fallback handler | The module consumes every message in `CanPacketReceive` (returns `false`), so `HandleGuildAdventureClientMessage` only runs while the module is disabled or absent: it logs at debug level and discards the payload. `Handle_NULL` would log an error for each message. |
| Difference to the proposal | The proposal had four named opcodes (`SMSG_GA_ACTIVE_PLAYER_SWITCH`, `CMSG_GA_ACTIVE_PLAYER_SWITCH_ACK`, `CMSG_GA_CLIENT_HELLO`, `CMSG_GA_WORLD_LOADED`). Two generic ones with a type byte keep every message that P2 adds (switch, ack, world ready, telemetry) out of the core. |
| Why a core patch | The opcode table and `NUM_OPCODE_HANDLERS` are fixed at compile time. A core without the patch closes the connection on an opcode at or above `NUM_OPCODE_HANDLERS` (`WorldSocket::ReadHeaderHandler`, `WorldSocket.cpp:320-326`) and rejects an opcode without a table entry (`ReadDataHandler`, `:478-483`). |
| Client side | The 3.3.5a client's handler table has `NUM_MSG_TYPES = 0x51F` slots and drops higher ids quietly. A stock client therefore ignores `0x522`; the DLL has to take it in a hook of the packet loop (take-control.md §3.4, §5.10). That is also why the server speaks first (ANNOUNCE): a DLL that only answers never sends `0x521` to a core without CP-5. |
| Module use | `ClientChannel`: ANNOUNCE, HELLO / HELLO_ACK, PING / PONG; `.ga client`, `.ga client ping`. |
| Upstream | Not proposed. |

## CP-7 — reagents from outside the bags

| | |
|---|---|
| Files | `src/server/game/Spells/Spell.cpp` (`Spell::CheckItems`, `Spell::TakeReagents`, `Spell::WriteCastResultInfo`), `Scripting/ScriptDefines/PlayerScript.h/.cpp` (two hooks, `PLAYERHOOK_ON_CHECK_REAGENT`, `PLAYERHOOK_ON_TAKE_REAGENT`), `Scripting/ScriptMgr.h` |
| Hooks | `OnPlayerCheckReagent(Player*, uint32 itemId, uint32 count, bool& enough)`: called when the bags hold fewer of a reagent than the spell needs (the reagent check, and the missing item `SPELL_FAILED_REAGENTS` names); a script sets `enough` when its own store makes up the rest. `OnPlayerTakeReagent(Player*, uint32 itemId, uint32& count)`: called in `TakeReagents` right before the reagent is destroyed in the bags, when they hold fewer than the spell takes; a script takes the missing part from its store and lowers `count` by it. |
| Why | The owner (2026-11-12): crafting must work straight out of the guild storage, for the bots and the leader. The reagent check and the consumption are inside `Spell`; no module hook reaches them. Without the patch the module could only consume the materials itself and cast the spell as triggered (what ForgottenLand's mod-endless-storage does), which skips the spell's normal checks, or move the materials into the bags first (needs bag space, leaves leftovers on a failed cast). With it every server-side cast of a guild character, a bot's included, takes what its bags lack from the storage and is otherwise a normal cast. |
| Behaviour without a script | Unchanged: the hooks are only called on the paths that would fail or destroy anyway, and leave `enough` / `count` as they are. |
| Client side | The client checks its own bags before it sends a tradeskill cast; the addon's "Create (storage)" button lets the server start such a cast (storage.md §5). |
| Module use | `StorageService::OnCheckReagent` / `OnTakeReagent` (guild characters, `GuildAdventure.Storage.Reagents`). |
| Upstream | Not proposed (ForgottenLand's fork documents hooks of the same names, `OnPlayerCheckReagent` / `OnPlayerConsumeReagent`, that were never wired in). |
| Diff | 4 files, +60 / -1 lines. |

## CP-8 — player hooks for the guild perks

| | |
|---|---|
| Files | `src/server/game/Entities/Unit/Unit.cpp` (`Unit::UpdateSpeed`), `Spells/SpellInfo.cpp` (`SpellInfo::CalcCastTime`), `Entities/Player/Player.cpp` (`Player::getMaxTimer`, `Player::HandleFall`), `Scripting/ScriptDefines/PlayerScript.h/.cpp` (four hooks, `PLAYERHOOK_ON_UPDATE_SPEED`, `_ON_SPELL_CAST_TIME`, `_ON_CHECK_WATER_BREATHING`, `_ON_FALL_DAMAGE`, the macro `GUILDADVENTURE_CP8_PERK_HOOKS`), `Scripting/ScriptMgr.h` |
| Hooks | `OnPlayerUpdateSpeed(Player*, UnitMoveType, float& rate)`: before `Unit::UpdateSpeed` sets a player's speed rate, after every aura. `OnPlayerSpellCastTime(Player*, SpellInfo const*, int32& castTime)`: in `SpellInfo::CalcCastTime` after the core's own modifiers (haste, spell mods), for a player caster. `OnPlayerCheckWaterBreathing(Player*, bool& breathes)`: when the breath timer starts (`getMaxTimer(BREATH_TIMER)`); `true` acts as a water breathing aura. `OnPlayerFallDamage(Player*, uint32& damage)`: in `HandleFall` with the damage before it is dealt. |
| Why | The owner (2026-11-12): guild perks for every guild member (movement speed, crafting and gathering speed, unlimited breath, no fall damage). The core takes these four only from auras; a module could add a real spell's aura and rewrite its amount, but that shows a buff, ends on death or dispel and stacks by the spell's rules. The hooks give a plain, permanent effect. Everything else the perks do needs no patch (module hooks for XP, gold, loot, drops, kills; the no-reagent update fields; teleports for instant flights). |
| Behaviour without a script | Unchanged: each hook passes its value through. |
| Module use | `PerkService` through `GuildAdventurePerkScripts.cpp`; built only when the macro is there, so the module still builds against a core without CP-8 (those four perks are then not offered). |
| Upstream | Not proposed. |
| Diff | 6 files, +94 lines. |

## CP-9 — quest eligibility hook

| | |
|---|---|
| Change | `PlayerScript::OnPlayerCanTakeQuest(Player*, Quest const*)` (hook id `PLAYERHOOK_ON_CAN_TAKE_QUEST`), called at the end of `Player::CanTakeQuest` after every check of the core passed; a script returns false to refuse the quest for that player |
| Files | `Scripting/ScriptDefines/PlayerScript.h`, `PlayerScript.cpp`, `Scripting/ScriptMgr.h`, `Entities/Player/PlayerQuest.cpp` |
| Why | mod-playerbots' NewRpg accepts every quest a giver offers that passes `bot->CanTakeQuest`. The module cannot keep it from taking quests the order planner cannot finish, and abandoning them afterwards makes NewRpg take them again at the same giver. No existing hook sits in the eligibility check (`disables` and `conditions` are global, they would also bind the owner's own play) |
| Module use | `QuestSupport::MayTake` (docs/orders.md "Quests the bots leave alone"): refuses listed, scripted-event and learned quests for characters the AI plays |
| Notes | `Player::CanTakeQuest` runs often and on map threads (quest giver status, quest menus, bot AI): a script must answer fast and guard what it reads. With no script registered for the hook the cost is one empty-vector check |
| Upstream | not offered; small and generic enough to offer later |

## CP-10 — point movement hook

| | |
|---|---|
| Change | `PlayerScript::OnPlayerCanStartPointMovement(Player*, PathGenerator const& path, bool usesPath, float x, float y, float z)` (hook id `PLAYERHOOK_ON_CAN_START_POINT_MOVEMENT`), called in `PointMovementGenerator<T>::DoInitialize` after the path was generated, and in `DoUpdate` when the start had been put off (the unit was rooted or casting). `usesPath` says whether the movement follows the path's points or falls back to `MoveSplineInit::MoveTo`. A script returns false to cancel the movement: nothing is launched and the generator ends |
| Files | `Scripting/ScriptDefines/PlayerScript.h`, `PlayerScript.cpp`, `Scripting/ScriptMgr.h`, `Movement/MovementGenerators/PointMovementGenerator.cpp` |
| Why | The path finder's result is not always a walkable path: for a destination it cannot reach from the start polygon it returns a two-point path that ends at the destination itself, and the point movement runs it as a straight spline, through a hill into the mine underneath. mod-playerbots moves bots with `MotionMaster::MovePoint`; a module has no other place to look at the path before the spline starts |
| Module use | `GroundGuard::MayMove` (docs/orders.md "Paths through the ground"): probes the movement for a floor under it and cancels it for characters the AI plays |
| Notes | Only for players (`unit->ToPlayer()`); creatures are not touched. A real player's client moves it, so the hook fires for server-driven movement only (bots; a charge or a scripted move of a real player goes through other generators). Cost with no script registered: one empty-vector check per point movement of a player. The module's check costs one height lookup per sampled yard: about 24 for a plain navmesh path (its two ends), the whole length for anything else. Not measured under load: the `gaps` instance ran three bots (update time 2 ms) |
| Upstream | not offered; the hook is generic, the two-point path behaviour itself may be worth an upstream issue |

## CP-13 — a player's chase asks the point movement hook

| | |
|---|---|
| Change | `ChaseMovementGenerator<Player>::DispatchSplineToPosition` calls `sScriptMgr->OnPlayerCanStartPointMovement(owner, *i_path, usesPath, end)` (the hook of CP-10) after the path was generated and cut, before the chase spline is launched; `usesPath` as in CP-10, `end` the path's last point. A script that returns false stops the chase movement there (`StopMoving`, return false), as a failed path does; the generator stays and tries again on its next update |
| Files | `Movement/MovementGenerators/TargetedMovementGenerator.cpp`, `Scripting/ScriptDefines/PlayerScript.h` (comment) |
| Why | A chase towards a target the path finder cannot reach (a creature that flies over a slope, or one spawned under the terrain: Putrid Gargoyle 69692 in the Eastern Plaguelands stands 18 yd under the hill) runs the two-point path as a straight spline. A druid in cat form chased it into the hill and was rescued from under the ground ten seconds later; nine such rescues in two days (round 147). The owner: rescues are a workaround, core changes are allowed |
| Module use | `GroundGuard::MayMove` judges the chase as it judges a point movement; the refusal line then says "(its chase in a fight)", and three refusals in 45 s in a fight make the planner give up what the bot cannot get to (`OrderService::UpdateBot`, the fight branch) |
| Notes | Only players; creatures, pets and the follow movement are untouched. The hook was named for point movements; the comment in `PlayerScript.h` says that the chase asks it too. Cost: the module's check is the CP-10 one (a short chase path stays below `MinDistance` and is not probed) |
| Upstream | not offered |

## CP-11 — characters without a client updated at a phase of their own

| | |
|---|---|
| Change | `Map::Update`: a player whose session has no open socket (`WorldSession::IsSocketClosed()`: a bot's headless session) is updated once every `Bots.UpdateInterval` ms (worldserver.conf, default 75; `CONFIG_BOTS_UPDATE_INTERVAL`; 0 = every world tick as before) at a phase taken from its guid (`Player::TakeBotUpdate`: a per-player phase counter that the tick's diff advances; the diffs of the ticks it skips are summed in `m_deferredUpdateDiff` and handed to its next `Player::Update`). The phases spread the bots of a map over the ticks, so a tick carries about `tick / interval` of them. Every other player is updated on every tick with `s_diff + TakeDeferredUpdateDiff()`. The session's packets (`WorldSession::Update`) are handled every tick as before |
| Files | `Maps/Map.cpp`, `Entities/Player/Player.h`, `World/WorldConfig.h`, `World/WorldConfig.cpp`, `apps/worldserver/worldserver.conf.dist` |
| Why | docs/performance.md F5: every bot ran the whole `Player::Update` (all auras, position, regeneration, the modules' hooks, mod-playerbots' AI) on every world tick. Under load a world tick is 20-40 ms, so a bot was updated 25-50 times a second, more often than creatures (once per rotation of the continent's full update) and more often than its AI thinks (`ReactDelay` 100 ms) |
| Module use | none; mod-playerbots' AI (`OnPlayerAfterUpdate`) and the module's per-character hooks (auto loot) run inside `Player::Update` and get the summed diff |
| First version | `e9b1cb5dd` updated every bot on its map's full step only (`Bots.UpdateOnFullStepOnly`): at 1600 bots that step then carried every bot's update and its AI together with the creatures, the world tick's p95 went from 45 to 90 ms while the other ticks fell to 13 ms (docs/performance.md §10). `8575b417a` replaced it with the phased interval; the old switch is gone |
| Notes | A character its owner plays (take control, watch mode) has the client's socket and is updated every tick. Timers that count by diff (auras, cooldowns, regeneration, swing timers) lose nothing; a melee swing or a cast that finishes between two updates happens at the next one, at most 75 ms late (mod-playerbots' AI looks every 100 ms anyway). A bot that becomes client-controlled gets its collected diff at its next update |
| Upstream | not offered (mod-playerbots' fork would be the place: it is about bots) |
## CP-12 — a smaller activation range for characters without a client

| | |
|---|---|
| Change | `WorldObject::GetGridActivationRange()`: for a player whose session has no open socket, on a map that is not instanceable (a continent), `Bots.GridActivationRange` (worldserver.conf, `CONFIG_BOTS_GRID_ACTIVATION_RANGE`, reloadable; default 0 = the map's range as before; the plan proposed 60 yd) when that is below the map's visibility range. Clients, dungeons and battlegrounds keep the map's range |
| Files | `Entities/Object/Object.cpp`, `World/WorldConfig.h`, `World/WorldConfig.cpp`, `apps/worldserver/worldserver.conf.dist` |
| Why | docs/performance.md F6: `Map::MarkNearbyCellsOf` keeps the cells within the activation range of every player awake (creatures, game objects updated), 100 yd on continents: every bot kept a client's surroundings alive. Creatures, their sight checks and visibility were 44 % of the map threads' CPU at 1600 bots; a visibility distance of 60 yd (the measured stand-in, §7 E1) cut them by a third |
| Module use | none |
| Notes | What a client sees is woken by the client's own character (its range is unchanged), so nothing a player sees stands still. Creatures 60-100 yd from a bot and from every client stand still until someone comes near: a patrol there is seen where it stands by the risk model (`Escape::RoamerNear`, the pull check), which is what a player would see too. Dungeons are left out on purpose: the dungeon conductor waits for patrols that cross its way |
| Measured | Off by default. At 1600 bots with 60 yd the world tick was slower in both comparisons of the load test (docs/performance.md §10): with CP-11 mean 21 -> 30 ms, p95 40 -> 57 ms; without it mean 25 -> 28 ms, p95 45 -> 50 ms. The cell marking keeps few creatures awake that would sleep otherwise (a creature a bot sees stays in the map's update list anyway, `Creature::IsUpdateNeeded`), while the creatures it lets fall out of the list are added again by the next sight of a bot. The stress test may try it again; it is a config line |
| Upstream | not offered |

## mod-playerbots patches

Kept as patch files in this repository (`patches/mod-playerbots/*.patch`, against the pinned commit of
`tools/pins.json`), applied by `tools/apply-playerbots-patches.ps1`, which `tools/setup-server.ps1` calls after it
checked out the pinned commit (D13, DQ5). A rebuild of the worldserver follows.

| Patch | What | Switch | Why |
|---|---|---|---|
| `0002-stat-weights-hook.patch` | `StatsWeightCalculator::SetBasicWeightsHook` (a static function pointer, nullptr by default) and its call in `GenerateWeights` after `GenerateBasicWeights`, before the talent extras and the fine-tune; the macro `PLAYERBOTS_STAT_WEIGHTS_HOOK` tells a module the hook is there | mod-guild-adventure's `GuildAdventure.StatWeights.Enable` (default 1); without a hook set nothing changes | The owner, 2026-11-26: the bots judge items by the stat weights of his wowbot335 research (docs/orders.md "Stat weights from wowbot335"); mod-playerbots picks and equips items itself, so its weights and the module's must be the same |
| `0001-skip-idle-triggers.patch` (PF-6) | `Trigger::IsIdle()` (virtual, default false); `ChatCommandTrigger` answers `!triggered`; `Engine::ProcessTriggers` skips an idle trigger before `needCheck`. The performance monitor's name (a `std::string` copy per trigger, action and value check) is built only while the monitor is on (`Engine.cpp`, `Value.cpp`). The triggers that fired in a pass are a small vector instead of a hash map built per pass | `Playerbots.SkipIdleTriggers` (playerbots.conf, default 1; the events are the same either way) | docs/performance.md F4: 97 % of all trigger checks were the 128 chat command triggers, checked five times a second per bot although no command was pending, each with a string copy for a monitor that is off |

## Settings the patches and the module rely on

| Setting | Value | Why |
|---|---|---|
| `Playerbots.BotAutologin` | `0` (default) | A `PlayerbotMgr` is created for every character the client controls; with autologin it would add every character of the session's account as an altbot (`PlayerbotMgr.cpp:1741-1763`). |
| `Playerbots.SelfBotLevel` | ≤ 2 (default 1) | Above 2, a new `PlayerbotMgr` turns its player into a self bot. |
| `Playerbots.KeepAltsInGroup` | `1` recommended | When a character is handed to the AI, `PlayerbotHolder::OnBotLogin` makes it leave a group that does not contain its master unless this is on (`PlayerbotMgr.cpp:529-563`). |
| `Warden.Enabled` | `0` | Needed for the Path S client DLL only. |

## Verification log

| Date | What | Result | Tier |
|---|---|---|---|
| 2026-11-12 | CP-7 on `ga/main` `b495f91` (branch `claude/project-thread-6nc5ok`): full MSVC 2022 `RelWithDebInfo` build of the `storage` development instance (`_work\build-storage`), then the module with the guild storage on top; worldserver started | 0 errors, 0 warnings; Linen Bandages crafted by a bot (`.ga admin craft`) and by the leader (the addon's CRAFT) with the Linen Cloth only in the guild storage | T1 + T2 (`storage` instance: SOAP; ga-probe `storage` 30/30, report `_work\ga-probe\reports\20261112_040258`) |
| 2026-11-12 | CP-8 on `ga/main` `c50488d` (branch `claude/project-thread-500exg`, core PR #3): full MSVC 2022 `RelWithDebInfo` build of the `perks` development instance (`_work\build-perks`), then the module with the guild perks on top; worldserver started | 0 errors, 0 warnings; `.ga admin perk check`: run speed rate 1.100 at +10 %, Linen Bandage 1500 → 1363 ms, Mining 3200 → 2666 ms, breathing under water, a fall of 1000 → 0 (the hooks through `sScriptMgr`); in the client the leader runs 7.7 instead of 7 yd/s right after buying | T1 + T2 (`perks` instance: SOAP; ga-probe `perks` 39/39, report `_work\ga-probe\reports\20261112_063738`); the `HandleFall` and breath call sites are built, not exercised by a real fall or dive |
| 2026-11-12 | CP-9 on `ga/main` `c50488d` (branch `claude/project-thread-lnm8xu`, `d87634132`): full MSVC 2022 `RelWithDebInfo` build of the `gaps` development instance (`_work\build-gaps`), then the module with `QuestSupport` on top; worldserver started | 0 errors, 0 warnings; a bot walking to a quest giver dropped the pickup ("no longer pickable") when the quest was added to `ga_quest_block_template` and `.ga reload` ran, and did not take the quest at the giver | T1 + T2 (`gaps` instance, SOAP, no client) |
| 2026-11-12 | CP-10 on top of CP-9 (branch `claude/project-thread-lnm8xu`, `72e3392a5`): MSVC 2022 `RelWithDebInfo` build of the `gaps` development instance with the module's `GroundGuard`; worldserver started | 0 errors, 0 warnings; a bot placed above a kobold of the Fargodeep Mine where the core only offers a straight line: with `GuildAdventure.Paths.StayOnGround = 1` it stays (movement cancelled), with 0 it sinks 20 yd through the rock to the kobold's spot | T1 + T2 (`gaps` instance, SOAP, no client) |
| 2026-11-09 | CP-1, CP-2, CP-3: patched translation units compiled (GCC 13.3, Ninja, `RelWithDebInfo`, `-Wall -Wextra`) in the cloud build tree | 0 errors, 0 warnings | T1 (compile only) |
| 2026-11-09 | Full `worldserver` build with GCC 13.3 (`Release`, `SCRIPTS=minimal-static`, `MODULES=static`) in the cloud build tree | Every object and static library of `worldserver` builds (0 compile errors). The final link fails on one symbol, `typeinfo for boss_kaelthas`, needed by mod-playerbots' `TKHelpers.cpp` from the Outland scripts that `minimal-static` leaves out; not related to the patches | T1 (compile only) |
| 2026-11-09 | Full build on the build PC in a separate tree: core worktree `_work\azerothcore-tc` (`5e2f56f`, junction to `_work\azerothcore-wotlk\modules\mod-playerbots` at `405f8119`), module worktree `_work\wt-take-control` (`202bdf9`), `cmake -G "Visual Studio 17 2022" -A x64 -DSCRIPTS=static -DMODULES=static -DTOOLS_BUILD=none -DAPPS_BUILD=all -DBUILD_TESTING=OFF`, `cmake --build _work\build-tc --config RelWithDebInfo`, `cmake --install` to `_work\server-tc` | Configure, build, link and install exit 0; 0 errors, 0 warnings (same warning level as the stock `_work\build`, `WITH_WARNINGS=OFF`); log `_work\logs\build-tc.log` | T1 (built and linked; not run) |
| 2026-11-09 | CP-4, CP-5: `Player.cpp`, `WorldSession.cpp` and `Opcodes.cpp` compiled (GCC 13.3, Ninja, `Release`, `-W -Wall -Wextra`) in the cloud build tree; `apps/codestyle/codestyle-cpp.py` passes; the module (all 8 translation units, including `ClientChannel.cpp`) compiles against them | 0 errors, 0 warnings | T1 (compile only; no MSVC build yet) |
| 2026-11-09 | Reported by the M2 thread (not run in this session): `_work\azerothcore-wotlk` fast-forwarded to `claude/take-control-od9n2y` = `17cba41` (CP-1 to CP-5), module = PR #5 branch `claude/m2-module-foundation-22fe87a7` at `2fc033e` (rebased onto `75bd0f9`, client channel included); `tools\build-server.ps1 -Reconfigure`, MSVC 2022 `RelWithDebInfo`, full rebuild 15.3 min, log `_work\logs\build-m2-9.log`; `_work\server` restarted on it | Configure, build and install exit 0, 0 errors, 0 warnings (`WITH_WARNINGS` off); "World Initialized In 0 Minutes 27 Seconds", "mod-guild-adventure: loaded (guild bot holder, take control via Path R)", `Errors.log` empty, no playerbots-settings warnings. No swap and no client-channel exchange run | T1 (built and started; by another session) |
| 2026-11-10 | In-world on the integration server `_work\server` (core `17cba41`, module PR #5 up to `05c4f8b`): Path R swaps both ways and logout while controlling a bot (CP-1, CP-3), client channel ANNOUNCE / HELLO / PING and PLAYER_RECORD (CP-5); the owner at the client on 2026-11-09, the unattended probe (ga-probe) on 2026-11-10 | All pass; `Errors.log` empty (take-control.md §5.7-§5.10, §11) | T2 |
| 2026-11-10 | The owner merged Shoro2/azerothcore-ga#1 into `ga/main` (merge commit `b495f91`, the five patches unchanged) | `ga/main` = the tree of `17cba41` | — |
