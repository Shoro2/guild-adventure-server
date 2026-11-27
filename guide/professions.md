# Professions

You decide which professions each bot has; the bots do the work. Profession plans unlock at renown level 2.

## The profession plan

In a bot's **Professions** sub-tab choose two primary professions, each with a target rank, and the secondary ones
(Cooking, First Aid, Fishing), then **Apply plan**. The tab also shows the bot's skill bars and which professions your
guild already covers. A bot without a plan learns no profession.

The bot learns the professions of its plan, their ranks and the recipes a trainer would teach it where it stands, for
free, as its level allows, and gets the tools it needs (a mining pick, a skinning knife, a fishing pole).

## While it levels

A bot on the Autopilot raises its professions on the way:

- **Gathering:** it mines, picks herbs and skins its kills as it passes them. If its gathering skill falls well behind
  what its zone has (25 points or more), it first catches up where its skill can gather, on the nodes that give skill,
  and then goes back to its quests. Miners also smelt ore at a forge to catch up.
- **Crafting:** after it hands in quests at a hub it makes what still gives skill, from its bags and the guild storage:
  orange and yellow recipes first, green ones only when nothing better can be made there. It buys materials vendors sell
  and walks to an anvil, forge or fire next to the hub when a recipe needs one.
- **Recipes:** it buys the recipes of its professions it can learn now at vendors it visits or that stand nearby (up to
  1 gold each), and learns recipes the guild storage holds (the bot with the highest skill gets them).
- Whatever it makes goes into the guild storage, except a tool, what its own quests need and gear it puts on.

## The profession mode

The order **Professions** makes a bot work only on its professions until they reach their targets:

- it crafts what gives skill, makes the materials its recipes need first (bars from ore, bolts from cloth), buys what
  vendors sell and walks to the places recipes need;
- it gathers on rounds over the nodes its skill opens, hunts creatures for skins and for materials that only creatures
  drop, and fishes;
- a profession more than 50 points behind the bot's others gets its full attention until it has caught up;
- a bot in the profession mode leaves its squad.

When there is nothing left it can do for its professions, the order goes back to the Autopilot by itself. At level 60
bots switch to the profession mode on their own until both primary professions reach their targets.

## Craft orders

Want something made? The **Storage** tab's **Craft orders** view lists what your guild's characters can make. Order "make
N of this item"; a bot that knows the recipe makes it on its next town trip from its bags and the guild storage, buys
vendor materials from the guild's budget, makes a tool it lacks first, and puts the result into the storage. The view
also lists your casters without a wand and who could make one.

An enchanter may disenchant green and blue items of the storage that nobody would wear, to get the dust and essences an
order needs.

## What crafters make for the guild by themselves

| Who | What |
|---|---|
| Cooks | The food your guild's characters eat at their levels, from the storage's meat and fish, until the storage holds 40 of each kind |
| Engineers | The best ammunition the guild's hunters with a gun can use, until the storage holds 5,000 rounds of it |
| Crafters at skill 300 | The best elixirs, flasks, potions, scrolls, sharpening stones, oils and scopes they can make for a dungeon or raid run, until the storage holds 20 of each kind |
| Enchanters | The best enchantment for blue or better gear of item level 60 or more that your bots wear, written on a vellum of the storage; the bot that wears the piece puts the scroll on |
| Anyone with the recipe | Raw materials above 1,000 in the storage processed: ore into bars, cloth into bolts, stone into grinding stones |
| Blacksmiths | Skeleton keys for the storage's lockboxes |
| Enchanters | Crafted gear the guild has more of than it wears, disenchanted |

Crafting gear for other guild members (the best piece a crafter can make for whoever would wear it, from the storage's
materials) is off by default; switch it on with the bot setting **Professions.CraftGear**.

## Crafting yourself

When you play a character of your guild, its profession window counts the guild storage's materials too: the recipe list
shows how often each recipe can be made, and **Create (storage)** / **Create All** make it with materials from the
storage. A bot's **Crafting** sub-tab lets it craft a recipe a number of times right away.
