# Guild management (M2 module foundation + M3 server side)

As-built notes of the guild management part of `mod-guild-adventure`: adventure guilds, the bot
account pool, recruitment, bots logging in with the owner, the offline window, renown and unlocks,
and the server side of the GADV addon protocol ([protocol.md](protocol.md), written by the addon
work). Take control (Path R) is a separate part: [take-control.md](take-control.md).

Status (2026-11-10): **T2 (ga-probe)** for founding, addon sync, recruitment, orders, bench, renown and the
offline window (see "T2 results" below); earlier **T1** — builds with MSVC 2022 into the integration server `_work\server`
(core `Shoro2/azerothcore-ga@claude/take-control-od9n2y`, CP-1..CP-3), the worldserver starts,
the module SQL is applied, the templates load, `.ga reload` answers over SOAP; the codec passes the
shared protocol vectors. Nothing of it has run with a client yet (T2 test below).

Guild-wide achievements (2026-11-12, "Flows" below): **T2 (ga-probe)** on the `ach` development instance
(`achievements` 23/23, report `_work\ga-probe\reports\20261112_022502`).
Tiered guild achievements (2026-11-12, "Flows" below): **T2 (ga-probe)** on the `msa` development instance
(`milestones` 22/22, report `_work\ga-probe\reports\20261112_182120`) and over SOAP there (counters, real bot kills, a
rewarded quest, a restart).

## Components

| File | Responsibility |
|---|---|
| `GuildAdventureMgr.*` | In-memory state + write-through persistence (characters DB), world templates (zones, unlocks, renown curve and sources), event log |
| `RosterService.*` | Founding the adventure guild and the WoW guild, bot account pool, recruitment, bench/activate, bots logging in with the owner, roster snapshots, guild membership of bots |
| `ProgressionService.*` | Renown, renown levels, unlocks (slots, offline window, feature keys), Death Knight unlock |
| `AchievementService.*` | Guild-wide achievements: the guild's list, renown per achievement, sharing with the members, the quiet-share packet filter |
| `OfflineService.*` | Offline window (bots keep playing, then log out) and the "while you were away" summary |
| `GuildEvents.*` | `RecordEvent` (log + away summary + push) and the push helpers |
| `Protocol/GadvCodec.*` | Wire format: escaping, message and chunk layer (no game dependencies) |
| `Protocol/GadvProtocol.*` | Links per owner, reassembly, rate limit, intents, sync, pushes, swap events |
| `GuildAdventureRosterScripts.cpp` | Player hooks (queue only) and the world script that drives the services |
| `GuildAdventureManageCommands.cpp` | `.ga` management commands |
| `Perks/PerkService.*` | Guild perks: templates, the guild's ranks and points, buying, the perk snapshot the hooks read, kill regeneration, instant flights, the no-reagent fields |
| `Perks/GuildAdventurePerkScripts.cpp` | The perk hooks (XP, gold, loot, drops, kills, logins; with core patch CP-8 speed, cast times, breath, falls), its world update, `.ga perks`, `.ga perk buy`, `.ga admin perk ...` |
| `Protocol/GadvPerks.cpp` | `PERKS` block and `PERK_BUY` (members of `GadvProtocol`) |
| `GuildAdventureAchievementScripts.cpp` | Achievement and login hooks (queue only), `CanPacketSend`, its world update, `.ga achievements`, `.ga admin achieve` |
| `MilestoneService.*`, `GuildAdventureMilestoneScripts.cpp`, `Protocol/GadvMilestones.cpp` | Tiered guild achievements: series and tiers (`ga_milestone_template`), counters, the scan, awards, the kill / quest / last-boss hooks (queue only), `.ga milestones`, `.ga admin milestone add / scan`, the `MILESTONES` block |
| `GuildAdventureSettings.*` | Config options of this part |

