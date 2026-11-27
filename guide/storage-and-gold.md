# Storage and gold

## The guild storage

Your guild has one storage without a size limit, open from anywhere through the **Storage** tab. It is separate from
the game's guild bank (which holds the guild's gold, see below).

**What goes in.** With the perk **Material Haulers** ([Progression](progression.md#guild-perks)) everything your bots
loot goes into the storage by itself, except what the loot filter decides otherwise and what a bot keeps:

- quest items and what a quest in its log asks for, what it uses itself (food, potions, ammunition, an upgrade), and
  the tools of its professions stay with the bot;
- junk is sold at once (see the loot filter);
- the crafting materials you loot yourself go in too; everything else you loot stays with you.

Without Material Haulers the bots keep their loot in their bags and only empty them into the storage when they visit a
vendor with full bags. Some things go in whatever the perk: what bots craft, gear they took off for something better
when it was never worn, Darkmoon cards, reputation turn-ins (the items repeatable reputation quests ask for), and the
contents of the containers they open.

**What comes out by itself.** The bots help themselves when they need something, one piece at a time: food or drink for
a rest, a potion in a fight, a bandage, a scroll; better gear for their level, ammunition for a hunter, pieces for their
dungeon set; reagents for their spells and recipes. Set a **reserve** on an item (**Res**) to keep that many in the
storage: the bots only take what lies above it. You can always take all of it yourself.

### The Items view

- Categories on the left; filters for type and slot, quality, minimum item level; sorting by name, quality, item level
  or amount; a search box.
- **Take**: one stack into your bags. Shift-click: type how many. Ctrl-click: everything.
- **Deposit materials** puts the crafting materials in your bags into the storage; Alt-click an item in your bags while
  the tab is open stores that stack.
- Per row: **Sell** (one) and **All** (every one of that kind) at the vendor price; **DE** has the guild's best enchanter
  disenchant one green or blue weapon or armour piece into the storage; **Del** and **All** in red replace Sell for what
  no vendor buys. Each asks first where it matters and leaves a line in Recent loot.
- **Sell junk** shows what of the storage is junk and what it is worth, and sells it after you agree.
- **AH** sets an auction rule for the item (see below).

### Containers and lockboxes

Clams, bags of loot and crates the bots loot are opened at once, and those already in the storage are taken out and
opened. Lockboxes stay in the storage until a rogue of the guild whose lockpicking is high enough, or a blacksmith with
a skeleton key, opens them, one every couple of minutes. A rogue's lockpicking rises with its level by itself.

### Gear and items nobody needs

- A green or blue piece a bot wore and replaced is disenchanted by the bot when it can enchant, otherwise sold at its
  next vendor visit (bot setting **LootFilter.WornGear**).
- Quest items no quest needs any more are sold or thrown away (**LootFilter.SpareQuestItems**).
- Enchanters disenchant crafted gear the guild has more of than it wears.

## The loot filter

The **Loot filter** view decides what happens to loot before it reaches the storage.

- **Options:** the filter on or off; what happens to loot no rule decides, for the bots and for your own character;
  auto loot (bots and you pick up loot from their kills, skin them and gather herbs and ore within reach).
- **Rules:** keep, sell, disenchant, delete or store, each with up to four conditions (item quality, type, level,
  name and more). The first rule that fits decides. **New rule**, edit, on/off, up and down.
- **Built in:** junk is sold at once: grey items and white weapons and armour, unless the looter puts the piece on.
  Crafting materials, quest items and consumables are never junk.

**Recent loot** lists what was looted, sold, stored, disenchanted, bought, taken out and put in, with who did it.

## The auction house

Turn a surplus into gold: **AH** on an item sets "keep this many, auction the rest in stacks of this size at this
price (or the default price)". **Category AH** does the same for a whole storage category. Every few minutes your
leader posts what lies above the line as normal auctions; the storage marks those items in gold (`[AH n]`). What does
not sell comes back into the storage. The gold of a sale goes into the guild bank.

On a server with the auction house buyer bot, your auctions sell at a fair price even without other players.

## The shopping list

The **Shopping list** view keeps a minimum stock of items that vendors sell, typically drinks and food for your bots.

- Search an item, or add the vendor waters and breads of every level at once with **Drink tiers** and **Food tiers**.
- Per row: the stock against the minimum, **Min**, **Pause**, **Remove**.
- When a bot visits a vendor anyway and the vendor sells a listed item below its minimum, the bot buys what is missing
  straight into the storage, paid from the guild bank within a budget per visit and per day. Bots make no extra trips
  for the list.

## The guild bank and the guild tax

The game's guild bank holds your guild's gold. It is filled by:

- the **guild tax**: a share of every copper your bots earn (looted coin, quest rewards, sales) goes into the guild
  bank at once; 10 % by default, set by the bot setting **Guild.TaxPercent**. Your own character pays no tax, and gold
  handed between your guild's characters is not taxed;
- the storage's sales and auctions.

It pays the guild's part when a bot buys something at a vendor: the bot pays half from its own gold, the guild bank the
rest, at most 10 % of the guild bank for one item and up to an allowance per bot (bot settings **Budget.ItemPercent**,
**Budget.MemberPercent**, **Budget.BotAllowance**). An empty guild bank pays nothing. It also pays the unlocks you buy
and the shopping list.
