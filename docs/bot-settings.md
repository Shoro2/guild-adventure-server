# Bot behaviour settings

How the guild leader changes how the guild's bots play: when they rest, how careful they are, which quests they take,
what they buy, how often their guild chat reaches the owner. The owner (2026-11-12): [quote left out]. Decisions and the list of settings: the design log §7 "Bot
behaviour settings for the guild leader" (BS1-BS4). Code: `BotSettings.h/.cpp`, `BotSettingsScripts.cpp`,
`Protocol/GadvSettings.cpp`, addon `UI/Behaviour.lua`.

## The model

- A **bot setting** is a key of `mod_guild_adventure.conf` that the leader may change in game. Its key is the conf key
  without `GuildAdventure.`: `Rest.BelowHealth` is `GuildAdventure.Rest.BelowHealth`.
- The **conf value is the default** for every guild (BS3). A server operator changes it there; a guild that set a value
  of its own keeps it.
- The leader sets a value for **the whole guild**, and, where the setting allows it (`perCharacter`), for **one guild
  character**: a roster bot or the leader itself (BS2). What counts for a character: its own value, else its guild's
  value, else the conf default.
- Each setting has a **range** (and for a choice, its options); the server refuses values outside it.
- Values are integers: on/off 0/1, a choice as the index of its option, money in copper, percentages 0-100, a decimal
  times its scale (`Escape.Margin` 1.25 is stored as 125).

Storage (characters database, `data/sql/db-characters/base/ga_settings.sql`): `ga_guild_setting` (owner account, key,
value) and `ga_character_setting` (character guid, owner account, key, value). Keys, not ids, are stored, so a setting
can be renumbered or removed without touching rows (rows of unknown keys are skipped at load and logged). Every change
is written through at once (world thread).

## Reading a setting in module code

```cpp
#include "BotSettings.h"

// one value (any thread: two hash lookups under a shared lock)
if (sBotSettings.IsOn(bot, GuildAdventure::Setting::RestEnable)) ...
uint32 below = uint32(sBotSettings.Get(bot, GuildAdventure::Setting::RestBelowHealth));
float margin = sBotSettings.GetFloat(bot, GuildAdventure::Setting::EscapeMargin);   // value / scale

// code that reads many fields of the conf struct: the struct as it counts for this character
GuildAdventure::Settings const settings = sBotSettings.For(bot);
if (settings.restEnable && health < settings.restBelowHealth) ...

// guild-wide settings without a character at hand
uint32 pause = uint32(sBotSettings.GetForGuild(ownerAccount, GuildAdventure::Setting::BotGuildChatMinSeconds));
```

A character in no adventure guild (or `nullptr`) gets the conf default, so the same code serves any player.
`For()` keeps pairs in order whatever was set (`Rest.UntilHealth` ≥ `Rest.BelowHealth`, the same for mana and for the
guild chat pauses).

## Adding a setting

1. The conf key as today: a field in `GuildAdventure::Settings` (`GuildAdventureSettings.h/.cpp`) and the key in
   `mod_guild_adventure.conf.dist`.
2. An id in `enum class Setting` (`BotSettings.h`), in the hundreds of its category; ids go over the wire and are never
   reused (a removed setting leaves a "retired" comment).
3. One row in the registry (`BotSettings.cpp`): id, key, category, type, min, max, step, scale, unit, per character,
   advanced, options, the conf field (`GA_BOOL(field)`, `GA_INT(field)`, `GA_FLOAT(field, scale)`), label, help.
4. The code that reads the conf field reads it through `sBotSettings` instead of `GetSettings()`.

Nothing else: the addon gets the new setting with the next sync (the schema version changes), the commands know it.

## Categories