Threading: everything runs on the world thread in `WorldScript::OnUpdate`, after the guild bot
holder's update (registration order). Player hooks that fire on map threads (level-ups, quests,
deaths and kills of bots) only queue an entry under a mutex; so does the chat hook for GADV whispers.

## Data model

Characters DB (`data/sql/db-characters/base/ga_characters.sql`):

| Table | Content |
|---|---|
| `ga_guild` | one row per owner account: WoW guild id, leader, team, slots, renown total/level, offline window, logout time |
| `ga_guild_unlock` | owned unlocks per owner |
| `ga_bot_account` | The owner's pool of bot accounts (D11) |
| `ga_bot` | one row per bot: bot account, slot, state (active/benched), auto-advance, lifetime counters |
| `ga_bot_order` | the current order (`queue_pos` 0) with explicit AUTO-limit columns |
| `ga_bot_profession_plan` | planned primaries (slot 1/2 with target rank) and secondaries (slot 0) |
| `ga_event_log` | event feed; ids assigned by the module |
| `ga_guild_achievement` | the guild's achievements: id, the member that earned it first, when, the renown it gave |
| `ga_guild_milestone` | the tiers of the tiered guild achievements a guild reached: tier id, the member that reached it (0 = the guild), when, guild points and renown |
| `ga_guild_counter` | what the guild counted for its series: kills per creature type, quests, dungeon and raid runs (sums), highest level per class and race and skill per profession (maxima) |
| `ga_guild_perk` | the guild perks a guild bought: rank and the guild points it cost per perk; the row with perk id 0 holds the points a GM granted |

World DB (`data/sql/db-world/base/ga_world.sql`): `ga_zone_template` (57 zones 1-80 from the
addon's mock list, faction A/H/C, continent, allowed orders), `ga_unlock_template` (the example
curve of the design log §2.2), `ga_renown_level` (points per level: 500, 1250, 2500, 4500, 7000, 10000, 14000, 19000 since
2026-11-27, five times the old curve), `ga_renown_source` (LEVEL_UP 10,
QUEST_DONE 3, PROF_UP 5, ACHIEVEMENT 5 = the least a guild achievement gives). Both files are idempotent; the DB updater re-applies them when they
change. The zone list has no hub positions yet: travel targets arrive with M4.

## Flows

- **Founding:** the first time an account enters the world with one of its own characters, that
  character becomes the leader (D14) and the adventure guild is created with
  `GuildAdventure.StartSlots` slots and the start offline window; the level-1 unlocks are owned.
  A WoW guild is founded with the leader as guild master (`GuildAdventure.Guild.NameFormat`,
  default "<leader> Adventurers"), or the leader's own guild is linked if they lead one. Other
  characters of the owner's account only get a hint to log in with the leader.
- **Login with the owner:** when the leader enters the world, every active bot that is not in
  the world logs in through `GuildBotHolder` with the leader as master. Bots joined after the
  WoW guild existed are added to it; bots in the world are checked once per login.
- **Recruitment** (`RECRUIT`, `.ga bot recruit`): needs the `recruit` feature and a free slot;
  validates race/class (DBC + `playercreateinfo`), expansion, disabled masks, the leader's
  faction (D9), the Death Knight unlock (a member at level 55; the core's own one-per-account and level-55-on-the-
  account rules belong to the character screen and do not apply; a death knight begins in its side's capital with
  the guild's `DeathKnights.SkipStart`, docs/orders.md "Death knights"), the core's name rules and name
  uniqueness. The character is created like the core's character creation (random appearance
  from `CharSections.dbc`, first-login flag, no intro) on a bot account of the owner's pool —
  the module creates the next account (`<Prefix><owner:5><index:2>`, random password, not kept)
  when all accounts hold `GuildAdventure.BotAccount.MaxCharacters` bots. After the save commits,
  the bot gets its slot, order `AUTO` (D16), joins the WoW guild and logs in at once.
- **Bench / activate** (`BOT_STATE`, `.ga bot bench|activate`): benching logs an AI-driven bot
  out; a controlled or loading bot cannot be benched. Activating logs it in while the owner is online.
