# Fight profiles (design; FP-0, FP-1 and FP-2 are built)

Bot combat logic as data the guild leader can read and edit in game: a read-only default profile per class and talent
tree that mirrors the mod-playerbots rotation, and profiles of the guild in which the rotation (a spell priority list
with conditions) is edited and assigned to bots.

- **State:** design of a research thread (2026-11-25, branch `claude/project-thread-fuuh81`); the owner decided FC1-FC11
  the same day (the design log §7 "Fight profiles"; this file follows them) and gave his go for the implementation ("ok
  go"). **The spike FP-0 is built and passed (T2 on a dev instance, §7):** the template read out of the strategies
  and put back as `ga rotation` / `ga buffs` plays as the default does, rules of the module's own fire. **The server
  core FP-1 is built and tested (§8):** profiles stored, compiled per bot, assigned per slot and changed by command.
  **The addon's Combat sub-tab FP-2 is built and tested (§9):** the template and the profiles read-only, the two
  slots, a copy of the template, "Copy to bots", the bot's own option values. Next: FP-3, the editor window.
- **The owner's request (2026-11-25):** [quote left out] The WRobot fight classes
  under `<WRobot FightClass folder>` are a schema reference only; their content is
  for other client versions and servers and is not taken over.
- **The owner's second pointer (2026-11-25):** the WRobot fight class `Talamin/AIO-Public` as a source of good configs;
  what the design takes from it is §4.10.

## 1. Goals

- The default rotation is visible in game, per class and talent tree, exactly as the installed mod-playerbots plays it.
- A copy of it is editable like a WRobot fight class: order, spells, target, conditions with numbers.
- Profiles belong to a guild (`owner_account`), are assigned per bot, and survive restarts.
- The first version covers the rotation in a fight, the buffs out of combat and usable items (the owner, FC2 and FC6).
- Non-goals for the first version: pet choice and pet actions, target choice, movement and positioning, pulls, and
  what the module's own fight rules cover (§4.11); they stay with mod-playerbots and the module. No Lua or script
  conditions.
- Anti-goals: a profile that makes a bot do what a player could not (spells of other classes, no cooldowns, no mana);
  a profile editor that can leave a bot standing in a fight; a second combat engine beside mod-playerbots'.

## 2. How a rotation is made today (mod-playerbots at the pinned commit, read 2026-11-25)

| Part | Where | What it is |
|---|---|---|
| Engine | `src/Bot/Engine/Engine.cpp` | per bot one engine per state (combat, non-combat, dead). `DoNextAction`: `ProcessTriggers` (every trigger of every active strategy whose check interval is due; a firing trigger pushes its actions with their relevance into a queue), `PushDefaultActions` (each strategy's default actions), then the queue is worked from the highest relevance: `isUseful`, multipliers, `isPossible`, prerequisites, execute; a failed or impossible action pushes its alternatives; one executed action ends the tick. Queued actions expire after `Playerbots.ExpireActionTime` (5 s) |
| Strategy | `src/Bot/Engine/Strategy/Strategy.h`, `src/Ai/Class/<Class>/Strategy/*` | a named rule set: `getDefaultActions()`, `InitTriggers()` (trigger name -> actions with relevance), `InitMultipliers()`, and action node factories (`ActionNode`: prerequisites, alternatives, continuers by name) |
| Trigger | `src/Bot/Engine/Trigger/Trigger.h`, `src/Ai/Base/Trigger/*`, class triggers | a named condition (`IsActive()`), 182 generic names (`low mana`, `medium aoe`, `enemy out of melee`, `behind target` ...) plus the class's (`art of war`, `seal` ...). Many class triggers are instances of generic classes: `BuffTrigger`, `DebuffTrigger`, `HasAuraTrigger`, `SpellNoCooldownTrigger`, `InterruptSpellTrigger`, `BoostTrigger` |
| Action | `src/Bot/Engine/Action/Action.h`, `src/Ai/Base/Actions/GenericSpellActions.*` | a named behaviour; `CastSpellAction(spell name)`: target by value name (`current target`), `isUseful` (`spell cast useful`), `isPossible` (`PlayerbotAI::CanCastSpell`: known, power, cooldown, range), `Execute` (`PlayerbotAI::CastSpell`). 200 generic names plus 64-175 per class |
| Relevance | `Strategy.h` | default rotation 5.x, normal 10, high 20, move 30, interrupt 40, dispel 50, raid 60, emergency 90 |
| Which strategies | `src/Bot/Factory/AiFactory.cpp` `AddDefaultCombatStrategies` | by class and talent tab (30 combinations): e.g. retribution paladin `ret`, `dps assist`, `cure`, `baoe`, plus the generic `racials`, `chat`, `default`, `cast time`, `potions`, `duel`, `boost`, `formation`, and by role `behind`, `tank face`, `healer dps`, `save mana` |
| Data-driven strategies that exist | `src/Bot/Engine/Strategy/CustomStrategy.*`, table `playerbots_custom_strategy` | strategy `custom::<name>` built from rows `trigger>action!relevance,...` per bot or for all. Only named triggers and actions: no numbers, no second condition |
| Per-bot registries | `src/Bot/Engine/AiObjectContext.h`, `NamedObjectContext.h` | strategies, triggers, actions and values are created by name per bot and kept in `created` maps. The module already replaces actions there (`BotDriver::GuardConsumables`: `drink`, `food`, `flee`, `blink back`, `consecration`), with no patch of mod-playerbots |

What the module does to the fight today: action replacements (above), strategy switches (`BotDriver::SyncStrategies`,
`ResetStrategies`, `KeepTotems`, `KeepAvoidAoe`, the dungeon buff plan), and steps of its own beside the engine
(`OrderService::StepSelfHeal`, `StepWand`, `StepPotion`, `StepSelfBuff`, the careful pull, `FightEscape`, the dungeon
conductor's rules).

## 3. Approaches

| | A: playerbots building blocks only | **B: a strategy of our own inside the engine (recommended)** | C: an executor of our own beside the engine |
|---|---|---|---|
| A profile is | rows "named trigger -> named action" (as `custom::`) | rules with our own parametrised conditions and "cast spell X on target Y"; playerbots triggers and actions stay usable as blocks | a WRobot-style runner in the module that picks the spell itself |
| Default as template | exact | exact: read from the strategies, translated into rules | rebuilt by hand for 30 combinations |
| Conditions | fixed names only | numbers, several per rule, target choice | anything |
| Movement, target choice, forms, buff plan, dungeon rules | stay | stay | partly rebuilt ("cat form first, then claw" is an action node chain today) |
| Effort / risk | small / small | medium / medium (spike first) | large / high |

A fails the owner's wish to set values ("Mana Percent"). C throws away what mod-playerbots knows per class and its
"template" would be an imitation. The owner chose **B** (FC1, 2026-11-25).

## 4. Design (approach B)

### 4.1 The default template

Read at run time, never stored: for a bot (or any bot of the class, with the spec's strategy names asked from its
context) the module calls `getDefaultActions()` and `InitTriggers()` of the *rotation strategies* and gets rows
(trigger name, action name, relevance), plus the action node chains. It is therefore always the rotation of the
installed mod-playerbots. Cached per class, tree and strategy set; a class with no bot in the world has no template
until one enters (a cache table can follow if that bothers).

Translation into profile rules, so that a copy is editable:

| playerbots trigger | Becomes |
|---|---|
| default action (no trigger) | no condition |
| `BuffTrigger(spell)` | "I lack aura *spell*" |
| `DebuffTrigger(spell)` | "target lacks my aura *spell*" |
| `HasAuraTrigger(spell)` / stack trigger | "I have aura *spell*" (stacks) |
| health and mana triggers (`low health`, `medium mana` ...) | "my health / mana % below N" with the configured threshold |
| `light / medium / high aoe` | "N or more attackers in range" |
| rage, energy, combo point triggers | "resource at least N" |
| `SpellNoCooldownTrigger`, `SpellCanBeCastTrigger` | "spell ready" |
| `InterruptSpellTrigger` | "target casts an interruptible spell" |
| `target low health`, `target critical health` | "target health % below N" |
| anything else (`art of war` if not a plain aura trigger, `boost`, class specials) | an opaque block "playerbots condition: *name*": usable, movable, not editable |

Actions: a class action stays "playerbots action: *name*" (so its node chain and its own checks stay) and is shown with
the icon and name of the spell it casts; a rule the user adds is "cast *spell* on *target*".

### 4.2 The profile

| Level | Fields |
|---|---|
| Profile | id, owner account (guild), class, talent tab, role (damage, tank, healer), name, note, the template it came from, revision, options (named values with a default and a range that conditions can refer to; each bot that uses the profile may set a value of its own, FC11), two rule lists: *combat* and *out of combat* |
| Rule | list, position, band (emergency, dispel, interrupt, high, normal, rotation; combat list only), enabled, action (playerbots action name, first-rank spell id, or an item: an item entry or "trinket 1" / "trinket 2"), target, flags (combat only, once per target, minimum interval in s, not twice in a row), up to 6 conditions (all must hold) |
| Condition | type, comparison (<, <=, =, >=, >), value(s) or the name of an option, "must hold" / "must not hold" |

- **Spells** are stored by the first rank of their chain; the bot casts the highest rank it knows and skips a rule
  whose spell it does not know yet, so one profile serves level 1 to 60.
- **Out-of-combat list (v1, FC2):** the buffs a bot keeps up between fights (armours, auras, seals, blessings,
  shouts, Fortitude, Mark of the Wild, a warlock's stones), one rule per buff with "I lack aura" or "a group member
  lacks aura"; no bands, the position is the order. Its template is read from the class's non-combat strategies as
  the combat list's is from the combat strategies. In a dungeon or raid group the module's buff plan (who gives
  which blessing, totem or shout) wins over the profile's rules for the kinds it plans.
- **Items (v1, FC6):** a rule's action may be "use item": an item with a use effect by its entry (a healthstone, an
  engineering device), or "trinket 1" / "trinket 2" (whatever is worn there, so a profile survives gear changes). A
  rule is skipped while the bot neither wears nor carries the item or its cooldown runs. Not offered: what the
  module's supply rules use (potions, bandages, food and drink, scrolls, elixirs, stones, oils, poisons; §4.11).
- **Targets (v1):** current target, self, pet, the party member with the least health, the tank, an enemy of the
  fight that meets the rule's conditions (to interrupt an add that casts), and a group member that meets them (a
  member that lacks a buff). The group conditions a healer profile needs beyond that follow with FP-4.
- **Bands** are mod-playerbots' relevance classes. The editor shows one list with divider lines; position gives the
  relevance inside a band (band base + a small step). Movement (30) and what stays with mod-playerbots sit between
  the bands, which is why a purely flat list would move rules above or below movement by accident (FC8).
- **Condition catalogue (v1)**, grouped: *me* (health %, mana %, mana points, rage / energy / runic power / focus,
  combo points, in combat, has aura with stacks and remaining time, moving, casting, stunned, swimming, behind the
  target, facing the target, level), *target* (health %, health points, has my aura with remaining time, has aura,
  casts / interruptible, elite or boss, creature type, distance, targets me), *around* (enemies within N yd of me / of
  the target, attackers on me), *pet* (exists, health %, has buff, casts), *group* (in a group, in a dungeon or raid,
  members below N % health, members within N yd of me / of the target, tank health %), *other* (spell known, spell
  usable, another spell ready, item count, item ready, playerbots condition). The catalogue is sent by the server
  (types, value ranges, labels), as the bot settings are: a new condition type needs no addon change.
- Checked against the owner's two screenshots of the WRobot editor (2026-11-25): its window has General Settings (fight
  class name, frames per second, range, additional C# code, pet: call / revive spell, use pet), "Spells (order by
  priority)" with Up / Down and Add Spell, Spell Settings, and Spell Conditions (list, Add Condition from a dropdown,
  Remove this Condition, Remove this Spell, a property grid per condition). Of the condition types in its dropdown
  the catalogue above takes Is Behind, Is Facing, Is Spell Usable, Item Count, Item Id Cooldown, Know Spell, Mana,
  Mana Percent, Me In Cast, Me In Combat, Me In Group, Me In Move, Me Is Stunned, Me Is Swimming, Me Level, Party
  Member Near (Target), Pet Buff and Pet Casting; left out on purpose: Lua Script and C# code, Is On Taxi, Me Is
  Flying, and types that do not exist in 3.3.5. Its pet settings stay with mod-playerbots in the first version (FC2).