| Key | Label | Settings (as of 2026-11-26, 136 settings) |
|---|---|---|
| `combat` | Combat and rest | `Rest.*` (`Rest.BeforeWalk`, 109: the rest before a walk through hostile ground, off by default), `Escape.*`, `Pull.Careful` (131: a creature that stands with others is pulled out to the bot, docs/orders.md "Careful pulls"), `Fight.ConsecrationMinFoes` (134: a paladin's Consecration only with this many foes in a fight within 8 yards, 2), `Fight.Interrupt` (135: a bot interrupts a foe's spellcast with its class's interrupt when it is ready and the foe in reach; on), `Fight.TemplatesByDefault` (136: a bot with no fight profile of its own in the slot it plays plays the template of its class and talent tree, docs/fight-profiles.md §12; on), `Fight.PotionBelowHealth` / `.PotionBelowMana` (132, 133: a healing potion in a fight below 35 % health, a mana potion below 15 % mana; the best the bot carries or the guild storage has) |
| `danger` | Danger and deaths | `Risk.*` (`Risk.MaxFightCostPercent`: how much of its health the fight at a target may cost) |
| `quests` | Questing and travel | `Quests.Escorts`, `Quests.Events`, `Quests.ItemsFromGuild`, `Quests.AbandonGrey` (grey quests leave the quest log), `AutoAdvanceDefault`, `Roads.*`, `Walks.*` (creatures above the bot on the way to a task: since the owner's ruling of 2026-11-14 a walk is refused from four levels above, `Walks.MaxLevelsAbove` 3), `Squads.CatchUpFallback` (313, guild-wide: a squad member that finds no way to its far leader is put beside it), `Travel.Hearthstone` (312: the hearthstone on travels only to another continent or a capital, never, or whenever it shortens one) |
| `grind` | Grinding | `Grind.Enable`, `Grind.LevelsBelow`, `Grind.LevelsAbove`, `Grind.MaxDistance` (grinding only when the zone has no quest work) |
| `farm` | Farm orders | `Farm.LevelsBelow` (4: only creatures this far below the bot), `Farm.GroundRadius` (80 yd: the size of a hunting ground), `Farm.MinSpawns` (4: the creatures a ground needs), advanced: `Farm.MaxDistance` (0 = anywhere on its land mass); docs/orders.md "Farm orders" |
| `upkeep` | Vendors and provisions | `Vendor.Trips` (at a hub after a hand-in / only when urgent / whenever due), `Provisions.*` (from level 8, how many, at a hub, a trip when nearly out; `Provisions.TripWhenOut` and `.TripWhenOutYards`: a mana user out of drink walks to a vendor within 600 yd), `Ammo.*` (412-417: a hunter's stock of 2000 / 1000 below level 20 / 200 below level 5 rounds, the trip below 100, one stack for other classes, the reach of a top-up; docs/storage.md section 9), `Storage.Supply.Provisions`, `Storage.Supply.Potions` / `.Bandages` / `.Scrolls` / `.Elixirs` (418-421: one piece out of the guild storage at the moment of use: the potion of a fight, a bandage in a rest with First Aid, the scroll of the main stat and elixirs on a task with fights; `Storage.Supply.Provisions` is now one piece of food or drink in a rest; docs/storage.md section 9), `Provisions.ConjureBelow` (422, advanced: a mage conjures water and food below 4 pieces and buys none) |
| `gear` | Gear and guild gold | `Storage.Supply.Gear`, `Gear.Buy.Enable`, `Gear.Buy.NearYards`, `Gear.WarnGap`, `Gear.FillEmptySlots`, `DungeonRoles.WearForLevelling` (509: a tank's or healer's dungeon set worn while it levels, docs/dungeon-roles.md), `Budget.ItemPercent` (guild-wide), `Budget.BotAllowance` (per bot), `Budget.MemberPercent` (guild-wide), `Guild.TaxPercent` (guild-wide; 526: the guild tax, a share of every gain of the roster bots into the guild bank, booked at once, docs/storage.md §10), `Unlocks.PayFrom` (guild-wide; 527: who pays for a gold unlock on the Progression tab: the guild bank, the guild bank and then the leader, or the leader; docs/guild-management.md "Buying unlocks"), `Guild.TaxHandovers` (guild-wide; 528: money handed over within the guild by trade or mail is taxed too) |
| `professions` | Professions | `Professions.LearnInPlace`, `.GatherDetour`, `.CatchUpGathering` (908: on the autopilot a gathering skill too low for the bot's zone is caught up before it quests on), `.CatchUpMinGap` (910: only when the skill is at least that many points behind, default 25), `.ConsumableStock` (920: crafters keep 20 of each endgame consumable of the guild's dungeon runs, docs/orders.md "Endgame consumables"), `.EnchantScrolls` (921: enchanters write the best enchantment for a guild character's blue or better piece of item level 60 or more on a vellum, the wearer puts the scroll on; docs/orders.md "Enchantments for the guild's gear"), `.EnchantMinItemLevel` (924, guild-wide: the guild's enchantments go on blue or better pieces from this item level, default 60), `.ProcessAbove` (922: raw materials above this many in the storage are processed, ore into bars, stone into grinding stones, cloth into bolts, default 1000; 0 = off), `.ProcessStock` (923: a processed material up to this many, bars out of ore uncapped, default 200), `.CookStock` (911: cooks make food for the guild until the storage holds this many of a kind, default 40; 0 = never), `.CookMinutes` (912, advanced: the next look after nothing was to cook, default 15), `.BuyRecipes` (913: recipes of its professions it can learn now are bought at vendors and learned at once), `.RecipeMaxPrice` (914: the most one may cost, default 1 gold), `.RecipeYards` (915, advanced: a vendor this near is walked to for one, default 300), `.FocusBehind` (916: in the profession mode a profession more than this many points behind the bot's highest is worked on alone, default 50; 0 = off), `.GreenMinChance` (917: no green recipe or enchantment for skill below this chance of a point a cast, default 20 %; 0 = any), `.AmmoStock` (918: engineers make ammunition for the guild's hunters with a gun until the storage holds this many rounds of their kind, default 5000; 0 = never; crafting for skill makes no ammunition while the storage holds this many of it), `.SkillCraftStock` (919: crafting for skill makes no other stackable consumable, a potion, a bandage, food or an explosive, while the storage holds this many of it, default 200; 0 = no cap), `.DisenchantSurplus` (909: an enchanter disenchants surplus crafted gear of the guild storage), `.CraftAtHub`, `.CraftBatch`, `.CraftGear` (which professions a bot has is its profession plan) |
| `class` | Class and training | `AutoLearnClassSpells`, `ClassAbilities.*` (`ClassAbilities.PetTalents`, 604: a hunter's pet spends its talent points along its tree's build, docs/orders.md "Class abilities"), `Poisons.Enable` / `.Stock` / `.Pair` (605-607: a rogue's poisons from level 20, docs/orders.md "Class abilities"), `ClassAbilities.Runeforging` (608: a death knight's best rune on a weapon without an enchantment), `DeathKnights.SkipStart` (609, guild-wide: a recruited death knight begins in the capital with the campaign counted as done, with or without its item rewards, or in Acherus; docs/orders.md "Death knights") |
| `chat` | Guild chat | `BotGuildChat.MinSeconds`, `.MaxSeconds` (guild-wide) |
| `loot` | Loot | `LootFilter.SellJunk`, `LootFilter.SellJunkQuality`, `LootFilter.KeepWornGear` (junk is sold at once, docs/storage.md "Junk"; 1003 `LootFilter.KeepWornGrey` retired), `Storage.UnwornBound`, `LootFilter.OpenContainers` (1006, per bot: bots open the containers they loot), `Storage.OpenContainers` (1007, guild-wide: bots take the storage's containers out and open them; docs/storage.md section 11), `Storage.LockboxMinutes` (1008, default 2 since 2026-11-26; rogues and blacksmiths with skeleton keys, guild-wide: one lockbox of the storage per rogue in this time), `Storage.ReserveForDungeons` (1009, guild-wide: what is ticked for a dungeon run may take into the storage's reserves), `Shopping.Enable` (1010, per bot: bots buy what the guild storage lacks of its shopping list at vendors they visit, docs/storage.md section 12), `Shopping.MaxPerVisit` / `Shopping.MaxPerDay` (1011, 1012, guild-wide: the guild bank's money for the list per vendor visit and per day), `Shopping.FromPurse` (1013, guild-wide: what the guild bank lacks from the bot's own gold), `LootFilter.WornGear` (1014, per bot: gear a bot has worn and will not wear again is disenchanted by it or sold at its next vendor visit, or kept; docs/storage.md "Gear it has worn"), `LootFilter.SpareQuestItems` (1015, per bot: quest items no quest needs any more are sold or deleted; docs/storage.md "Quest items no quest needs"), `Storage.DarkmoonCards` (1016, guild-wide: the Darkmoon Faire cards and decks the bots loot or carry go into the guild storage; docs/storage.md "Darkmoon cards"), `Storage.ReputationTurnIns` (1017, guild-wide: the quest items repeatable reputation quests ask for go into the guild storage; docs/storage.md "Reputation turn-ins"), `Auction.SellUnsold` (1018, guild-wide: what the guild storage put up in the auction house and did not sell is sold at the vendor price when it comes back; docs/storage.md section 8) (1005: never-worn bind-on-pickup loot may go into the guild storage, docs/storage.md "Soulbound loot") |
| `dungeons` | Dungeon runs | `Dungeons.EngageMargin` (1201, 8 yd), `Dungeons.BigPullAt` (1202, 4 creatures), `Dungeons.GateHealth` / `.GateHealerMana` / `.GateDamageMana` (1203-1205, 90 / 80 / 60 %): guild-wide, the value of the bot that leads the run counts (docs/orders.md "The run's settings", package PW) |

`.ga settings` lists the current set with its values. Settings marked *advanced* (time windows, distances) show in the
addon only with "Show all options" or when they have a value.

**Quests.Escorts and Quests.Events** are per character since the settings exist: the module now always finds escort and
event quests at startup (before, `GuildAdventure.Quests.Escorts = 0` / `.Events = 0` skipped that), and a character whose
setting is off takes no new such quest and does not work one it has (`QuestSupport::IsLeftAlone`).

## The addon

- **Settings tab → Bot behaviour** (the default view; the old interface options are the view **Addon**): the guild's
  values. Categories on the left with the number of changed settings, a search box over all categories (label, key,
  help), "Changed only", "Show all options"; per row the control (checkbox, choice, a number with − / + or typed in:
  `65`, `65%`, `1.25`, `1g 20s`), where the value comes from (*default*, *guild*, *this bot*) and **Reset**; at the
  bottom "N changed from the server's defaults", **Reset category**, **Reset all...**. Hovering a row shows the help,
  the server default, the guild's value (in a bot's panel), the range and the conf key.
- **Bot detail → Behaviour**: the same panel for one bot, only the settings a bot can have of its own; Reset goes back
  to the guild's value.
- The schema is cached in the SavedVariables (`settingsCache`) like the zones, so a sync sends it only when it changed.

## Commands

| Command | Who | What |
|---|---|---|
| `.ga settings [character]` | GM, console with a character | every setting with the value that counts for the character and where it comes from (`[own]`, `[guild]`, default) |
| `.ga setting <key> <value \| default>` | GM in game | the guild's value (the guild of your account) |
| `.ga setting <key> <value \| default> <character>` | GM in game | that guild character's own value |
| `.ga admin setting <character> <key> <value \| default>` | administrator, console, SOAP | the guild's value of that character's guild |
| `.ga admin botsetting <character> <key> <value \| default>` | administrator, console, SOAP | that character's own value |

Keys are case-insensitive and may carry the `GuildAdventure.` prefix; the key `all` with `default` drops every value of
the scope. Values: `on`/`off`, numbers with or without their unit, `1g 20s 5c`, `1.25`, an option's token or label.
A change from a command reaches the owner's addon at once.

## Protocol

docs/protocol.md §7.19: `HELLO^sv`, the blocks `SETDEFS` (records `SETCAT`, `SETDEF`) and `SETVALS` (records `SETVAL`),
the intent `SETTING_SET`, `GET^what=settings`.

## Open

- Settings the other threads add later, and the proposals from wowbot335 in the design log §7.
- A per-bot view for the leader in the addon (the API and the commands take the leader's guid; the Overview's leader
  panel has no Behaviour tab yet).
- "Share code" (copy a guild's values as text and paste them into another guild) once there is more than one owner.