- **Dismissing a bot** (`BOT_DISMISS`, the bot detail's **Dismiss**, `.ga bot dismiss <name> confirm`,
  `.ga admin dismiss <bot> confirm`; `RosterService::Dismiss`; the owner, 2026-11-28: [quote left out]): the
  character is deleted for good and its slot is free. Before that, what the guild storage takes of it goes there:
  every item in its bags, its bank and its bags there, on its body and in its role store's dungeon set that is not
  soulbound and that the storage accepts at all (no quest items, nothing conjured, timed or enchanted; worn pieces
  are mended; ammunition is taken: `StorageService::DepositDismissed`), and its gold goes into the guild bank
  (`GuildBank::Deposit`; with no WoW guild it goes with the character). The storage's rows are written at once
  (`FlushNow`), then the bot is logged out (saved without what went to the guild), forgotten by the order planner,
  the squads (a squad it led is free), the dungeon roles and the bot settings, deleted with the core's own
  `Player::DeleteFromDB` (final delete: its mail goes back to the senders, it leaves the WoW guild, the realm's
  character count follows) and its rows of the module's tables are deleted (the event log keeps its lines). The
  owner gets `BOT_REMOVE`, `STATE` and an event `DISMISS` that says what went where; the activity log names each
  kind. Refused while the owner plays it (also in watch mode), while it logs in and while it is on a dungeon run (its
  own or its squad leader's). A bot that is not in the world (benched, or the owner offline) logs in for it first
  (`RosterService::UpdateDismissals` finishes it once it is in the world, gives up after 120 s). Renown, unlocks,
  achievements and perks belong to the guild and stay; the bot account stays in the owner's pool for the next recruit.
  The addon asks first and wants the bot's name typed in; the command wants the word `confirm`.
- **Orders** (`ORDER_SET`, `.ga bot order`): `AUTO`, `IDLE`, `FOLLOW`, `LEVEL_ZONE <zone>` are
  validated and stored (zone of the guild's faction or contested, order allowed in the zone; AUTO
  limits only with `auto_limits`). Since M4 the bots act on their order and the roster's `why` line says what
  they do ([orders.md](orders.md)).
- **Renown:** level-ups and completed quests of guild bots give renown (`ga_renown_source`; the
  leader's own level-ups and quests do not count), and so does every new guild achievement, the leader's included
  (below). Every gain is scaled by `GuildAdventure.Renown.Rate` (1.0; a GM's `.ga renown add` is not). New levels own
  their free unlocks (slots and offline
  window only ever go up; a higher GM value stays). Gold unlocks show as `available` once their renown level is
  reached and are bought with the Progression tab's *Buy* (`UNLOCK_BUY`, since 2026-11-14, below). At every start
  the renown level is recomputed from the guild's renown and the curve as it is now (a lower level takes no owned
  unlock away), and the slots are kept within `GuildAdventure.MaxSlots` (39: with its leader a guild is at most 40
  people, the owner 2026-11-27; a guild with more bots keeps them, it just recruits no more).
- **Buying unlocks** (2026-11-14, the owner: the remaining 10 slots for gold; `ProgressionService::BuyUnlock`): a gold
  unlock (`ga_unlock_template.gold_cost`: 10 slots 100g at renown 4, 15 slots 200g at 5, 25 slots 500g at 7, 39 slots
  1000g at 9; the owner doubled the costs on 2026-11-27, they were 50, 100, 250 and 500g) is bought with *Buy* on the Progression tab or `.ga admin unlock`. Renown first,
  then gold: a tier is `locked` until its renown level, `available` after. The gold comes from the WoW guild bank by
  default; the bot setting `Unlocks.PayFrom` (guild-wide) can make it the guild bank and then the leader's purse, or the
  leader's purse alone (the leader must be in the world for its purse to count). The purchase owns the unlock
  (`ga_guild_unlock`), raises the slots, writes an `UNLOCK` event ("Bought: 39 bot slots (1000g from the guild bank).")
  and an activity log line, and pushes `STATE` and `UNLOCKS`. A slot or offline tier no larger than what the guild has
  already is `covered` (renown gave the owner's guild 20 and 30 slots for free, so the paid 10, 15 and 25 would buy
  nothing): the addon shows it as included and the server refuses it (`COVERED`). Paid tiers are no prerequisites of
  the next tier (the design log section 7 UL1, R3).
- **Offline window:** when the owner's account has no session (logout, or a lost connection after
  the core's tolerance), the window starts; after `offline_window_min` minutes every AI-driven
  character of the owner logs out. The leader logs out with the owner (D12, take-control code).
  Logging in again ends the window; bots still in the world are re-attached (new master), the
  others log in fresh. A server restart ends every window (all bots are offline after a restart).
  The window does not end while a login of the owner's characters is still in flight: such a
  character would slip past the logout and play on with no window left to end it (a window of a
  few seconds, or an owner who leaves right after logging in); the end is tried again every second.
- **Event log:** `ga_event_log` rows older than `GuildAdventure.EventLog.KeepDays` (30, 0 = keep
  everything) are deleted at startup and every six hours; each guild's newest 30 entries, the feed
  the addon shows, always stay.
- **Guild achievements** (the owner, 2026-11-12; `AchievementService`): achievements are guild-wide. When a guild
  character (the leader or a bot) completes one, the guild has it: the first completion is recorded once per guild
  (`ga_guild_achievement`), gives renown — its achievement points times `GuildAdventure.Achievements.RenownPerPoint`
  (1), at least `ga_renown_source.ACHIEVEMENT` (5; Feats of Strength have no points) — and an `ACHIEVEMENT` event
  ("Probemage earned the achievement "Level 10" for the guild.", `pts` = the renown). With
  `GuildAdventure.Achievements.Share = 1` (default) every other guild character in the world gets it in its own
  achievement list at once, through the core's own `CompletedAchievement` (meta achievements, titles and reward mail
  follow as for a normal completion); characters that are offline, benched or not recruited yet get the guild's list
  when they next enter the world. Entering the world also works the other way: what a character already had (from
  before the feature, or earned while the module did not see it) joins the guild's list, with its renown and one
  "brought N achievements into the guild" event. The shares are quiet: `ServerScript::CanPacketSend` keeps the guild
  chat line and the "has earned" lines of each shared completion away from the clients, so the owner sees the earner's
  own guild line once, not one per member; a client that plays the member itself still gets its toast and its line (so the leader, logging in, sees a toast for each achievement the guild earned while it was away).
  Not shared: realm-first achievements (they count for the guild only), and characters in GM mode (the core refuses
  achievements there; they get them on their next login). Counters and hidden tracking achievements are ignored.
  `STATE^ach`, `achp`, `achr` carry the guild's count, achievement points and the renown they gave.
- **Guild perks** (the owner, 2026-11-12: [quote left out]; `PerkService`): the guild earns **guild points**, one per achievement point of each guild achievement
  (`STATE^achp`; a GM can grant more, `.ga admin perk grant`), and the owner spends them on **perks** that work for the
  leader and every guild bot, permanently: at once for the characters in the world, on login for the others. Buying does
  not touch renown, its level or its unlocks; the points are a balance of their own (earned + granted - spent). The perks
  and their costs live in `ga_perk_template` (world database, one cost and one value per rank, comma separated):

  | Id | Perk (key) | Effect per rank | Costs | Where it acts |
  |---|---|---|---|---|
  | 1 | Swift Feet (`move_speed`) | +5/10/15/20/25 % movement speed (running, swimming, flying) | 50/100/200/350/500 | CP-8 `OnPlayerUpdateSpeed` |
  | 2 | Fast Learners (`experience`) | +10/20/30/40/50 % experience | the same | `OnPlayerGiveXP` |
  | 3 | Deep Pockets (`gold`) | +5..25 % looted gold | the same | `OnPlayerBeforeLootMoney`, also auto loot |
  | 4 | Thorough Hands (`extra_material`) | 5..25 % chance of one more of a looted crafting material (trade goods) | the same | `OnPlayerLootItem`; the extra goes through the loot filter |
  | 5 | Second Wind (`kill_regen`) | after a kill, 1..5 % health and mana over 10 s, 10 s cooldown | the same | `OnPlayerCreatureKill`, the world update |
  | 6 | Reagent-Free (`no_reagents`) | class spells need no reagents (tradeskill recipes still do) | 150 | the `PLAYER_NO_REAGENT_COST_*` fields (all bits) |
  | 7 | Instant Flights (`instant_flight`) | a flight from a flight master lands at once (quest flights a spell started keep flying) | 400 | the world update teleports to the last node; the planner takes the landing as no jump |
  | 8 | Cartographers (`all_flight_paths`) | every flight path of the faction is known (bots fly with them too) | 250 | the taxi mask, on login and on buying |
  | 9 | Material Haulers (`materials_storage`) | crafting materials the owner's own character loots go into the guild storage (bots' loot does already) | 100 | `LootService`, only where no loot rule decides |
  | 10 | Lucky Finds (`drop_chance`) | +5..25 % drop chance of items up to rare (not quest items) | 50/100/200/350/500 | `OnItemRoll` |
  | 11 | Master Crafters (`craft_speed`) | +10..50 % crafting speed (tradeskill spells) | the same | CP-8 `OnPlayerSpellCastTime` |
  | 12 | Quick Hands (`gather_speed`) | +10..50 % mining, herb gathering and skinning speed | the same | CP-8 `OnPlayerSpellCastTime` |
  | 13 | Gills (`water_breathing`) | breathe under water | 100 | CP-8 `OnPlayerCheckWaterBreathing` |
  | 14 | Feather Light (`no_fall_damage`) | no fall damage | 100 | CP-8 `OnPlayerFallDamage` |

  "+x % speed" means x % faster: a cast time is divided by 1 + x/100. Without core patch CP-8 the module builds too and the
  five CP-8 perks are not offered (`avail = 0`). The addon's **Perks** tab shows the points, every perk with its rank, its
  effect now and next, and a Buy button (`PERK_BUY`); a `PERK` event reports each purchase.
