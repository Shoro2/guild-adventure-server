# Talents, roles and combat

## Talent templates

Each bot spends its talent points by a **talent template**, in its **Talents** sub-tab:

- **Class default** (where you start): a levelling template of its class.
- **Built-in templates** of its class for levelling, dungeons and raids, and PvP. Each takes the talents its spec is
  played with as early as their tier allows.
- **Your guild's own templates:** play a character, set its talents as you like and press **Save as template**.
- **Manual:** the bot keeps the talents it has and spends nothing by itself.

**Assign** resets the bot's talents for free and spends its points by the new template at once (it asks first). From then
on the bot spends a new point at every level-up. Its fighting follows its talent tree by itself.

## Class abilities and pets

Abilities that the game hands out through class quests come by themselves when a bot reaches the level: a warrior's
stances, a warlock's demons, a druid's bear form, a shaman's totems, a paladin's resurrection. A hunter gets its first
pet at level 10 and a strong one at 60 (an exotic Devilsaur if it has the Beast Mastery talent); its pet spends its own
talent points. A rogue keeps a stock of poisons from level 20, out of the guild storage or bought at a poison vendor. A
death knight puts runes on its weapons.

## Dungeon roles

Bots level as damage dealers. For dungeons and raids you can give a bot the role **tank** or **healer** (as far as its
class can), in its **Dungeon role** sub-tab:

- It gets a second talent spec for the role, by a template you choose. The change is free and happens at the dungeon's
  entrance; after the run it changes back.
- While it levels it collects a **dungeon set** for the role: loot, quest rewards and pieces of the guild storage that
  are better for the role than what it has. The set is kept in a role store of its own, outside its bags; the sub-tab
  lists it piece by piece with its gear level and readiness.
- At the entrance it puts the set on, afterwards it puts its levelling gear back on.
- Pieces of the set that are better for levelling than what it wears it also wears while it levels.

The Roster's role column shows the role in bright letters when you gave one, in grey when it only comes from the
talents.

## Fight profiles

How a bot fights, which spell it casts when, is its **fight profile**. Its **Combat** sub-tab shows what it fights by and
why.

- **By default** a bot plays the template of its class and talent tree: a damage rotation when it fights alone, the tank
  or healer rotation of its role in a dungeon or raid.
- Every bot has two slots: **Solo** (while it levels) and **Group** (in a dungeon or raid). Put a profile into each, or
  leave a slot to its default.
- **Copy as new profile** makes an editable copy of a template for your guild; **Copy to bots...** gives this bot's
  profiles (and its own option values) to other bots of its class.
- **Options for this bot:** a profile can have options (a health threshold, a switch for a burst spell); each bot can
  have its own values.

**Edit...** opens the fight profile editor:

- the profiles of the class on the left (templates are locked; **New copy**, **Delete**);
- the two lists **Fight** and **Between fights** (buffs out of combat), in the order the bot tries them, with dividers
  for the priority bands (emergency, dispel, interrupt, high, normal, rotation);
- per rule: **Up**, **Down**, **On / off**, **Remove**, and **Add spell** (the class's spells) or **Add item** (a usable
  item, a trinket);
- for the selected rule: its target (its current target, itself, its pet, the weakest member of its group, the tank, an
  enemy that meets the conditions), its band, an interval, "once per target", "not twice in a row", and up to six
  conditions in words (health, mana, auras, how many enemies, the group, the profile's options);
- the profile's options; **Save** (the server checks the profile and marks a rule it refuses), **Discard**,
  **Assign solo** / **Assign group**.

Limits: 20 profiles per class, 60 rules per profile, six conditions per rule. If a profile makes a bot do nothing for 10
seconds in a fight, it falls back to the default rotation and you get a message.

Some things stay with the bots' own rules whatever the profile says: healing themselves in an emergency, potions and
bandages, leaving a fight they would lose, careful pulls, rest, pets, poisons, and the dungeon group's rules. The editor
shows them as "handled by guild rules".