- Schema references: WRobot's `FightClass` XML (general settings; spells with name, priority, cast-on, combat only,
  timer, once per target; conditions with a type and a typed parameter) and the owner's own wowbot335 fight classes
  (`profiles/fightclass/*.json`: `spells[]` with `spellId`, `priority`, `target`, `conditions[]` of `type` / `op` /
  `value`, options). Not taken from WRobot: Lua scripts as spells or conditions, range and frames per second.

### 4.3 Run time

- `FightProfiles` compiles a saved profile once into an immutable structure shared by all bots that use it
  (`shared_ptr<const CompiledProfile>`); a new revision is swapped in as a whole. Per bot only small state (interval
  timers, "once per target", and the bot's own option values: a condition that names an option reads the bot's
  value, else the profile's).
- Glue (the second place besides `BotDriver` that touches playerbots internals, or part of it): a strategy
  `ga rotation` whose `InitTriggers` emits one trigger node per enabled rule, a trigger class that evaluates the
  rule's conditions (playerbots triggers by name are asked through the context), and an action class that casts the
  rule's spell on its target through `PlayerbotAI::CanCastSpell` / `CastSpell`, or wraps the named playerbots action.
  The action re-checks the rule's conditions in `isUseful()` (queued actions live up to 5 s). The objects are put
  into the context's `created` maps like the module's replaced actions; no patch of mod-playerbots.
- Assignment: a "keep" step of the planner (like `KeepTotems`, `KeepAvoidAoe`; mod-playerbots resets strategies at
  logins and master changes) removes the *rotation strategies* of the bot's class from the combat engine and adds
  `ga rotation`; a bot without a profile is untouched. A bot has two slots, *solo* and *group*: the group slot
  counts while it is on a dungeon or raid map or a member of a dungeon run's group, else the solo slot; an empty
  group slot falls back to the solo slot. Changes apply at the bot's next quiet moment, never in a fight. A bot's
  two assignments can be given to other bots of its class in one step (FC3: "copy to bots").
- **Strategies replaced in v1** (the spike confirms the list per class): in the combat engine the spec strategy
  (`ret`, `shadow`, `frost`, `arms` ...), its `aoe` / `debuff` companions, `boost`, `cc` and a shaman's totem
  strategies; in the non-combat engine, as strategy `ga buffs`, the class's buff strategies (blessings, auras,
  armours, shouts) and the buff rows of its generic non-combat strategy. A row of a replaced strategy that the
  profile does not own (what the module's rules cover, §4.11; pet rows; resurrection, food and the like in a
  non-combat strategy) is emitted again unchanged and is not shown as a rule. **Kept:** `dps assist` /
  `tank assist` (target choice), `pull`, `behind`, `tank face`, `cure`, `potions`, `racials`, `default`,
  `healer dps`, `save mana`, raid strategies, and everything the module adds.
- Safety: auto attack / shoot is always the last rule; a bot with a profile that casts nothing for 10 s in a fight
  with a target falls back to the default rotation and writes an event for the owner (FC9).
- The module's own fight rules stay above any profile and are left out of templates and profiles (FC4; the list is
  §4.11).

### 4.4 Storage (`GA_characters`, own SQL file `ga_fight.sql`)

| Table | Content |
|---|---|
| `ga_fight_profile` | `id`, `owner_account`, `class`, `tab`, `role`, `name`, `note`, `template_key`, `revision`, `created_at`, `updated_at` |
| `ga_fight_profile_rule` | `profile_id`, `list` (0 combat, 1 out of combat), `position`, `band`, `enabled`, `action_kind` (playerbots action, spell, item, trinket slot), `action_ref` (action name, spell id or item entry), `target`, `flags`, `min_interval`, `conditions` (compact text, validated on save) |
| `ga_fight_profile_option` | `profile_id`, `option_key`, `label`, `kind` (switch, number, percent), `value`, `min_value`, `max_value` |
| `ga_bot_fight_profile` | `bot_guid`, `slot` (0 solo, 1 group), `profile_id`; no row = default |
| `ga_bot_fight_option` | `bot_guid`, `profile_id`, `option_key`, `value`; no row = the profile's value (FC11) |

Per guild like the talent templates (`ga_guild_spec_template`), so later every player has profiles of their own;
sharing by an export text is a later package.

### 4.5 Protocol (GADV, a new section of docs/protocol.md)