- **Tiered guild achievements** (the owner, 2026-11-12: [quote left out]; `MilestoneService`): series that
  count one thing for the whole guild, each with tiers. A tier is reached once per guild (`ga_guild_milestone`), gives
  its points as guild points (perks, `STATE^msp`, part of `gp`) and points × `GuildAdventure.Achievements.RenownPerPoint`
  renown (`STATE^msr`), and a `MILESTONE` event. The last tier the level cap allows gives
  `GuildAdventure.Milestones.LastTierFactor` (2) times its points ([quote left out]). The series
  and tiers are data (`ga_milestone_template`, world database, generated by `tools/gen_milestones.py`, 309 tiers in 47
  series):

  | Series (kind) | Counts | Tiers | Points per tier |
  |---|---|---|---|
  | one per class (`level_class`, 10 classes) | the highest level a guild character of that class reached | level 10 / 20 / 30 / 40 / 50 / 60 (70 / 80 wait for the level cap) | 5 |
  | one per race (`level_race`, 10 races; the addon shows the guild's faction) | the same per race | the same | 5 |
  | one per profession (`skill`, 11 primary + Cooking, First Aid, Fishing) | the highest skill a guild character has in it | 75 / 150 / 225 / 300 (375 / 450 wait for 70 / 80) | 5 |
  | `storage_items` | items in the guild storage now | 100 / 1,000 / 10,000 / 100,000 / 1,000,000 | 10 |
  | `guild_gold` | the guild's gold now, in gold: the WoW guild bank plus the money of the leader and every roster bot (the ones offline with what they had when last seen) | the same | 10 |
  | one per creature type (`kills`: beasts, dragonkin, demons, elementals, giants, undead, humanoids, mechanicals) | creatures of that type the guild's characters got the kill credit for (pets, totems and players' guardians do not count, nor critters) | the same | 10 |
  | `quests` | quests completed by guild characters, the same quest once per character | 10 / 100 / 1,000 / 10,000 / 100,000 | 10 |
  | `dungeons` | dungeon runs: the last boss of a dungeon (`instance_encounters.lastEncounterDungeon`) killed with a guild character's credit, once per instance and dungeon | 5 / 25 / 50 / 100 / 500 | 10 |
  | `raids` | the same for raids (Onyxia, Molten Core, Blackwing Lair, Zul'Gurub, both Ahn'Qiraj, Naxxramas) | 1 / 5 / 10 / 25 / 50 | 20 |

  Counting: kills, quests and runs come from player hooks (`OnPlayerCreatureKillCredit`, `OnPlayerCompleteQuest`) that
  only queue; the world update counts them at once. Levels, skills, storage items and gold are read by a scan of the
  guild characters in the world every `GuildAdventure.Milestones.ScanSeconds` (30). Sums and maxima are kept in
  `ga_guild_counter` (written at every scan and at shutdown). At startup the levels, skills and money of every guild
  character, offline ones included, come from the characters database, and a guild without a quest count starts with
  the quests its characters completed before (`character_queststatus_rewarded`), so a guild gets at its first scan the
  tiers it had already reached, in one event line ("The guild reached 14 tiered achievements ..."). Later dungeon and
  raid orders report runs with `MilestoneService::AddRun`. The addon's **Achievements** tab lists every series with its
  progress to the next tier (filter: classes, races, professions, kills, guild; the tooltip shows every tier); the
  Overview and Perks tabs count the tiers in. Open points for the owner: the design log §7 "Open" M1-M7.
