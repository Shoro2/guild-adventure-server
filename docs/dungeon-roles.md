# Dungeon roles

How a guild bot becomes the tank or the healer of a dungeon group although it levels as a damage dealer: the role
the guild leader gives it, the talents it changes to for a run, the dungeon set it collects while it levels and the
role store that set lies in. Module: `DungeonRoles.h/.cpp` (`RoleService`), `Protocol/GadvRoles.cpp`; addon: the bot
detail's *Dungeon role* sub-tab (`UI/Talents.lua`). Built 2026-11-13.

## What the owner asked for

> [quote left out]

So: a role per bot, talents switched for the run, role gear collected while levelling, and a store of the bot's own
for that gear.

## The parts

### The role

The guild leader gives a bot the dungeon role *tank* or *healer*, or none. A class can take a role when a talent
template of the class has it (`ga_spec_template.role`): warrior, paladin, death knight and druid can tank; priest,
paladin, shaman and druid can heal. A bot without a role goes into a dungeon as it levels (its group role is then
the role of the talent template it levels with, as before). The guild leader takes no role.

Stored in `ga_bot_role` (characters): `role`, `template_id` (the talent template of the role, 0 = the default of the
class for it: its "Dungeon and raid" template with that role; a priest's default is Holy), `worn` (1 while the
dungeon set is on).

Set in the addon (bot detail, *Dungeon role*), over GADV with `ROLE_SET`, or with `.ga admin role`.

### Talents: the second spec

A bot with a role uses the game's dual talent specialization: spec 1 holds the talents it levels with (its talent
template, docs/specs.md), spec 2 the talents of its role. The second spec is given by the module when the bot first
needs it, free and from level 10 (the game sells it for gold at level 40). `RoleService::EnterDungeonMode` activates
spec 2 and spends its points along the role's template (`SpecService::ApplyTemplate`: talents that do not fit are
reset first, free); `LeaveDungeonMode` activates spec 1 again. Mana and energy, which the game empties on a change
of spec, are put back. While spec 2 is active `SpecService` plays the bot by the role's template (a level-up during
the run spends its point there); mod-playerbots takes its strategies (tank, healer, the rotation) from the active
talents (`BotDriver::ResetStrategies`).

Why dual spec and not "reset and learn again": both talent sets stay as they are, the change is two calls, action
bars are kept per spec (they matter when the owner takes the bot over), and the state after a server stop is simply
"which spec is active".

`GuildAdventure.DungeonRoles.RespecCost` (copper, default 0) makes the change to the role's talents cost the bot
money; the change back is always free.

### The dungeon set: what a role wants

A piece of equipment is rated for a role by `BotDriver::RoleScore`: mod-playerbots' reading of the item's stats
(`StatsCollector`, with random properties) times the stat weights of the class's tank or healer tree (since
2026-11-26 those of the wowbot335 profiles, docs/orders.md "Stat weights from wowbot335"; before, mod-playerbots': stamina,
armour, defense, dodge, parry, block for a plate tank; stamina, agility, strength, armour for a bear; intellect,
spirit, healing, mana regeneration for a healer). This is not `BotDriver::ItemScore`: that one rates by the talents
the bot has at the moment, so it cannot say what a levelling Arms warrior's tank set should hold.

A set has one piece per place: head, neck, shoulders, back, chest, wrists, hands, waist, legs, feet, ranged, two
rings, and three places for the hands: a one-hand weapon, a two-hand weapon, and what the other hand holds (shield
or held item). In a run the bot holds the better of "two-hand weapon" and "one-hand weapon plus other hand". A
warrior or paladin tank collects no two-hand weapon and only a shield for the other hand, and changes its weapon
only once the set has a shield (a one-hand weapon alone is no gain); a death knight tank only two-hand weapons. Trinkets are not collected (their worth is in effects no score reads).

A piece **improves the dungeon set** (`RoleService::IsRoleUpgrade`) when the bot can wear it now (class, level and
the armour or weapon proficiency: the core's own check for a kind of item leaves the proficiency out, and a priest
collected mail in the first build) and its worth for the role is clearly above (5 % and half a point) what the set has for that place: the piece already collected, else
the levelling piece the bot wears there. So a place where the levelling piece is the best the bot has for the role
too gets no piece of its own: the set is the levelling set plus the pieces that are better for the role. A piece it
wears only once (unique-equipped, or carried at most once) has to beat the copy it has (worn or stored) **and** be
one of the best of its place, as the rule that keeps the store's pieces counts them; of such a piece only the best
copy takes a place (two copies of a unique ring do not fill both fingers). Before 2026-11-14 the first test alone
held for it: Duduheal wore one Aquamarine Signet, its store held two better rings, and two more Signets with better
suffixes went into the store and out again every minute.

Where pieces come from, while the bot levels:

- **Loot.** The loot filter keeps such a piece with the bot ("for its dungeon role") whatever the guild's rules,
  the junk rule and the default say, as it keeps what the bot uses itself; the bag sweeps and the "room in the bags"
  step leave it alone. Within a second or two the bot's next planner tick moves it into the role store.