Intents `FIGHT_SAVE` (a whole profile: create or update; the answer names errors per rule), `FIGHT_DEL`,
`FIGHT_ASSIGN` (one or several bots of a class, slot, profile; several bots is "copy to bots"), `FIGHT_OPTION` (bot, profile, option, a value or "default"), `GET^what=fightprofiles | fightprofile | fightcatalog`; blocks `FPROFILES`
(the class's templates and guild profiles with who uses them), `FPROFILE` (one profile with its rules), `FCATALOG`
(the class's spells, targets, condition types with ranges and labels; versioned like `SETDEFS`); `BOT^fp/fpn` (the
assigned profile). Size: a rule is 40-100 bytes, a profile of 60 rules 4-6 kB, inside the 64 chunks of 240 bytes.

### 4.6 Validation (the server is authoritative)

- Only the guild's owner; profile and bot must belong to the guild; the profile's class must be the bot's.
- Spells: active spells the class can learn up to the level cap (trainer and talent spells) and racials. Items: items
  with a use effect that are no consumables of the module's supply rules, and the two trinket slots. Nothing else.
- Casting goes through mod-playerbots' normal checks (spell known, power, cooldown, range, line of sight).
- Limits (constants or conf keys): 20 profiles per class and guild, 60 rules per profile, 6 conditions per rule,
  values inside the catalogue's ranges, sanitised names.
- A profile whose tree differs from the bot's talents may be assigned, with a warning; unknown spells are skipped.

### 4.7 Performance

A profile replaces the triggers of the rotation strategies (20-60 per class) with at most 60 of its own, so the
reaction pass stays the size it is (about 400 trigger checks per bot five times a second today, docs/performance.md
F4). Conditions read mod-playerbots' cached values (`attackers`, `nearest hostile npcs`, health, auras) instead of
grid searches of their own. `.ga perf` gets counters (rules evaluated, casts by profiles). Acceptance: on the load
test instance with 1600 bots on a copied profile the mean tick is at most 5 % above the default's.

### 4.8 Addon

- Bot detail, new sub-tab **Combat** (beside Talents): "Fight profile (solo)" and "Fight profile (group)" as dropdowns ("Default (mod-playerbots)" or a guild profile of class and tree);
  below, the active rule list read-only (icon, name, target, conditions in short; unknown spells grey); buttons
  *Edit*, *Copy as new profile* and *Copy to bots...* (this bot's two assignments for other bots of its class). Under it "Options for this bot": the
  profile's options with this bot's values and a reset to the profile's value (FC11); *Copy to bots...* takes them
  along.
- **Editor window** (its own movable frame, about 860 x 540): left the profile list of the class (templates with a
  lock, guild profiles; *New* = copy of the selection, *Rename*, *Delete*; "used by"); middle the two lists as tabs,
  *Combat* and *Out of combat*: the spell priority list (enabled box, icon, name, target, conditions in short; band
  dividers; *Add spell* with a picker of the class's spells, search and "known by this bot"; *Add item*; *Up*,
  *Down*, *Remove*) and below it the locked box "Handled by guild rules" (§4.11); right *Spell settings* (target, combat only, once
  per target, minimum interval) and *Conditions* (list, *Add*, *Remove*; type, comparison, value, must hold); top
  name, note and *Options* (the profile's named values); bottom *Save*, *Discard*, *Assign to...*, the server's message line. A template opens locked with
  "Template (mod-playerbots), read-only. Copy to edit."
- UI texts in English like the rest of the addon; spell names and icons from the client (`GetSpellInfo`).
- Mock server and offline tests as for every screen; ga-probe scenario `fight-profile`.

### 4.9 Commands, settings, logs

`.ga fight <bot>` (profile, rules, what each rule did lately), `.ga admin fight dump <bot>` (the template as rows),
`.ga admin fight assign <bot> <profile | default>`, `.ga admin fight check <profile>`; conf / bot setting
`Fight.Profiles.Enable`; activity lines for an assignment, a fallback, and (debug) per cast "rule N".

### 4.10 AIO (Talamin/AIO-Public) as a reference

the owner, 2026-11-25: [quote left out]. Read through the GitHub API on 2026-11-25 (default branch `master`, last push 2025-01-23): a
WRobot fight class for WotLK 3.3.5 in C#, 55 rotation files for the ten classes (`AIO/Combat/<Class>/Solo<Spec>.cs`,
`Group<Spec>.cs`, `LowLevel.cs`, one PvP file), buff, pet, totem and poison helpers, and one settings class per class
(`AIO/Settings/<Class>LevelSettings.cs`). **The repository has no licence:** nothing of it is copied; it is a reference
for structure and for what a good profile contains.

| AIO | What the design takes from it |
|---|---|
| A rotation is a list of `RotationStep(action, priority, target predicate, constant predicate, target finder, exclusive, forceCast, checkRange, checkLoS, forcedTimerMS, preventDoubleCast, ignoreGCD)` (`AIO/Framework/RotationStep.cs`) | It confirms the rule model (spell, priority, conditions, target). The flag "not twice in a row" is added; range and line of sight are always checked by the server |
| Target finders: me, the bot's target, any enemy that meets the predicate, an enemy that casts, a party member, the tank, the pet | A target "an enemy of the fight that meets the rule's conditions" in the first version, "a group member that meets them" with FP-4 |
| Settings per class and rotation with a default (a mana threshold for Divine Plea, the number of enemies for Consecration, "heal in combat" on or off), named by the steps' conditions | **Profile options:** named values with a default and a range; a condition's value may name an option; an *Options* view in the editor. Moved from FP-5 into the first version |
| Separate rotations for solo, group and low level, chosen by itself (`ChooseRotation`) | The two slots per bot are **solo** and **group** (FC3). No low-level profiles: unknown spells are skipped, and "level" and "spell known" are conditions |
| The rotations as content for 3.3.5 | The second row of templates beside the mod-playerbots default (FC5, decided). The owner's own wowbot335 fight classes (`profiles/fightclass/*.json`, 43 files: solo specs, low level and fallback per class) already have nearly this shape (`priority`, `target`, `conditions`, `forcedCooldownMs`, `preventDoubleCast`, `ignoreGcd`, `options` with `optionRefValue`), so an importer for them is a small package (FP-4) |

### 4.11 What the module's own rules cover (FC4)

the owner, 2026-11-25, to FC4: [quote left out]. The module's rules stay above every profile, and what they cover is left out of templates and profiles: a
template row whose action a module rule owns is not shown as a rule (it is emitted again unchanged, so a bot plays as
before), and the editor lists these rules in a locked box "Handled by guild rules" with the bot setting that steers
each. Nothing stops the leader from adding such a spell as an offensive rule of his own.

| Module rule (where; bot setting) | What it does itself | In a profile |
|---|---|---|
| Self heal (`OrderService::StepSelfHeal`, `SelfHealSpell`; `Fight.SelfHealBelow`) | a paladin, priest, shaman or druid heals itself below 45 % health; a paladin's Holy Light / Flash of Light rule, Lay on Hands and Divine Shield in an emergency | not in damage and tank profiles; a healer profile keeps its heals as rules |
| Potions, bandage (`StepPotion`, the rest; `Fight.PotionBelowHealth`, `Fight.PotionBelowMana`) | healing and mana potion in a fight, a bandage in a rest | not offered as items |
| Wand (`StepWand`) | Shoot as a filler and to finish a nearly dead creature | no rule |
| Leaving a fight, protection (`FightEscape`, `Escape::StepDefend`, `Escape::StepBack`; `Escape.*`) | Frost Nova, Sprint, Vanish, Psychic Scream, Blink on a run; Evasion, Barkskin, Divine Protection, Shield Wall or Ice Barrier at the second attacker; a caster's step back to range | no emergency rules; the spells can still be added as offensive rules |
| Careful pull (`Escape::StepPull`, `PullOpener`; `Pull.Careful`) | the opener from range and pulling a single creature out | pulls stay with the module |
| Consecration (`GuildConsecrationAction`; `Fight.ConsecrationMinFoes`) | only with N foes in reach | a copy's rule carries it as a condition with an option; the bot setting stays the rule of the default |
| Rest (`StepRest`; `Rest.*`, `Provisions.*`) | eating, drinking, a mage's conjuring | stays with the module |
| Mage armour, one Searing Totem while levelling (`StepSelfBuff`, `BotDriver::KeepTotems`) | buffs mod-playerbots does not set fittingly by itself | become rules of the profile's lists; for a bot without a profile the module's rule holds |
| Pets and poisons (the voidwalker strategy, `ClassAbilities`, `RoguePoisons`) | a warlock's voidwalker, a hunter's pet and its talents, a rogue's poisons | stay with the module; pet actions follow with FP-4 |
| Dungeon rules (`StepDungeonFight`, `StepDungeonRescue`, the buff plan) | the tank's target, taunts, a healer without damage spells, who gives which blessing, totem or shout | stay above the profile; in a group the buff plan wins over the out-of-combat list for what it plans |
| Supplies from the guild storage (`StorageSupply::UseBuffs`, `DungeonSupplies`) | scrolls, elixirs, stones, oils | stay with the module; items in a profile are trinkets, a healthstone and other usable items |
| Ground effects, travel forms, a leftover Auto Shot (`StepGroundEffect`, `LeaveTravelForm`, `StopLeftoverAutoShot`) | stepping out of ground effects, leaving a travel form to fight, ending a stale Auto Shot | stay with the module |

## 5. Packages

| Package | Content | Done when |
|---|---|---|
| **FP-0 spike** | dev instance: dump a bot's rules (the combat rows and the buff rows of the non-combat engine), put them back as `ga rotation` / `ga buffs` in place of the replaced strategies with the rows a profile does not own emitted unchanged, add one own condition, one free cast and one item use | warrior, mage and a healing priest in a dungeon play with the re-injected template as before: same spell distribution over 50 fights, fight duration within 10 %. If the engine refuses, the question of the approach goes back to the owner |
| **FP-1 server core** | tables, compile, glue, condition catalogue, options with a value per bot, the out-of-combat list, item rules, the rows left to the module's rules (§4.11), assignment (solo and group slot, one or several bots), fallback, commands; no UI | a profile made by command steers a bot; activity lines name profile and casts (T2 over SOAP) |
| **FP-2 Combat sub-tab** | protocol, template view, assignment, *Copy to bots...*, the bot's own option values | ga-probe: template visible, assignment acts, no Lua error |
| **FP-3 editor** | copy, edit both lists (spells and items), spell settings, conditions, options, the "Handled by guild rules" box, save with errors | ga-probe: copy, move a rule, change a condition, save, the bot fights by it; offline tests green |
| **FP-4 more content** | pet actions, group conditions for healers, the second row of templates (curated profiles with options: an importer for the wowbot335 fight class files, checked against AIO; FC5; the templates are built, §11) | a healer profile carries a dungeon run without a wipe |
| **FP-5 comfort** | export / import text, a rotation log (which rule fired, which did not and why), fight statistics per profile, profiles for the leader in auto mode | two profiles can be compared on one bot |
| **FP-6 load test and docs** | 1600 bots on a copied profile; this file as built, protocol.md | mean tick at most 5 % above the default |

## 6. Risks and questions for the spike

- Action node chains belong to the strategies (`Engine::Init` copies the factories of the active strategies): with the
  spec strategy removed, a wrapped playerbots action loses its prerequisites and alternatives unless `ga rotation`
  hands out the same nodes (`Strategy::GetAction`). To prove per class in FP-0.
- Multipliers of the removed strategies (threat, cast time rules inside spec strategies) go with them; list what each
  spec strategy's `InitMultipliers` adds and keep those.
- Strategies whose constructor or `InitTriggers` looks at the bot (level, known spells) make the template depend on
  the bot it was read from; the cache key must include what they look at, or the dump is per bot.
- mod-playerbots' raid strategies and the module's dungeon rules name class actions; they must act the same with
  `ga rotation` (acceptance of FP-0 includes a dungeon).
- `ResetStrategies` (talent sync, dungeon role change, `KeepTotems`) must not drop `ga rotation` for longer than a
  planner tick; the keep step has to be cheap (a `HasStrategy` test).
- The out-of-combat list (FC2): a class's generic non-combat strategy mixes buffs with other rows (resurrection, food,
  pet care). FP-0 shows whether its buff rows can be told apart by their action class and the rest emitted unchanged.
- Buffs in a group: the module's dungeon buff plan switches mod-playerbots' blessing and totem strategies today; with
  `ga buffs` it has to steer the profile's rules instead (skip the kinds it plans).
- Rows left to the module's rules (§4.11): the dump has to recognise them by action name per class; a row it misses
  shows up as an ordinary rule, which is harmless but doubles what the box says.

The answers of the spike are §7.

## 7. FP-0: the spike (2026-11-25)

**Built** (`src/FightProfiles.h/.cpp`, a keep step in `OrderService::UpdateBot` beside `BotDriver::GuardConsumables`,
the loader): the strategy `ga rotation` in the combat engine and `ga buffs` in the non-combat engine; a rule is a row
of mod-playerbots (trigger -> action, relevance, the rows of one trigger node kept in one node) or a rule of the
module's own (a spell by name, a usable item by entry, a playerbots action, each with up to several conditions:
health, mana, target health, an aura missing on the bot or on its target, attackers, a playerbots trigger); the
strategy carries the action node factories, multipliers, type and target exclusions of the strategies it stands for;
its objects are put into the bot's AI context and the old ones deleted after the engine's next `Init`. A keep step
puts a profile back after a strategy reset and drops it (the default plays, an activity line) when the bot's
strategies are another talent tree's. Casts and fights are counted by a player hook. Test commands, administrator,
console and SOAP: `.ga admin fight <character> [buffs] dump | nc | template | on | off | rows | spell | item | act | rel
| del | stats [reset]` (the syntax is in the file's comment).

**What was run** (dev instance `fight`: core `23297a835` with CP-13, the integration checkout's mod-playerbots with
the PF-6 patch, the GA databases cloned, a test guild; bots of level 30 boosted with `.ga admin boost`; SOAP, no
client): pairs of one class, one on the default (A), one on a copy of its template (B), on a farm order for Wool
Cloth (the Blackrock camps of Redridge), counted 40 minutes and, after a restart, another 30 minutes for the mages
and priests; then the same group of five twice through the Scarlet Monastery Graveyard, once on the default, once
with copies of their templates put on inside (the dungeon talents: Protection, Holy).

| Class (tree) | Fights A / B | Mean fight A / B | Casts by kind A / B |
|---|---|---|---|
| Warrior (Arms) | 60 / 57 | 7.8 / 7.4 s (-5 %) | Rend 18 / 20 %, Overpower 14 / 17 %, Execute 14 / 11 %, Slam 13 / 9 %, Bloodrage 11 / 12 %, Battle Shout 7 / 6 %, Victory Rush 6 / 7 %, Heroic Strike 5 / 4 %, Thunder Clap 4 / 5 % |
| Mage (Frost) | 57 / 60 | 14.4 / 14.1 s (-2 %) | Frostbolt 44 / 40 %, Frost Nova 24 / 25 %, Icy Veins 9 / 9 %, Blink 5 / 9 %, Replenish Mana 5 / 6 %, Mana Shield 3 / 6 % |
| Priest (Shadow) | 56 / 64 | 16.0 / 14.9 s (-7 %) | Mind Blast 28 / 31 %, Shadow Word: Pain 27 / 28 %, Power Word: Shield 23 / 20 %, Devouring Plague 15 / 14 %, Smite 4 / 4 % |

Scarlet Monastery Graveyard, the same five (Protection warrior, Holy priest, two Frost mages, an Arms warrior): on the
default 2 of 2 bosses, no wipe, 14 min, 17 fights; on the copies 2 of 2, no wipe, 10 min, 14 fights. The tank cast
Sunder Armor, Thunder Clap, Revenge, Rend and Taunt in both runs in about the same shares (27 / 22 %, 17 / 21 %, 12 /
15 %, 10 / 7 %, 10 / 7 %), the healer Heal, Renew and Dispel Magic; the run's acceptance numbers (docs/orders.md
"The acceptance numbers of a run") stayed in the same range (the healer out of heal reach 0 of 17 / 0 of 14). After
the run both went back to their levelling talents and the keep step dropped their copies ("plays its default rotation
again: its strategies changed (a talent tree)").

Rules of the module's own: a mage with "Fire Blast at relevance 25 while the target is below 30 %" cast it 7 times in
38 fights (the mages on the default: 0; their own Fire Blast row, a default action at 5.2, never comes before
Frostbolt); a warrior with "use Nifty Stopwatch on itself" used it twice in 40 minutes (its cooldown is 30 minutes);
an action of mod-playerbots under conditions (Thunder Clap with two attackers and without its debuff on the target)
was taken. The buff lists: a priest on `ga buffs` (in place of `buff`, `cure`, `nc`, `rshadow`) and a mage (in place
of `bmana`, `buff`, `cure`, `nc`) renewed Power Word: Fortitude, Divine Spirit, Inner Fire and Arcane Intellect (read
from `character_aura` after `.saveall`). No crash, no new line in `Errors.log`, no death of a test bot.

**Answers to §6:**

- Action node chains and multipliers: carried by `ga rotation` (the factories of the strategies it stands for are
  copied, their `InitMultipliers` called); the spec strategies of the three classes have no multipliers of their own
  (they sit in `cast time` and `force rebuff`, which stay). No difference in the runs.
- The template depends on the bot: `InitTriggers` of several class strategies look at the bot (the druid's forms by
  `HasSpell` and auras, the mage's mana gem by the gem it knows, the shaman's totems by the totems it knows). The dump
  is therefore made per bot, at the moment (FP-1: a template cache keyed by class, tree and the spells those
  strategies ask for, or a dump per bot).
- The spec strategies contain the generic rows of `CombatStrategy` (reach spell 20, drop target 99, check mount state
  54, reset 1, set facing 37, and for melee reach melee 21): they are no rotation; FP-1 emits them unchanged and does
  not show them as rules.
- A talent change (a dungeon role's talents, a new template) changes the set of strategies: a profile belongs to a
  tree. FP-1 keys a bot's profiles by tree and slot (FC3's group slot is the dungeon tree of a tank or healer).
- `ResetStrategies` and the module's strategy switches: the keep step puts the profile back in the same planner step;
  its cost is a map lookup and a strategy name test for bots with a profile (the load test is FP-6).
- The out-of-combat list: the buff rows can be told apart by their action class (`CastBuffSpellAction`: Inner Fire,
  Vampiric Embrace, Fortitude on party, Divine Spirit, Arcane Intellect, Vigilance, Mage Armor, Shadow Protection);
  two exceptions to a hand list: "focus magic on party" is a plain spell action, "conjure mana gem" counts as a buff.
  The other rows of `nc` (clean quest log, check mount state, resurrection, the heals out of a fight, apply oil or
  stone, set pet stance) are emitted unchanged.
- `cure` was among the replaced strategies in the spike; §4.3 keeps it: FP-1 follows §4.3.
- Counting: the player hook counts spells cast, not melee swings; it is the measure of the comparison, not of a
  bot's damage.

## 8. FP-1: the server core (2026-11-25)

**Built** (`src/FightProfileStore.h/.cpp`, `src/FightProfiles.h/.cpp`, `src/FightProfileCommands.cpp`, module SQL
`data/sql/db-characters/base/ga_fight.sql`; the store is loaded with the other templates at startup):

- **Storage** (§4.4, with two changes): the five tables as designed; a rule stores its **relevance** itself (the
  bands of FC8 are the editor's view of it, FP-3), and its trigger and action names, spell id (the first rank of the
  chain), item entry or trinket slot, target, flags, interval, the template row's node and source strategy, and its
  conditions as text. A profile has no `role` column (the class and talent tree say enough for now).
- **What a profile stores:** only the rules it owns. The rows of the replaced strategies it does not own are read from
  the bot's strategies each time the profile goes in and emitted as they are: in the fight list movement and target
  housekeeping (reach spell, reach melee, drop target, check mount state, reset, set facing, set behind), the auto
  attacks ("melee", and a hunter's "auto shot" since 2026-11-28), the module's own (the wand "shoot", "flee", "blink back", oils, stones, poisons) and every row of
  a pet; in the buff list every row that is no buff (§7). `cure` is no longer replaced (§4.3).
- **Conditions** (§4.2's catalogue for v1, the text form `[!]key[:param][op value]`, joined by `;`, a value may name
  an option as `$key`): *me* hp, mana, manapts, rage, energy, runic, combo, combat, aura:spell, stacks:spell, moving,
  casting, stunned, swimming, behind, facing, level; *target* thp, tdebuff:spell, taura:spell, tcasting, tinterrupt,
  telite, tboss, tdist, ttargetsme (FP-4 added tdebuffleft:spell, ttype:type, tenemies:yards, §11); *around* attackers (of the group), myattackers, enemies:yards; *pet* pet, pethp;
  *group* ingroup, indungeon, membersbelow:percent, tankhp; *other* known:spell, ready:spell, itemcount:item,
  itemready:item, opt:option, trig:trigger. A condition that cannot be known (no target, no pet, no mana pool) does
  not hold, also when it is turned round. Target conditions are asked of the rule's target.
- **Rules:** a row of mod-playerbots (trigger -> action, as the template has it), an action of mod-playerbots under
  conditions, a spell (the bot casts the highest rank it knows; a spell it has not learned is skipped), a usable item,
  a trinket slot. Targets of spells, items and trinkets: the current target, self, the pet, the member of the group
  with the least health, the tank, an enemy of the fight that meets the target conditions (the nearest); a row or
  action of mod-playerbots keeps its own target. Flags: once per target, not twice in a row (the bot's last cast was
  this spell), a minimum interval; only for spells, items and trinkets.
- **Options (FC11):** a condition's `$key` and `opt:key` read the bot's own value of the option, else the profile's.
- **Assignment (FC3):** per bot two slots; the group slot plays on a dungeon or raid map and as a member of a dungeon
  run, else (and when it is empty) the solo slot. A profile of another class is refused; one of another talent tree
  may be assigned (a warning) and plays only while the bot's tree is the profile's (`AiFactory::GetPlayerSpecTab`).
  `copyto` gives other bots of the class both assignments and the bot's own option values.
- **Changes go in between fights:** the keep step of `OrderService::UpdateBot` (out of a fight) puts the profile of
  the slot in, takes it out, or puts it in again after a strategy reset; a save of the profile, an assignment or an
  option value raise the bot's version, and the profile goes in anew at its next quiet step.
- **Fallback (FC9):** in a fight (`OrderService::UpdateBot`'s fight branch) a bot on a profile that for 10 s casts
  nothing, does not swing or shoot, does not move and is held by no crowd control while it has a living target is
  put back on the default of mod-playerbots (both lists) until the profile is saved or assigned again; the owner gets
  an event `FIGHT` and the activity log a line.
- **Activity lines:** "plays fight profile #id "name" (solo slot, rotation): N rules of its own and M rows left to
  mod-playerbots and the module, in place of ...", "plays the default of mod-playerbots: profile ... is made for talent
  tree ...", the fallback, and per fight on a profile "fought N s by its fight profile "name": M casts (spell count,
  ...)".
- **Commands** (`FightProfileCommands.cpp`; administrator, console and SOAP unless named): `.ga fight <character>`
  (GM: its slots, option values and what it plays now); `.ga admin fight <character> dump | nc | template [buffs] |
  copy [<name>] | list | assign <solo|group> <id|default> | option <id> <key> <value|default> | copyto <character> ...
  | forget | stats [reset]`; `.ga admin fightprofile <id> show | check | delete | rename <name> | note <text> | rule add
  <c|n> <spell|item|trinket|action|row> <relevance> <ref> [settings] | rule set <c|n> <index> [settings] | rule del
  <c|n> <index> | rule move <c|n> <from> <to> | option set <key> <switch|number|percent> <value> [<min> <max>]
  [<label>] | option del <key>`; settings `rel=`, `target=`, `cond=`, `trigger=`, `interval=`, `once`, `!once`,
  `notwice`, `!notwice`, `on`, `off`.
- **Level-ups and logins:** `BotDriver::SyncStrategies` compares the talents' rotation with the strategies `ga
  rotation` stands for (it logged "plays by its talents again: ga rotation -> ..." at every level-up before); the
  reset it does still takes the profile out, and the keep step puts it in again.

**What was run** (dev instance `fight`, the test guild of §7, SOAP, no client):

- A copy of a Frost mage's template (`copy`): 34 rules of its own, 10 rows left as they are (reach spell, drop target,
  check mount state, reset, set facing, flee, blink back, toggle pet spell, set pet stance, shoot), 4 buff rules
  (Mage Armor, Arcane Intellect, Focus Magic) and 4 rows left (clean quest log, check mount state, apply oil, conjure
  mana gem). An option `fbhp` (30) and a rule "Fire Blast while the target is below `$fbhp` % and within 20 yd" at
  relevance 25; one mage on it with its own value 50, a second mage given both by `copyto`, a third on the default.
  On a farm order in Redridge: the two on the profile cast Fire Blast 8 times in 19 fights and 11 times in 19, the one
  on the default never (24 fights); fights 11.7 and 13.8 s against 15.1 s; the other spells in about the same shares.
- `rule set` (relevance, interval 6 s, not twice in a row), `rule move` to the top, `check` ("plays it"), a
  restart (the store loaded 4 profiles and 5 bots) and a level-up (the profile went in again, no "plays by its
  talents again" line) changed nothing of that.
- **Fallback:** a shadow priest on a copy with every fight rule deleted and its wand taken off: "Fpriestb did nothing
  for 10 s in a fight against Blackrock Sentry under its fight profile "Shadow empty": it plays the default of
  mod-playerbots until the profile is saved or assigned again", the event `FIGHT` in `ga_event_log`.
- **Slots and talent trees:** a warrior's copy made while its Protection dungeon talents were on (tree 2), put into
  both its slots: while it levelled as Arms "plays the default of mod-playerbots: profile #3 "Prot copy" is made for
  talent tree 2, its talents are of tree 0"; in a run of the Scarlet Monastery Graveyard (the group of §7) "plays fight
  profile #3 "Prot copy" (group slot, rotation): 41 rules of its own and 7 rows left ..." at the entrance, after the run
  the default again. Its healer got a group profile in the middle of the run (a copy of its Holy template and a rule
  "Flash Heal on the member with the least health below 70 %"), which went in at its next quiet step and out at the
  end ("no fight profile in its solo slot"). The run: 2 of 2 bosses, no wipe, 18 min.
- Found on the way, not of the fight profiles: a level-up in a dungeon run resets the bot's strategies and brings
  mod-playerbots' "healer dps" back, which the run takes off only once per run, so the healer cast Smite, Holy Fire and
  Shadow Word: Pain for the rest of the run (reported to the dungeon thread's owner).
- No crash, no new line in `Errors.log`.

**Not tested:** the targets pet, tank and enemy, trinket rules, `opt:` conditions, `delete`, the other classes, a
raid, the tick cost (FP-6).

## 9. FP-2: the Combat sub-tab (2026-11-25)

**Built** (server `src/Protocol/GadvFight.cpp` and the dispatch in `GadvProtocol.cpp`, `FightProfiles::Refusal` /
`TalentTree`, a push of the bot's record when what it plays changes; addon `UI/Talents.lua` (the sub-tab, no TOC
change), `Protocol.lua`, `State.lua`, `MockServer.lua`; protocol: docs/protocol.md §7.26):

- **Protocol:** `BOT^fp/fpg/fpn/fpsl/fpw` (the two slots, what it plays now and from which slot, why not), blocks
  `FPROFILES` (the guild's profiles of the bot's class with its slots), `FPROFILE` (one profile's rules; id 0 the
  template read from the bot, with the strategies it stands for and how many rows stay as they are) and `FOPTS` (the
  profile's options with this bot's values), intents `FIGHT_ASSIGN` (a slot, or `to` = "Copy to bots"), `FIGHT_OPTION`
  (a bot's own value or back to the profile's), `FIGHT_COPY` (a new profile from the bot's template). A row of the
  template carries the spell the bot casts by that name as `ref`, for its icon. The block `FCATALOG` of the plan (the
  condition catalogue) moves to FP-3: the read-only list gets each rule's conditions in words from the server.
- **The sub-tab Combat** (beside Talents): a status line (what it fights by and from which slot, or why not), the
  dropdowns *Solo* and *Group* (the default and the guild's profiles of its class; a profile of another talent tree
  grey with a tooltip), *Copy to bots...* (one bot of its class or all of them, with a confirmation that names both
  profiles), *Show* (the template or any profile of the list), *Fight* / *Between fights*, a name box and *Copy as new
  profile*; the rules read-only in the engine's order with a divider line where a relevance band begins (FC8), each
  with its icon, name, target, trigger and conditions in words and its relevance, a tooltip with the rest; on the
  right *Options for this bot*: each option with the bot's value (gold when it is its own), *Set* or *On / Off*, and
  *Default*. Editing the rules is the editor window of FP-3.
- **Mock server and offline tests:** the mock has a template of three rules, makes copies with two options and
  answers all three intents; offline tests 742 of 742 (the sub-tab's test covers the template, the bands, a copy, an
  assignment, an own value and a refused one, the reset, "Copy to bots" and the default again).

**What was run** (dev instance `fight`, the probe guild Probeguild, **T2 (ga-probe)**): `connect` 5/5, the new
extended scenario `fight-tab` 30/30 twice (reports `_work\ga-probe\reports\20261125_170236`, `20261125_170520`): the
sub-tab of Probemage (Frost, level 43) opens without a Lua error, the profiles of its class and its template arrive
(read from the bot in the world), `FIGHT_COPY` makes "Probe rotation", `FIGHT_ASSIGN` puts it into the solo slot and
the bot plays it (`fpn`, `.ga fight` "solo slot: #5 "Probe rotation""), an option made by command gets the bot's own
value 45 from the addon (`.ga fight`: "option fbhp: 45.0 (its own value)"), 500 is refused (`FIGHT_INVALID`), the
default comes back, and the default slot again ends the profile (`fpn` empty). In the second run the template's rows
show their spells' icons (Ice Block, Fire Ward, Evocation, Mana Shield ...).

**Not tested in the client:** the dropdowns clicked by hand (the probe sends the intents the dropdowns send),
"Copy to bots" in the client (offline and mock only: the probe guild has no two bots of one class), a refusal text
(`fpw`) on screen, a benched bot (its template does not show).

## 10. FP-3: the editor window (2026-11-25)

**Built** (server `src/Protocol/GadvFight.cpp`, the dispatch in `GadvProtocol.cpp`, `FP::ClassSpellFamily`; addon
`UI/Talents.lua` (the window, no TOC change), `Protocol.lua`, `State.lua`, `MockServer.lua`; protocol: docs/protocol.md
§7.26 "The editor"):

- **Protocol:** `FIGHT_SAVE` (a whole profile as the editor has it: name, note, every rule and option; the store checks
  it as the commands do and keeps it or nothing; the error names up to four reasons, a rule as `rule i` counted over
  the fight list and then the list between fights), `FIGHT_DEL`, `FIGHT_COPY` with `from` (a copy of a stored profile,
  "<name> copy"), `GET^what=fightcatalog` with the blocks `FCONDS` (the condition catalogue), `FSPELLS` (the spells of
  its class the bot knows, by their first rank, while it is in the world) and `FITEMS` (the usable items it carries a
  rule may use); `FRULE` carries the row's node `nd` (a saved row of mod-playerbots is found again by strategy and
  node).
- **The window** (*Edit...* in the Combat sub-tab opens it on the profile shown there): left the template (locked) and
  the guild's profiles of the bot's class with their tree, rule counts and users, *New copy* (of the selected profile,
  or of the template) and *Delete* (with a confirmation); top the name, a note and *Options*; in the middle the lists
  *Fight* and *Between fights* in the engine's order with the band dividers (FC8), *Up* / *Down* (past a rule of the
  same relevance by the order, inside a band by swapping the relevance, over a divider into the next band with the
  other rule's relevance), *On / off*, *Remove*, *Add spell ...* (the bot's class spells in submenus of 15) and *Add
  item ...* (its items, trinket 1 and 2), a new rule under the selected one with its relevance; below the list what
  the guild's own rules handle (§4.11); right the selected rule: its target (spells and items only; no enemy between
  fights), relevance and interval with *Apply*, *Once per target*, *Not twice in a row*, its conditions (each with *X*)
  and the form for a new one (the condition by group, *not*, the comparison and its value or `$option`, the parameter:
  a spell by name or id, an item, yards, an option's key), at most six; *Options* turns the right side into the
  profile's options (key, label, kind, value, range; at most twelve); at the bottom *Save*, *Discard*, *Assign solo*,
  *Assign group* for the bot and the server's message. The editor works on a copy of the profile: a change marks it,
  switching profile or closing asks first, a save the server refuses selects the rule it names and scrolls to it, the
  template stays read-only.
- **Mock server and offline tests:** the mock answers the catalogue, `FIGHT_SAVE` (counts, fields, condition keys, an
  option a condition names), `FIGHT_DEL` and a copy of a profile; offline tests 794 of 794 (the editor's test: the
  catalogue, the locked template, a spell from the submenu, the moves over a divider and past an equal relevance, on
  and off, an option and a condition from the forms with a refused `$option`, save, a refused save that selects its
  rule, *Discard*, *New copy* and *Delete* with its confirmation).

**What was run** (dev instance `fight`, the probe guild Probeguild, **T2 (ga-probe)**): `connect` 5/5, `fight-tab`
30/30, the new extended scenario `fight-profile` 39/39 (report `_work\ga-probe\reports\20261125_185738`): the editor
opens on a copy of Probemage's template (Frost, 34 + 4 rules) with the condition catalogue and the bot's class spells,
a Fire Blast rule with the conditions "my mana % > 20" and "the profile's option is on burst" and the option *burst*
are saved and the server has them (`.ga admin fightprofile <id> show`: "cast Fire Blast (2136) on its target (19.0)
[my mana % > 20] [the profile's option is on burst]"), a condition the server does not know is refused with its
name and the rule is selected, *Discard*, *Assign solo* from the editor and the bot plays the profile (`fpn`), *New
copy* makes "Probe editor copy" with the rule, *Delete* asks and deletes it, no Lua error.

**Not tested:** a fight of the bot by an edited profile (the fight lines of FP-1 show that a stored profile steers a
bot, and the editor stores the same data), the forms typed by hand (the probe fills them from Lua), the list between
fights edited in the client, an item rule from the client, the box "Handled by guild rules" names the rules but not
yet the bot setting that steers each (§4.11).

### 10.1 After the owner's first test (2026-11-26)

the owner, testing the editor: [quote left out], [quote left out], [quote left out], [quote left out]when medium aoe[quote left out].

- **Triggers in plain language.** The grey text after a rule was the name of its trigger of mod-playerbots (a row brings
  it from its strategy: "medium aoe" fires with 3 or more enemies within 8 yd of the bot's target). The new
  `tools/gen_trigger_texts.py` reads mod-playerbots' source (the trigger contexts, the factories, the trigger classes and
  the macros of `Bot/Engine/AiObject.h` with the base each calls) and writes `src/FightTriggerTexts.h`: a text per
  trigger and class, all 504 triggers the class strategies use (a class with an `IsActive` of its own gets a text from
  the generator's hand list, read in its source; a trigger the generator has no text for shows as `check "<name>"`).
  Five rows of the class strategies name a trigger mod-playerbots has nowhere ("high threat" of the mage, "rune strike",
  "chaos bolt", "conflagrate", the priest's "boost"): the engine finds no trigger by that name and skips the row, so
  they read "never: mod-playerbots has no check by this name". Values of playerbots.conf
  (`{LowHealth}`, `{MediumMana}` ...) are filled in at run time. The server sends the texts of the bot's class with the
  catalogue (block `FTRIGS`). The lists show the text in place of "when <name>", and the selected rule's conditions
  list it first ("mod-playerbots: 3 or more enemies within 8 yd of its target"), with *X* to take it away (the rule then
  fires by its own conditions only; a row without either is tried whenever nothing more relevant is due). The condition
  "a trigger of mod-playerbots" reads the same way. Regenerate the header after a new pin of mod-playerbots.
- **Add spell by the spellbook.** `FSP` carries the spell's spellbook tab (the class skill line it is learned under,
  else "General"); the menu has a submenu per tab ("General" first, then the class's three as in the spellbook), each
  alphabetical, a tab of more than 25 in parts.
- **Rows of mod-playerbots and their spell.** A row's spell is found by the longest spell name its action begins with
  ("wind shear on enemy healer" casts Wind Shear), first among the spells the bot knows, then among those its class can
  learn; `FRULE^kn` says whether the bot knows it (`lv` from which level): such a row is grey with a grey icon and its
  tooltip says it waits until the spell is learned (Heroism and Fire Elemental Totem of a level-60 shaman).
- **Layout.** The window is 1000 x 600 and opaque (the bot detail no longer shows through), the rule names and their
  grey text are one line each with the whole of both in the row's tooltip, the profile list says "Enhancement, 27
  rules, 0 bots", the band line has two lines of its own, the rule's title one, and what the guild's own rules handle
  is one line whose tooltip has the list. The Combat sub-tab's read-only list shows the triggers in words the same way
  (it asks for the catalogue once per bot).
- **AIO profiles** came next, as the second row of templates (FP-4, §11).

## 11. FP-4: templates from the wowbot335 fight classes (2026-11-26)

The second row of templates (FC5, the owner: [quote left out]). AIO itself has no licence, so
nothing of it is copied: the templates come from the owner's own fight classes in the wowbot335 repository, and AIO is
only read to compare (below).

**The import.** `tools/gen_fight_templates.py` reads `profiles/fightclass/*_solo_*.json` (schema version 2: 24 fight
classes, every class; the `*_lowlevel` and `*_fallback` files are not imported) and the server's `Spell.dbc`, and
writes the world SQL `data/sql/db-world/base/ga_fight_template.sql` (tables `ga_fight_template`,
`ga_fight_template_rule`, `ga_fight_template_option`, the same columns as the guilds' `ga_fight_profile*`; the DB
updater applies it and re-applies it when it changes). Result: 24 templates, 457 rules, 102 options; regenerate after a
change of the fight classes. Each fight class becomes:

- a template of its class and talent tree (`spec` "Warrior_SoloArms" = Warrior, tree 0), named "<its name>
  (wowbot335)", with a note that says where it comes from, ids from 1000001 (the guilds' own profiles count from 1);
- its `spells` the fight list, its `oocSpells` the list between fights (a target there becomes the bot itself), each a
  spell rule (kind 2) of the spell id the fight class names; the server takes the first rank of the chain when it loads
  the table (a bot casts the highest rank it knows);
- its options the profile's options (`bool` a switch, a number with `isPercent` a percent, else a number; label,
  default, min and max as they are), at most 12;
- its order kept: a fight class's priority (2 to 130) becomes a relevance of the engine in the same order, 1 to 28
  (below the engine's movement band 30, so that reaching a target or stepping out of a ground effect still comes
  first); a spell on the bot itself under a health condition (Ice Block, Icebound Fortitude, Shield Wall) goes into the
  emergency band, 90 to 91.3, as mod-playerbots' own defensives do;
- `preventDoubleCast` the flag "not twice in a row", `forcedCooldownMs` the rule's interval in whole seconds;
- targets: `Target` the current target, `Player` the bot itself, `Tank` the group's tank, `EnemyAttackingMe` "an enemy
  that meets the conditions" with the condition "it targets me".

**Conditions** (a fight class's condition types and their counterparts in the catalogue): `ProfileOption` the option
check (`opt:<key>`); `PlayerHealthPercent`, `PlayerManaPercent`, `TargetHealthPercent` the health and mana checks with a
number or `$option`; `PlayerResource` rage, energy, runic power, combo points; `PlayerLevelAtLeast`; `SpellOffCooldown`;
`EnemiesTargetingPlayer` the bot's attackers; `HostileUnitsNearPlayer` / `HostileUnitsNearTarget` the enemies within N
yd of the bot or of its target; `PlayerHasAura` / `PlayerMissingAura` / `TargetHasMyAura` / `TargetMissingMyAura`, by
the aura's name (any rank); `PlayerAuraStacks`; `TargetCreatureType`; the flags elite, boss, casting, interruptible, in
combat, moving, channelling, in a group, pet alive. Three were new in the catalogue for it: `tenemies:N` (enemies
within N yd of the target), `tdebuffleft:<spell>` (seconds left of the bot's own aura on the target: "Corruption has
less than 3 s left") and `ttype:N` (the target's creature type). A list of auras ("has any of these") becomes one
condition per distinct name where the list asks for all of them; where it asks for any one of them only the first is
checked (the catalogue has no "or"): three rules, Ambush of the Subtlety rogue (Stealth, not the second aura) and Shadow
Bolt of the Affliction and Demonology warlocks (Shadow Trance, not Backlash). Nothing else was left out; at most 6
conditions and 60 rules per profile were never exceeded.

**Read-only templates.** The server loads them with the guilds' profiles (`FightProfileStore::LoadTemplates`, owner 0,
a bad rule is switched off and logged: none is), and lists them for every guild with the bots of their class:
`FPROF^tpl` and `FPROFILE_BEGIN^tpl` (docs/protocol.md §7.26). A template can be assigned to a bot as it is (solo or
group slot, *Copy to bots...*, `.ga admin fight ... assign`), and a bot's own value of one of its options can be set; it
cannot be saved, deleted or changed (`FIGHT_SAVE` and `FIGHT_DEL` refuse it, `.ga admin fightprofile <id> rule ...`
too). *New copy* (`FIGHT_COPY^from`) makes a profile of the guild out of it, which can be edited. In the addon the
templates stand in the Combat sub-tab's *Solo* and *Group* lists and in the editor's profile list with a lock and
"template, <tree>"; the editor shows a template read-only with the banner "Template (wowbot335), read-only" (no *Save*,
no *Delete*, no new rules; *New copy* and *Assign* are there).

**Compared with AIO** (the spell names of AIO's `Combat/<Class>/Solo<Tree>.cs` against each template; names only,
nothing copied; a one-off look on 2026-11-26). The differences in the spells:

- *Out of the module's way on purpose.* AIO casts interrupts (Counterspell, Strangulate, Wind Shear), the hunter's
  Disengage and Feign Death, cleanses (Remove Curse, Abolish Poison, Cleanse, Cure Toxins, Purify) and the druid's and
  paladin's self heals in its rotations. The module does all of that itself above every profile (§4.11: interrupts
  `Escape::StepInterrupt`, the escape's spells, self heal), so the templates need none of it; a guild can add them to a
  copy.
- *The buffs between fights.* The templates carry the class's buffs and stances in their second list (aspects, armours,
  auras, blessings, seals, Battle Shout, Horn of Winter, Mark of the Wild, Arcane Intellect); AIO keeps them out of its
  solo rotation files.
- *Real gaps of the fight classes*, worth a look in a copy or in wowbot335: the Feral template is cat only (AIO's solo
  feral also tanks in bear form: Maul, Swipe (Bear), Mangle (Bear), Demoralizing Roar), the warriors lack Charge and
  Victory Rush, the Protection paladin lacks Hand of Reckoning and Righteous Defense (its taunts), the Affliction warlock
  has no curse but Agony (AIO picks Curse of the Elements, of Weakness, of Tongues), the Blood death knight lacks Chains
  of Ice and Anti-Magic Shell, and the mages of AIO open with Fireball where the Frost template does not.
- AIO has no solo rotation for the Subtlety rogue; its "SoloAssassination" plays the combat rogue's cooldowns.

**Not done by this import**: healer and tank templates (the wowbot335 fight classes are damage specs only: no Holy,
Discipline, Restoration, Protection warrior); they came with §12, written from the fight class research. Pet actions
are still open.

**Tested** (dev instance `fight`, SOAP): the DB updater is off there, the SQL was applied by hand; at the start "loaded
N fight profiles, 24 templates (wowbot335)", no rule switched off; `.ga admin fight Probemage list` lists the three
mage templates; Probemage (Frost) on the Frost template (#1000019) on a farm order among the Bloodsail of Stranglethorn fought four times in seven minutes by it ("fought 54 s by its fight profile "Mage Solo Frost (wowbot335)": 18 casts (Frostbolt 9, Frost Nova 2, Ice Barrier 2, Blizzard 1, Cold Snap 1, Cone of Cold 1, ...)"; Fire Blast, Mana Shield and the module's Counterspell in the others), three won and one left (four foes); a change of a template by command is refused with its reason. ga-probe `fight-profile` checks it in the client (step 6).

## 12. The template of its spec by default; tank and healer templates; the research's corrections (2026-11-27)

the owner: [quote left out], then [quote left out] (it had not been) and [quote left out]. The research is the vault's `docs/World of Warcraft/wowbot/fightclass-research/`
(`<class>.md`, one per class: every spec's rotation at levels 1 to 80, solo and group, tank and healer specs included).

**The default (FC12).** A bot with nothing of its own in the slot it plays (`FP::Keep`) plays the template of its
class and talent tree (`FP::DefaultFor`):

- *its role* comes from its talents (`PlayerbotAI::IsTank` / `IsHeal` by the talents only, not by its strategies,
  which a profile replaces);
- *the group slot* (a dungeon or raid map, a dungeon run): the template of that role (tank, healer, else the damage
  one), or mod-playerbots' rotation where there is none;
- *the solo slot*: the damage (solo) template of its tree first, else the one of its role (a healer that levels alone
  with healing talents);
- *its own profile of another tree*: a tank or healer whose role talents are on only in a dungeon run (its second
  talent spec, docs/dungeon-roles.md) has its role profile in a slot but levels on its damage tree; while its talents
  are of the other tree it plays the template of the tree it has (before: mod-playerbots' rotation), and in the run its
  own profile again; `.ga fight` names it;
- *the bot setting* `Fight.TemplatesByDefault` (id 136, category Combat and rest, per guild and per bot, default on;
  conf `GuildAdventure.Fight.TemplatesByDefault`): off, an empty slot plays mod-playerbots' rotation as before;
- the fallback after 10 s without an action (FC9) still goes to mod-playerbots' rotation.

`.ga fight <bot>` says "solo slot: by default the template #N ... of its class and talent tree", the activity log
"plays fight profile #N ... (solo slot, by default: nothing of its own in it, ...)"; GADV `BOT^fpid` (the profile it
plays) and `BOT^fpd` (by default), `FPROFILES_BEGIN^dflt` / `dfltg` (the template each slot would take), `FPROF^tr`
(a template's role: 0 solo, 1 tank, 2 heal; docs/protocol.md §7.26). The Combat sub-tab's status line says "by
default: the template of its spec", and its slot lists read "Nothing of its own (the template of its spec: ...)".

**Tank and healer templates** (`tools/fightclass/<stem>.json`, written from the research in the wowbot335 schema with
three fields more: `tree`, `role`, and a `relevance` per spell, since a healer's order is the engine's heal bands, not a
list of priorities): Protection warrior, Protection paladin, Feral (bear) druid and Blood death knight as tanks; Holy
paladin, Holy and Discipline priest, Restoration shaman and Restoration druid as healers. What they do:

- *tanks*: the stance or form in a band of its own (40), the taunt on an enemy that does not target the bot, in a group
  only (30, `!ttargetsme`, `ingroup`), the defensives in the emergency band, then threat: Shield Block, Revenge, Shield
  Slam, Thunder Clap, Devastate (Sunder Armor while Devastate is not known); Avenger's Shield, Holy Shield, Hammer of the
  Righteous, Consecration from two foes; Mangle (Bear), Swipe (Bear), Lacerate, Maul; Death Grip on an enemy 8 yd off
  that is not on it, Icy Touch and Plague Strike by their diseases, Death and Decay, Heart Strike;
- *healers*: the heals on the lowest member or the tank by that target's health (`thp` is evaluated on the rule's
  target) in the engine's heal bands (light 10, medium 20, critical 30, emergency 90): Flash Heal, Flash of Light,
  Lesser Healing Wave, Regrowth quick; Greater Heal, Holy Light, Healing Wave, Healing Touch big; Renew, Rejuvenation,
  Riptide, Lifebloom over time; group heals by `membersbelow:<health><op><count>` (Prayer of Healing, Circle of Healing,
  Chain Heal, Wild Growth); the tank's shields (Power Word: Shield without Weakened Soul, Earth Shield, Beacon of
  Light); cooldowns (Pain Suppression, Guardian Spirit, Nature's Swiftness, Divine Favor, Tree of Life in a dungeon,
  Innervate, Mana Tide); damage spells only outside dungeons (`!indungeon`: a healer spends no mana on damage in a
  dungeon, DB5); their buffs, auras, seals, shields and weapon imbues between fights.

Spells a template names above level 60 (Penance, Riptide, Beacon of Light, Hand of Reckoning, Shield of Righteousness,
Death Strike ...) do not fire for a bot that does not know them; where a lower spell does the same job it has a rule
under `!known:<id>` (Bear Form while Dire Bear Form is not known, Lesser Heal and Heal while Flash Heal and Greater Heal
are not, Judgement of Light before Judgement of Wisdom).

**The DPS templates against the research.** Each wowbot335 fight class keeps its file; a correction is a
`tools/fightclass/<stem>.patch.json` beside it (`remove`, `conditions` to add, `set`, `add`, per list; a spell is
named by its name and priority) that the generator applies; the template's note names it. 16 files; what was wrong:

- *a filler or finisher only from a level above 60*: Arcane mages had nothing below Arcane Blast (64): Arcane Missiles,
  then Frostbolt while Arcane Blast is not known; Destruction warlocks nothing below Incinerate (64): Shadow Bolt;
  Assassination rogues no finisher below Envenom (62): Eviscerate; Subtlety rogues no builder before Hemorrhage (a
  talent of level 30): Sinister Strike; Retribution paladins Exorcism only on an Art of War proc: without the talent
  when mana allows; shadow priests Mind Flay only in Shadowform (level 40): before it too;
- *casts from the wrong side*: Shred, Backstab, Ambush and Garrote only behind the target (`behind`);
- *casts again and again*: Mend Pet and Health Funnel only for a hurt pet (`pethp`); the warlocks' Life Tap between
  fights is gone (it is for a glyph the bots do not have, and tapped them down to 70 % health after every fight);
  Searing and Magma Totem only with no fire totem down, the shamans' weapon imbues only on a main hand without one
  (mod-playerbots' triggers "no fire totem" and "main hand weapon no imbue" as conditions, `trig:<name>`);
- *missing*: the warriors' Charge (Battle Stance, 8-25 yd, out of combat; relevance 21.4, above mod-playerbots' "reach
  melee" at 21) and Victory Rush, Fury's Intercept in Berserker Stance, Retribution's Consecration from two foes
  (the owner's rule: no Consecration against a single target).

The rest matched the research (death knights, balance, cat apart from Shred, the hunters' order, fire and frost mages,
combat rogue, the shamans' order).

**A second look, after a live fight (2026-11-27).** The maintainers relayed a Demonology warlock's fight line "fought
30 s ...: 6 casts (Corruption 2, Curse of Agony 2, Healing Potion 1, Immolate 1)" with no Shadow Bolt. That fight was
against four creatures, which the warlock left after about 10 s (it died on the run); the 30 s are the whole fight with
the run, and over the live log since the templates went live Shadow Bolt was cast in 297 of 308 of its fights. Not a
broken template, but the look at every template's lowest rules found gaps where nothing is cast:

- *Demonology below 20 % target health*: its filler Shadow Bolt had `thp>20`, and Demonology has no Drain Soul and
  Soul Fire only with Decimation (from level 50): a dying target got no spell. The filler has no health condition now
  (warlock.md 2d: "Shadow Bolt is the standard filler");
- *death knights below 15 % target health*: their strikes need both diseases and the diseases `thp>15`; a target that
  fell below 15 % before they were on got nothing. Blood Strike as the last rule of all three trees (deathknight.md:
  "Blood Strike (Blood rune filler)");
- *hunters in melee*: a template takes over the rows of the strategies it replaces, and mod-playerbots' hunter strategy
  casts Mongoose Bite, Raptor Strike and Wing Clip on "enemy within melee"; without them a hunter in the dead zone cast
  nothing while its pet fought. The three hunter templates have them (5 yd; Wing Clip not on elites);
- *Consecration against one foe*: the Retribution template's own Consecration on an elite and the Protection paladin's
  (solo) Consecration were single-target casts: both from two foes now;
- *Beast Mastery*: its Mend Pet between fights needs a hurt pet too, and the Aspect of the Hawk the first round had added
  was there already (the fight class has it).

33 templates, 654 rules. Files: `warlock_solo_demonology`, `deathknight_solo_{blood,frost,unholy}` (new),
`hunter_solo_{beastmastery,marksmanship,survival}`, `paladin_solo_retribution`, `paladin_solo_protection` (new).

**Generator and storage.** `tools/gen_fight_templates.py`: the wowbot335 files with their patches and the own files;
the world tables get a column `role` (0 solo, 1 tank, 2 heal); ids stay stable across generations (an existing template
keeps its id, new ones follow: 1000025-1000033); new in the import: `PetHealthPercent` (`pethp`), `TankHealthPercent`
(`tankhp`), `TargetDistance` (`tdist`), `SpellKnown` (`known:<id>`), `Trigger` (`trig:<name>`), `MembersBelow`
(`membersbelow`), `TargetHasAura` (`taura`), the flags `TargetTargetsMe`, `InDungeon`, `PlayerIsBehindTarget`, and the
target `EnemyNotOnMe`. Result: 33 templates (24 solo, 4 tank, 5 heal), 654 rules, 102 options; the load line "loaded N
fight profiles, 33 templates (solo 24, tank 4, heal 5)".

**Install**: the world SQL `ga_fight_template.sql` (its tables are dropped and made anew with the column; the DB updater
applies it), the module build (no new source file), the conf key (the default is fine), the addon (no TOC change).

**A hunter without Auto Shot** (2026-11-28; the owner: [quote left out]). A level-1 night elf
hunter recruited on the integration server picked up its quests, turned to its first Young Nightsaber and stood in
the combat engine with nothing to do (`.ga bot plan`: "combat engine; last action: "; no fight began, so the
fallback of FC9 never came). It played the template of its tree by default (FC12), "Hunter Solo Beast Mastery
(wowbot335)": the profile stands in for mod-playerbots' strategies aoe, bm and cc, and "auto shot", the default
action of the hunter's three tree strategies (relevance 5.1), was one of the rows the profile owned and so left
out; no template has a rule for it. At level 1 that left Raptor Strike (within 5 yd, never walked to) and nothing
else; at every level it cost the hunter its auto shot. "auto shot" is a row the profile does not own now, as the
other auto attacks (`IsModuleRow`): it goes back into the rotation from the bot's own strategies, with its own
relevance, under every hunter profile and template.

*The swings for a spell on next swing.* That was half of it: a new level-1 hunter of the probe guild on the same
template still stood beside a Young Thistle Boar 3 yd away, inside Auto Shot's least range, with "0 casts", until the
module ended the fight for it and FC9 put it back on the default, on which it won its next four fights. Its rule
"Raptor Strike, within 5 yd" fired and never landed: Raptor Strike, like Heroic Strike, Cleave and Maul, is a spell
"on next swing", cast only with the next swing of the auto attack, and a hunter's strategies start that auto attack
only through the prerequisite "melee" in the action node of mod-playerbots' own "raptor strike"; a rule's action of
the module's own (`RuleSpellAction`) has no node and so no prerequisite. A rule with such a spell starts the auto
attack on its own target first when that stands in melee reach (`RuleSpellAction::SwingAt`).

*A template the bot cannot play yet.* A player of the public release reported, and the owner confirmed, that a level-1
(undead) mage did not level under the templates and did with `Fight.TemplatesByDefault` off. Below level 10
mod-playerbots counts a mage's talent tree as Frost (`AiFactory::GetPlayerSpecTab`), so the bot played "Mage Solo Frost
(wowbot335)", whose first spell against a foe is Frostbolt (level 4); a level-1 mage has Fireball, which the template
has not, and no wand. mod-playerbots' Frost rotation ends in Shoot and Fireball (its default actions 5.1 and 5.0) and
plays at level 1. The first spell against a foe of each template (Spell.dbc's spell level of the rule's first rank):

| Template (below level 10 the tree mod-playerbots counts) | First spell against a foe |
|---|---|
| Warrior Arms, Fury; tank | Heroic Strike, 1 (on next swing: swings since the commit before) |
| Paladin Retribution, Protection (solo) | Hammer of Justice, 8 (Judgement of Wisdom 12, Exorcism 20); the paladin swings before |
| Paladin tank, Holy healer | Judgement of Light, 4 |
| Hunter (three trees) | Raptor Strike, 1 (Auto Shot is a row of its own) |
| Rogue (three trees) | Sinister Strike, 1 |
| Priest Shadow, the healer templates | Smite, Shoot, 1 |
| Shaman Elemental, Enhancement, Restoration | Lightning Bolt, 1 |
| Mage Frost, Arcane | Frostbolt, 4 |
| Mage Fire | Fireball, 1 |
| Warlock (three trees) | Shadow Bolt, 1 |
| Druid Balance, Restoration | Wrath, 1 |
| Druid Feral; bear tank | Faerie Fire (Feral), 18; Growl, 10 |
| Death Knight | 55 |

A template is played by default now only when the bot knows a spell of its fight list on its target or an enemy
(`PlayableTemplate`); below that level it plays mod-playerbots' rotation (`plays the default of mod-playerbots, not the
template ...: it knows none of the template's spells against a foe yet`, once per level), and from it the template.
So a Frost or Arcane mage plays mod-playerbots' rotation at levels 1 to 3, a Retribution paladin at 1 to 7, a feral
druid at 10 to 17. A profile the guild assigns is played as it is.