- **Away summary:** events during the window feed a summary (levels, renown, quests, deaths, gold
  earned, one line per bot that changed). It is sent in the sync after the next login until the
  addon sends `AWAY_ACK`. In memory only.

## Protocol, server side

Implemented per [protocol.md](protocol.md) v1: transport through
`PlayerScript::OnPlayerCanUseChat` (private chat, `LANG_ADDON`, receiver == sender, prefix
`GADV\t`; consumed, never delivered). After a Path W swap the client still whispers to the previous
character's name (`UnitName("player")` is stale, docs/take-control.md §5.8), so a `LANG_ADDON` whisper to one
of the sender account's own guild characters counts as the sender's own GADV traffic; addon whispers are
never delivered to guild bots; chunk reassembly (≤ 4 partial messages per link, 15 s
timeout); rate limit `GuildAdventure.Protocol.MaxMsgPerSecond` with at most one `RATE_LIMIT`
error per second; HELLO → sync (`WELCOME`, `STATE`, `CONTROL_STATE`, `ZONES` if `zv` differs,
`UNLOCKS`, `ROSTER`, `EVENTS`, `AWAY`); intents `GET`, `ORDER_SET`, `BOT_STATE`, `PROF_SET`
(needs `professions`), `RECRUIT`, `CONTROL_TAKE`, `CONTROL_RETURN`, `UNLOCK_BUY`, `AWAY_ACK`;
pushes `EVENT`, `STATE`, `UNLOCKS`, `BOT` deltas every 2 s (against the values last sent),
`BOT_REMOVE`, `NOTICE`.