- **Quest rewards.** Where the module hands a quest in (squad members, and the leader's hand-in after ten seconds at
  the ender): a choice that is an upgrade for levelling comes first; when none is, the choice that improves the
  dungeon set most. mod-playerbots' own turn-in picks by its own rating (the levelling talents).
- **The guild storage.** With the supply step (once a minute): a piece that improves the dungeon set and that no
  guild character in the world would wear for levelling is taken out, at most two per look
  (`GuildAdventure.DungeonRoles.FromStorage`). Levelling comes first, between tanks and healers who looks first.
  The activity log names every piece (`took from the guild storage: for its dungeon set (into its role store): ...`).
- **Gear made for the guild** (bot setting `Professions.CraftGear`, off by default; docs/orders.md "Gear made for
  the guild"; the owner, 2026-11-13: [quote left out]). A crafter that finds no
  piece somebody would level in makes the best piece that improves a dungeon set. Its own goes from the bags into
  its role store; another character's goes through the guild storage and the rule above.
  White gear a dungeon set would take is no junk, and an enchanter does not disenchant it for a craft order.
- **Dungeon runs.** In a run the bot has the talents of its role, so mod-playerbots rolls for and puts on what
  improves the set it wears.

Levelling always comes first: a piece mod-playerbots would put on for the levelling talents is left to it.

**The dungeon set worn while levelling** (the owner, 2026-11-14, on a decision card of the maintainers: "17 of your 30
bots wear gear far below their level. Where should better gear come from first?", his choice [quote left out]). A piece went into the role store when it was no levelling upgrade at the moment it was looked at,
and mod-playerbots never rates a necklace or ring as one; so the stores held better pieces than the bots wore
(Wartank, level 46, levelled with a staff of item level 37 and a necklace of 18 while its store held a sword of 41,
a shield of 48 and a necklace of 41). Now the planner's step, once a minute while the bot levels (its first talent
spec, no run, the set not on), looks at the store's pieces with the levelling talents: an armour piece or a weapon
goes on when mod-playerbots itself rates it as an upgrade over what is worn (`BotDriver::IsUpgrade`: its threshold of
10 %, its rule for the armour kind of the class and the stats it uses), the best of a place by the weights the
levelling gear is rated with (`BotDriver::ItemScore`); a necklace, ring or relic by those weights, clearly better (5 %
and half a point) or on a bare place whatever its stats (mod-playerbots rates no jewellery as an upgrade). The first
build rated everything by the weights alone, and mod-playerbots took the replaced pieces back from the bags at once:
a healer's Embersilk pieces were "bad equipment" for a priest by its stat rule, and a one-hand blade and an orb went
on for a staff that mod-playerbots, comparing the staff with the blade alone, put back within the minute (every
minute after). What it replaces goes into
the bags: the role store takes it back when the set wants it (`Collect`), the loot filter the rest (the guild
storage, or the junk rule). The pieces stay part of the dungeon set: a run finds them on the bot, a place keeps them
while nothing in the store is better for the role, and after the run they are what the bot wears. The hands
(`DungeonRoles.WearForLevelling` 2, the default; 1 armour, jewellery and relics only; 0 never; a bot setting per
guild and per bot): the store's weapons against what is worn, piece by piece as mod-playerbots rates them: a sword
replaces a two-hand weapon only when the sword alone is the upgrade, and a shield then joins it. A worn piece of the set is no longer listed in the role store in the addon;
`.ga role` shows the gear level of the set with it. Log line: `wears pieces of its dungeon set (tank) while it levels:
they are better for its levelling talents than what it wore: Protective Pavise (off hand), Kodobone Necklace (neck,
for Malachite Pendant)`.

### The role store

`ga_bot_role_gear` (characters): the bot's own store outside its bags, one row per piece with everything
`item_instance` holds about it (entry, random property, enchantments, charges, durability, flags, creator), so a piece
comes back as it went in, soulbound pieces included. Two kinds of rows:

- kind 1, a piece of the dungeon set: in the store while the bot wears its levelling set;
- kind 2, a levelling piece put aside while the dungeon set is on, with the equipment slot it came from (a row
  without an item says "this slot was empty").

A soulbound piece that was never worn (a dungeon drop, or a bind-on-pickup piece the bot took out of the guild
storage, which accepts such pieces since the owner's ruling on DL1) keeps that fact in the store (`slot` 254): when the
set lets it go again, it comes back into the bags with the guild storage's never-worn mark
(`StorageService::MarkUnworn`), so it can go into the guild storage for another character. A piece that was worn in
a run has lost the mark for good.

Moves, all by `RoleService`:

| Move | When |
|---|---|
| bags → store (kind 1) | the planner's step finds a piece in the bags that improves the dungeon set |
| guild storage → bags → store | the supply step (above) |
| store → bags | the set no longer needs a piece: a better one came, the levelling piece there is better now, the role changed or was taken away. In the bags the loot filter treats it like any item (guild storage, or the junk rule) |
| body → store (kind 2), store → body | `EnterDungeonMode`: per place with a piece of the dungeon set, the levelling piece goes into the store and the dungeon piece on |
| body → store (kind 1), store → body | `LeaveDungeonMode`: the reverse |

**Nothing is lost on a server stop.** A move destroys the item in one place and creates it in the other; each batch
of moves is written in one database transaction that holds the character's inventory
(`Player::SaveInventoryAndGoldToDB`), the bot's row in `ga_bot_role` and its whole store. A server that stops before
the transaction is written comes back with the state before the move, afterwards with the state after it: a piece
is in exactly one place. (The same idea as `StorageService::Touch` for the guild storage.)

A levelling piece that stays on in a run (no dungeon piece for its place) and is replaced by a dungeon drop in the
run is remembered by its item guid: the loot filter keeps it in the bags, and after the run it goes back on and the
drop joins the dungeon set.

### A dungeon run

`OrderService::RunDungeon` (docs/orders.md "Dungeon runs"): at the entrance, when the group is together, alive and
out of combat, every member with a role gets `EnterDungeonMode`; `EndDungeonRun` gives everybody `LeaveDungeonMode`.
A member that cannot change at that moment does it at its next quiet moment: `RoleService::Step` runs every planner
tick and puts the levelling set back on whenever the dungeon set is on without a run. That is also what happens
after a server stop in the middle of a run.

```
activity: Tankwar gets ready for the dungeon as tank: talents "Dungeon and raid: Protection" (0/0/10); dungeon set on: Heavy Mace, Large Round Shield, Light Mail Belt; into its role store: Battle Axe; gear level 17
activity: Tankwar is back from the dungeon: talents for levelling again (10/0/0); levelling set on again: Battle Axe; into its role store: Heavy Mace, Large Round Shield, Light Mail Belt
activity: Tankwar put into its role store for its dungeon set (tank): Worn Mail Bracers
activity: Tankwar took out of its role store what its dungeon set no longer needs: Light Mail Belt
```

### Readiness

`RoleService::RoleGearLevel`: the gear level (docs/orders.md "Readiness") of the set the bot would wear in a run,
and from it a readiness per dungeon. `.ga readiness <bot>` and `.ga role <bot>` print it; the BOT record carries it
(`drgl`, `drrdy`); the Orders sub-tab's dungeon text names each tank and healer of the squad with the readiness of
its dungeon set, and says when the tank does not lead the squad (the group walks behind its leader). The run's
first log line carries "(as tank in its dungeon set N%)" per member with a role. The order is **not refused** for a
weak dungeon set: the 50 % rule of the dungeon order still looks at level and worn gear, as before.

## Commands

| Command | |
|---|---|
| `.ga role <character>` | GM, console, SOAP: the role, the talents of the role, which set is on, the gear level in the dungeon set, the readiness per dungeon in it, the role store piece by piece |
| `.ga admin role <bot> <none \| tank \| healer> [<talent template id>]` | administrator, console, SOAP: sets the role |
| `.ga admin roleset <bot> <on \| off \| look>` | test aid: puts the dungeon set and the role's talents on as a run does, changes back, or makes the bot look at its bags now |
| `.ga admin rolescore <bot> <item> [<random property>]` | what a kind of item is worth as tank and as healer for the bot, whether its dungeon set would take it, and whether it is a levelling upgrade |

## Settings

`GuildAdventure.DungeonRoles.Enable` (1), `.DungeonRoles.FromStorage` (1), `.DungeonRoles.RespecCost` (0 copper),
`.DungeonRoles.WearForLevelling` (2; a bot setting, "Wear the dungeon set while levelling" in the Gear category).

## Protocol

docs/protocol.md "Dungeon roles": `BOT^dr/drs/drt/drn/drw/drp/drgl/drrdy`, intent `ROLE_SET`, block `ROLEGEAR`
(`GET^what=rolegear^g=<bot>`).

## Status (2026-11-13)

T2 on the `roles` dev instance (SOAP, no client) and by ga-probe (`dungeon-role` 20/20, `addon-ui` 41/41): the design log
section 0 "What works", row "Dungeon roles", has the evidence line by line. In short: role set and refused; loot and
storage pieces into the role store, levelling first; talents and set on and back with random properties and
durability kept; a server killed six seconds after a change loses nothing and the bot changes back by itself; a
Deadmines run at level 20-21 with a warrior tank and a priest healer changed at the entrance, fought 18 minutes
with one death, and changed back when the run was over. Not tested: a quest reward chosen for the role, the
respec cost, paladins and druids in a role.

## Choices taken without the owner

the design log section 7 "Open", DR1 to DR8.

## Open

- mod-playerbots' own quest turn-in picks the reward by the levelling talents; only the module's hand-ins know the
  role.
- In a run a bot with a role rolls for what improves the set it wears; a drop that would be a levelling upgrade for
  it is passed on (it reaches the guild storage when it is not soulbound).
- A piece that leaves the role store soulbound and unwanted stays in the bags like any soulbound piece nobody
  wants (the design log section 7, DL1).
- One role per bot. A druid or paladin that should tank and heal needs a second set; the store's rows already
  allow it, the rules do not.
- The gear level of a dungeon set does not count a tank's shield (the readiness table has no off-hand place).
- A tank that is not the leader of its squad walks behind the leader; the party builder should put the tank in
  front (next K6 stage).
