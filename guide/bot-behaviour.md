# Bot behaviour

Your bots look after themselves. This page says what they do on their own and how you change it.

## What bots do on their own

- **Rest:** out of a fight they rest below 65 % health or 40 % mana and go on at 85 % / 75 %, eating only for health and
  drinking only for mana. They rest at a quiet spot, never in the water; in a dungeon where they stand.
- **Food and drink:** from level 8 they buy food and drink at vendors; first they take one piece at a time out of the
  guild storage. Mages conjure their own. A bot without anything to drink goes to a vendor.
- **Care in the open world:** they choose quest spots and grinding spots by what a fight there would cost them, keep
  away from camps and strong creatures (elites, rares, creatures four or more levels above them, groups they cannot take),
  and remember where they died. A bot that dies several times in a short while plays it safe for a while.
- **Pulls:** a creature that stands with others is pulled carefully, with a ranged spell or weapon to the bot's side,
  from the side away from its friends.
- **In a fight:** they interrupt spellcasts, heal themselves when their class can, use a potion or a bandage when it gets
  close, step out of poison and fire on the ground, and leave a fight they would lose and run until the creatures give
  up.
- **Upkeep:** they repair and sell at a hub after handing in quests, or at once when their bags are full or their gear is
  breaking; they put on better gear from loot, the guild storage and nearby vendors (the guild bank helps pay); they
  learn their class spells in place at every level.
- **Guild chat:** at most one line of the bots' guild chat reaches you every 30 to 60 seconds.

## Bot settings

**Settings**, **Bot behaviour** lists every setting you can change, in categories:

| Category | What it is about |
|---|---|
| Combat and rest | When bots rest, when they leave a fight, interrupts, self-healing, potions, fight profile defaults |
| Danger and deaths | How careful they choose their spots, what happens after deaths |
| Questing and travel | Which quests they take (escorts, quest events, grey quests, quest partners), roads and walks |
| Grinding | Whether and where they grind when no quest is left |
| Farm orders | What a bot with a farm order hunts and where |
| Vendors and provisions | Food, drink, ammunition and vendor trips |
| Gear and guild gold | Where bots get gear, how much of the guild's gold they may spend, the guild tax, who pays for unlocks |
| Professions | Gathering, crafting at hubs, what crafters make for the guild |
| Class and training | Spells, abilities, pets, poisons and runes the bots get on their own |
| Guild chat | How often the bots' guild chat reaches you |
| Loot | Junk, worn gear, spare quest items, containers, what goes into the storage |
| Dungeon runs | How a dungeon group goes its way, pulls and waits before a boss |

- The search box searches all of them; **Changed only** shows what you changed; **Show all options** adds the finer ones.
- Each row has a checkbox, a choice or a number, says where its value comes from (the server's default, your guild's
  value, the bot's own value), and has **Reset**. **Reset category** and **Reset all** go back to the defaults.
- A value you set here holds for the whole guild.
- Most settings can also be set for one bot: its **Behaviour** sub-tab in the bot's details has the same panel; a bot's
  own value wins over the guild's. Guild-wide matters (the guild tax, guild chat, the budget per item) can only be set
  for the guild.

The server's operator sets the defaults for every guild; your values override them, within the range each setting
allows.