Swaps: `CONTROL_TAKE` / `CONTROL_RETURN` call `ControlSwapService::RequestSwap`; a refusal is
`ERROR^code=SWAP_REFUSED^reason=<token>` (benched bots: `BENCHED`, foreign GUIDs: `NOT_OWNER`). On
acceptance the server pushes `CONTROL_STATE^busy=1` and the `ACK`; on the `LeavingWorld` event it
sends `CONTROL_PENDING^g^cn`; the result is the new character's `HELLO` sync. A failed swap pushes
`CONTROL_STATE^busy=0` and an `INFO` event; `Completed` records a `CONTROL` event.

Not sent yet (fields exist, values pending later milestones): `role` (`none`), `spec` (empty). `xph`/`gph` come
from the order planner since M4 (0 during a bot's first five minutes in the world). `sk` (known professions) is
sent while the bot is in the world.

## Commands (GM, not from the console unless noted)

| Command | Effect |
|---|---|
| `.ga info` | your adventure guild: leader, renown, slots, offline window, features, bot accounts |
| `.ga roster [player]` | the bots of your guild, or of the player's guild |
| `.ga renown add <points>` | add or remove renown (levels and free unlocks follow) |
| `.ga slots set <n>` / `.ga offline set <minutes>` | override slots / offline window |
| `.ga unlocks [leader]` | every unlock of your guild (console / SOAP: of the leader's guild) with its renown level, state (`owned`, `available`, `locked`, `covered`) and cost, then who pays and what the payer has now |
| `.ga admin unlock <leader> <unlock id>` | administrator, also console / SOAP: buys an unlock for that leader's guild as *Buy* does (the same checks, the gold from `Unlocks.PayFrom`'s payer; test aid) |
| `.ga bot recruit <race> <class> <gender> <name>` | recruit a bot (ids as in protocol.md §8.7, gender 0/1) |
| `.ga bot order <name> <order> [zone id]` | set an order |
| `.ga bot bench <name>` / `.ga bot activate <name>` | bench / activate |
| `.ga bot dismiss <name> [confirm]` | dismiss a bot for good (see "Dismissing a bot"); without `confirm` it says what would happen |
| `.ga admin dismiss <bot> [confirm]` | administrator, also console / SOAP: the same for the guild the bot belongs to (test aid) |
| `.ga reload` | reload the world templates (also from the console / SOAP) |
| `.ga achievements [character]` | the guild's achievements (count, points, renown, the newest ten); with a character (also console / SOAP), that character's guild and how many of the guild's shareable achievements it has |
| `.ga perks [character]` | the guild's points (earned, granted, spent) and every perk with its rank and next cost (also console with a character) |
| `.ga perk buy <perk>` | buy the next rank for your guild (perk = key or id) |
| `.ga admin perk buy <leader> <perk>` / `reset <leader>` / `grant <leader> <points>` | administrator, also console / SOAP: buy for a leader's guild; drop every rank and grant (every point back); grant guild points |
| `.ga admin perk check <character>` | administrator, also console / SOAP: what the perks do for that character now (speed rate, sample cast times, flight paths known, no-reagent mask, XP / gold factors, breath, falls) |
| `.ga admin achieve <character> <id>` | administrator, also console / SOAP: a guild character completes achievement `<id>` the normal way, as if it had met the criteria (tests without a client) |
| `.ga milestones [character]` | the guild's tiered achievements: tiers reached of the active ones, guild points, renown, and every series it counted something for with its value and next tier (also console with a character) |
| `.ga admin milestone add <character> <key> <n>` | administrator, also console / SOAP: adds n to a sum (`kills_7`, `quests`, `dungeons`, `raids`) or raises a maximum (`level_class_1`, `level_race_3`, `skill_186`) to n for that character's guild, then awards what it reaches (tests) |
| `.ga admin milestone scan <character>` | administrator, also console / SOAP: reads the guild's characters in the world now instead of at the next scan |

## Tests

- `tools\test-gadv-codec.ps1` compiles `tests\gadv_codec_test.cpp` + `GadvCodec.cpp` with
  `cl /W4 /WX` and runs them against `docs\protocol-vectors.txt` (the addon work's generated
  vectors). 2026-11-09: 82/82 checks, 14 vectors.

## T2 results (2026-11-10, ga-probe on the integration server)

Run with the unattended client probe (PR #6) on `GAPROBE2` / leader `Probeguild`, intents sent through the
addon's own `GuildAdventure.Protocol` functions, results checked in the addon state and the database:

| Check | Result |
|---|---|
| Founding on the first login | "Guildlead Adventurers" (the owner's test leader), "Probelead Adventurers", "Probeguild Adventurers" founded with the leader as guild master; events logged |
| HELLO sync | addon `connected`; guild name, slots, renown, features `orders,recruit`, orders `AUTO,IDLE,FOLLOW,LEVEL_ZONE`, 25 unlocks, events arrive |
| `RECRUIT` (x2 via the addon, 1 via `.ga bot recruit` by the probe thread) | ACK; characters on bot account `GABOT0000501` (pool of the owner), `ga_bot` slots 1-3, members of the WoW guild, logged in at once; addon roster 3 |
| 4th recruit | `NO_SLOT` "All 3 bot slots are taken." |
| `ORDER_SET` | IDLE ok; LEVEL_ZONE Westfall ok (`o`/`oz`/`ozn`/`why` pushed); Durotar `ZONE_INVALID`; GRIND `NOT_AVAILABLE` |
| `PROF_SET` before / after renown level 2 | `LOCKED` / ok (plan stored in `ga_bot_profession_plan`) |
| `BOT_STATE` bench / activate | bot logged out (`benched`) / logged in again (`active`) |
| `.ga renown add 100` | renown level 2, unlocks "5 bot slots" and "Profession plans" owned, `STATE` pushed (rl 2, rn 250, slots 5) |
| Offline window (set to 1 min) | after the owner's session ended: "offline window of owner account 5 ended, 3 characters logged out" |

Bugs found and fixed: the first recruit crashed the worldserver (bot account inserts used statements that
are prepared for the asynchronous connections only; `b922838`); a leader that returns through a Path R attach
(no database login) did not log the bots in again (now handled on the swap's completion).

Verified afterwards (T2 ga-probe): a bot logged out while the owner played another bot logs in again when the
owner swaps back to the leader (attach).

Fixed issue: logging out one guild bot marked every character of its bot account offline in the database (core
`CHAR_UPD_ACCOUNT_ONLINE` in `WorldSession::LogoutPlayer`) although the other bots stayed in the world. The take-control
work fixed it in `GuildBotHolder` (`OnPlayerCanMarkAccountOffline`, `25cd719`); verified on Probeguild's bots
(one bot account, three bots): after `.ga bot logout Probepriest` the other two keep `characters.online = 1` (T2 ga-probe).

Away summary with real activity: done with M4 (ga-probe `away-summary` 12/12, report
`_work\ga-probe\reports\20261110_033751`, [orders.md](orders.md) § Status). Still open: Path W with the addon.
`CONTROL_TAKE` from the addon UI against the server (the take-control probe scenarios use `.ga swap`) is
covered by the addon's ga-probe run on the same day: scenario `ui-take-control` 27/27 PASS over Path R
(report `_work\ga-probe\reports\20261110_002442`, the design log §10).

## T2 test script (manual)

On the integration server (`_work\server`), with a GM account:

1. Enter the world with a fresh character → chat hint "you lead an adventure guild now";
   `.ga info` shows 3 slots, features `orders`, `recruit`; the guild "<name> Adventurers" exists.
2. `.ga bot recruit 1 1 0 Aldric` (Human Warrior) → "Aldric joined your guild"; the bot logs in
   (in Northshire), is in the guild roster; `.ga roster` lists it; `ga_bot_account` has one row.
3. Recruit two more; a fourth answers `NO_SLOT`. `.ga renown add 500` → level 2, 5 slots.
4. `/ga` (addon): roster, recruit dialog, orders and bench work against the live server.
5. Log out: bots stay in the world; with `.ga offline set 1` they log out after a minute; log in
   again → "while you were away" summary, bots log in again.
6. Take control from the addon (Play) → CONTROL_PENDING, character screen, enter the bot; return.
