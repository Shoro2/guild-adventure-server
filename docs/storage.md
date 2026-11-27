# Guild storage, loot filter, auto loot and crafting from the storage

the owner, 2026-11-12: *[quote left out]* and *[quote left out]*.

The guild gets one shared, unlimited storage that the leader and every bot use. Everything the bots loot goes there
(through a loot filter), guild characters loot their own kills around them automatically, and spells take missing
reagents from the storage, so the leader and the bots craft from it without taking anything out first.

Sources: ForgottenLand's own modules (the owner's, read-only, never changed): `mod-endless-storage` (unlimited material
storage with an AIO window and a "craft from storage" overlay in the profession window), `mod-auto-loot` (AOE looting,
skinning and gathering on the player's update tick) and `mod-loot-filter` (rule-based keep / sell / disenchant / delete /
store on every looted item). Their logic is ported into `mod-guild-adventure` and the GuildAdventure addon; no Eluna, no
AIO (decision D3). The Paragon part of the loot filter (the "cursed" condition, which reads mod-paragon-itemgen's slot-11
enchant, and the one-tick wait for that enchant) is left out.

And (the owner, later the same night): *[quote left out]* — the storage's surplus goes to the
auction house (§8).

Status: see [§ Status](#status). Open decisions: the design log §7 "Open" S1-S14.

---

## 1. Pieces

| Piece | Where | What |
|---|---|---|
| `StorageService` | `src/Storage/StorageService.{h,cpp}` | The store: one map per adventure guild (owner account) from item kind (entry + random property) to amount; what may go in (`RefuseReason`); deposits, withdrawals, deltas for the addon; the CP-7 reagent hooks. Thread-safe (one mutex), written to `ga_guild_storage` in one transaction every 5 s and at shutdown |
| `LootService` | `src/Storage/LootService.{h,cpp}` | The loot filter: rules and settings per guild (`ga_loot_rule`, `ga_loot_condition`, `ga_loot_settings`), loot routing one world tick after the loot hook, the actions, totals, the guild's recent-loot log |
| `LootRules.h` | `src/Storage/LootRules.h` | Pure rule logic (model, matching, evaluation, validation, condition codec, ordering): a port of mod-loot-filter's `LootFilterRules.h` without the cursed condition and the migration; offline test `tests/test_loot_rules.cpp` |
| Auto loot | `src/Storage/AutoLoot.cpp` (`LootService::OnCharacterUpdate`) | Port of mod-auto-loot for guild characters |
| `CraftService` | `src/Storage/CraftService.{h,cpp}` | Crafting on request: casts a recipe n times for the played character or a bot; a crafting bot stands still (the order planner pauses it) |
| Hooks and commands | `src/Storage/GuildAdventureStorageScripts.cpp` | PlayerScript (loot, update, spell cast, the two CP-7 hooks), WorldScript (startup, update, shutdown), the `.ga storage`, `.ga loot` and `.ga admin ...` commands |
| Protocol | `src/Protocol/GadvStorage.cpp` | GADV blocks and intents (docs/protocol.md §6, §7.10-§7.13) |
| `BotDriver::WantsItem` | `src/BotDriver.cpp` | playerbots' item usage: what a bot will use itself stays with it |
| Core patch CP-7 | `Shoro2/azerothcore-ga`, `Spell.cpp`, `PlayerScript.h/.cpp`, `ScriptMgr.h` | `OnPlayerCheckReagent` / `OnPlayerTakeReagent` (docs/core-patches.md) |
| `AuctionService` | `src/Storage/AuctionService.{h,cpp}` | The surplus in the auction house: rules per guild and kind (`ga_auction_rule`), the auctions it posted (`ga_auction_listing`), posting, unsold auctions sold at the vendor price (or back into the storage) |
| Addon | `addon/GuildAdventure/UI/Storage.lua`, `UI/Crafting.lua`, `State.lua`, `Protocol.lua` | Storage tab (Items with the AH dialog, Loot filter, Recent loot), the profession window overlay, the bot detail's Crafting sub-tab |

## 2. The guild storage

- **Per guild.** The key is the owner account (as every module table); a second owner gets a second storage (M9).
  The owner of a character is found through its WoW guild (`Player::GetGuildId`): every guild character is a member of
  its adventure guild's WoW guild. That lookup is safe on map threads, which the reagent hooks and auto loot need.
- **Unlimited**, like mod-endless-storage, and **separate from the native WoW guild bank** (6 tabs of 98 slots, bought
  with gold): the guild bank stays what it is; the plans of the design log §2.5/§2.11 ("products go to the guild bank",
  "deposit all gathered herbs") now mean this storage (S1).
- **What goes in:** every item without its own state. A kind is its entry plus its random property ("of the Bear"; the
  suffix factor follows from the item level, so two such items are the same). Refused, with the reason
  `RefuseReason` gives: quest items (class, bonding or quest starter), conjured, timed (`Duration`), keys, soulbound
  (except never-worn bind-on-pickup loot, below) or account-bound, wrapped, with text, damaged, enchanted by a player (permanent, temporary, gems, socket bonus,
  prismatic), used charges, a bag that is not empty. mod-endless-storage took only stackable trade goods, gems, recipes
  and food; the owner wants *everything* the bots loot in there, so the guild storage takes gear too.
- **Deposits:** "Deposit materials" (the addon, `.ga storage deposit`) moves mod-endless-storage's materials
  (stackable trade goods and gems, recipes, stackable food) from the played character's bags; Alt+click on a bag item
  while the Storage tab is open moves that stack; the loot filter moves loot.
- **Withdrawals:** the owner takes items into the bags of the character he plays (the addon: one stack, Ctrl everything,
  and since 2026-11-25 Shift-click opens a box for any amount (the owner: [quote left out]; the Take
  button's tooltip says so; before, Shift took ten stacks); `.ga storage take`). As many as fit; the rest stays. Bots do not take items out (S4).
- **Finding items** (the Items view): a category on the left (cloth, herbs, weapons, armour, ...), a search by name, and
  since 2026-11-13 (the owner: [quote left out]) two dropdowns beside the search: **type** (for armour the kind: cloth, leather, mail, plate,
  shields, jewelry; for weapons the weapon kind: one- and two-handed swords, axes, maces, staves, bows, wands ...; for
  the rest the item subclass: potions, elixirs, food & drink, bandages; parts, explosives; recipes by profession; gems
  by colour) and **slot** (head, neck, shoulder, back, chest, ... for what is worn; robes count as chest). Both offer
  only what the storage holds in the chosen category (the slots of the chosen type), and they combine with the search.
  The slot comes with each storage row (`inv`, docs/protocol.md §7.10). A second row (2026-11-25, the owner: [quote left out]): **Quality** (one
  quality, or uncommon / rare / epic or better), **iLvl from** (only weapons and armour of that item level or higher;
  empty: no limit) and **Sort** (name, quality, item level, amount; the highest first). Quality and item level come with
  each storage record (`qi`, docs/protocol.md §7.10), from an older server out of the client's item cache.
- **Reserves** (the owner, 2026-11-13: [quote left out]): the button *Res* on an item's row sets how
  many of the kind the bots leave in the storage (0: none); the row shows it (`res 20` after the count, *Res* in blue,
  the tooltip). Table `ga_storage_reserve`, intent `STORAGE_RESERVE`, `.ga admin storage reserve <character> <item>
  <n>`. Enforced in one place, `StorageService`: `GetAmount`, `GetAmountOfEntry`, `List`, `Remove` and `Withdraw`,
  which every automatic use calls (the bots' supplies while they level, quest items, crafting and the reagents of a
  bot's spells, gear and dungeon sets, ammunition, poisons, containers, disenchanting for orders, the auction surplus,
  Sell junk), see and take only what is above the reserve; `GetStored` / `ListAll` are the whole amount (the addon,
  counting). A take the reserve refuses writes one activity line per character and kind in half an hour
  (`... leaves the guild storage's 20 x Elixir of Agility alone: the guild keeps 20 of it in reserve`). **Not held
  back:** the owner's own Take, Sell, Delete and DE (a `StorageService::OwnerOrder` on the thread, also for the
  administrator's `.ga storage take` and `.ga admin storage sell / disenchant`), what the character on the owner's
  client uses as a reagent when he crafts by hand. What a dungeon run uses (elixirs, scrolls, stones, oils, scopes:
  docs/orders.md "The dungeon run window") keeps the reserve too since 2026-11-14, when the owner made that use automatic
  (every kind is used unless the leader takes it out); the bot setting `Storage.ReserveForDungeons` (off by default,
  it was on) lets a run into the reserves again (§7 "Open" RS1, DC3). An auction rule keeps at least the reserve; the target stock of a FARM
  order counts the whole amount (`GetStoredOfEntry`), so a reserve does not raise it.
- **Persistence:** in memory, written as absolute amounts every 5 s and at shutdown. A crash can lose the last seconds,
  and since the bags of a character are saved on their own cycle, an item can then exist twice (in the storage and back
  in a bag) or once less; mod-endless-storage has the same window. No duplication without a crash.

### Soulbound loot

the owner (2026-11-13, §7 DL1): the guild storage takes a bind-on-pickup piece that was never worn ([quote left out]). On a dungeon run a bind-on-pickup blue every bot passed on ends up soulbound in the bags of the bot whose auto
loot reached the corpse; before this, the storage refused it and it lay there for good.

- **Which items:** bind-on-pickup **weapons and armour of green quality or better** (`StorageService::IsUnwornKind`;
  none of the kind's own refusals such as quest item or timed). Nothing else that is soulbound: bind-on-equip or
  bind-on-use pieces that were bound by wearing or using them, soulbound consumables, account-bound items stay refused
  (for gear a bot has worn see "Gear it has worn" below).
- **How "never worn" is known:** the core keeps no "was equipped" state on an item (a bind-on-pickup piece is bound
  the moment it is picked up, and nothing records that it was put on), so the module **marks the piece when it is
  looted**: `OnPlayerLootItem` (loot window and auto loot), `OnPlayerGroupRollRewardItem` (a won roll) and, since
  2026-11-14, `OnPlayerQuestRewardItem` (a quest's reward: a bot carried eight bind-on-pickup quest rewards it never put
  on, which the storage refused) call `StorageService::MarkUnworn` for a guild character. The mark is the item instance (`item_instance.guid`) and the
  adventure guild, kept in `ga_unworn_loot` across restarts. **`OnPlayerEquip` ends the mark for good**, whatever
  equips the piece (the client, playerbots' "equip upgrade", the storage supply, a swap), so a piece that was put on
  once and taken off again is refused. Marks of items that no longer exist are deleted at startup. Pieces looted before
  this existed carry no mark and stay refused (there is no safe way to tell they were never worn).
- **What may go in:** a marked piece that is in the bags (not equipped) of a character of the guild it was marked for,
  while the bot setting `Storage.UnwornBound` (default on, per guild and per character, category Loot) is on for that
  character. Then it goes every way an item goes in: the loot filter's "to the storage" (for an AI-played looter after
  the check that the bot will use it itself: an upgrade stays with it), the bag sweep, Alt+click and
  `STORAGE_DEPOSIT`. The loot log says "never worn, bound on pickup", the activity log "put a never-worn
  bind-on-pickup ... into the guild storage".
- **Taking it out:** the storage keeps the kind, not the instance; a withdrawal creates a **new item bound to the
  character that takes it** (the core binds a bind-on-pickup item as it is stored, `Player::_StoreItem`), so another
  guild character can wear it, and the storage supply hands it to the bot it is the best upgrade for (§9). The new
  instance is marked again, so a piece taken out and not put on may go back (the supply's "put back what it cannot
  wear"); once it is worn it stays with its wearer. `StorageService::IsMarkedUnworn` tells such a freshly taken piece
  from one the character wore (the dungeon roles' role store looks for it so). An enchanter can disenchant it from the storage (green and blue,
  "Selling and disenchanting").
- *The developers' choices:* green and better only (white bind-on-pickup gear is rare and junk anyway); a mark is kept for the
  guild, not only for the character that looted it, so a piece traded to another guild character within the loot
  trade window still counts.

### Gear it has worn

Found on 2026-11-14 (the observer's report): Magefrost, one of the owner's bots, had stood at 0 free bag slots for over
an hour, its vendor trips sold nothing, and a quest object could not be looted. Its bags held 34 green pieces, and
every one of them was soulbound: worn and replaced by better gear as it levelled (a bind-on-equip piece is bound the
moment it is put on). The guild storage takes no soulbound item, the junk rule sells grey and white only, and
mod-playerbots' own sale at a vendor skips such a piece for an enchanter (it rates it "disenchant" and never does it)
and for what it rates "bad equip" (a warrior in mail at level 47, where it expects plate). Four bots of the guild stood
at 0 free slots with 15 to 34 such pieces each (Magefrost, Priestyo, Shadow with Enchanting; Wardps without); 158 worn
green pieces lay in the bags of the guild's bots.

The rule (`LootService::HandleWornGear`, §7 WG1; the owner chose "disenchant or sell" on 2026-11-14):

- **Which pieces:** a green or blue weapon or armour piece in the bags of an AI-played guild character that is
  soulbound and **not** marked never-worn (those go into the storage, §2 "Soulbound loot"), and that the character
  will not wear: not an upgrade or a piece for a bare place (`BotDriver::WantsItem`), not wanted by its dungeon set nor
  an upgrade of it (`RoleService::Wants`, `IsRoleUpgrade`). Never a quest item or what a quest in its log asks for, a
  tool, a unique piece (`MaxCount`), a kind the guild keeps a reserve of in the storage, a piece that came out of the
  guild storage (marked when the storage hands it out, kept in `ga_storage_origin`), and a blue
  bind-on-pickup piece of a class that can tank or heal while the guild has given it no dungeon role yet.
  `.ga admin worngear <character>` lists each soulbound weapon and armour piece with what the rule makes of it.
  *Since 2026-11-26* (the owner, the design log section 7 WG1): a unique-equipped piece (a ring, a trinket; one without a limit
  of how many may be carried) goes like any other worn piece, also one that came out of the guild storage; the worn
  copies of an Aquamarine Signet had stayed in the bags of Retadin and others for good and kept mod-playerbots from
  putting a better copy on. A piece that came out of the storage and was never worn keeps its protection.
- **What happens:** the character disenchants the piece itself when its Enchanting is at least what the piece needs
  (the game's disenchant table; the materials go into the guild storage, what it does not take into the bags); a
  character without the skill sells it at its next vendor visit (the planner's vendor visit, before the purchases),
  for the vendor price; the gold is taxed as usual ("selling its worn ... to a vendor").
- **When:** with the bag sweep (an AI-played character with fewer than 8 free bag slots, once a minute; with or
  without the perk Material Haulers: only disenchanting there) and at every vendor visit of the planner.
- **What it writes:** an activity line per piece ("disenchanted its worn ... (it will not wear it again): ... into the
  guild storage", "sold its worn ... at ... for ...c") and a Recent loot line (outcome sold or disenchanted, "worn, it
  will not wear it again").
- **Bot setting** `LootFilter.WornGear` (per guild and per character, category Loot): *Disenchant or sell* (default),
  *Disenchant only* (a character without the skill keeps such pieces), *Keep it*.
- *The developers' readings:* green and blue only (an epic stays); the disenchant needs no skill-up and gives none (as the
  guild's disenchants of storage items); pieces from before the never-worn marks of quest rewards cannot be told apart
  from worn ones and count as worn; a piece that came out of the storage before `ga_storage_origin` existed carries no
  origin mark. Open (§7 WG2): a worn piece that came out of the storage stays in the bags for good under this guard.

### Quest items no quest needs

the owner (2026-11-18, §7 QI1), after the worn gear was gone and quest items filled the bags (Wartank: 25 stacks of
scourgestones, power crystals, a photograph of a quest long done): [quote left out] (`LootService::HandleSpareQuestItems`, called first by
`HandleWornGear`, so with the bag sweep and at every vendor visit of the planner).

- **Which items:** a quest item (quest class or quest binding) or a quest starter, grey or white, in the bags of an
  AI-played guild character, that no quest in its log uses (asks for it, hands it out as its source item or a drop),
  that holds nothing (a container), and that no quest asks for which the character can still do for experience. A
  starter goes only once its quest gives no experience any more.
- **"Still gives experience"** (*the developers' reading*): below the level cap, not done (a repeatable one counts again),
  for its race and class, and not grey for it (the bots leave grey quests alone, GQ1; the game would still give a
  tenth of the experience). At the level cap no quest gives experience, so a level-60 character keeps only what its
  log uses.
- **What happens:** what a vendor buys is sold at the next vendor visit (taxed, "selling ... to a vendor (no quest needs
  it)"), what no vendor buys is deleted where the character stands. An activity and a Recent loot line per stack.
- **Not by this rule:** green or better quest items (the blue Darkmoon cards are worth a deck), what the owner's own
  character carries.
- Bot setting `LootFilter.SpareQuestItems` (per guild and per character, category Loot, on). `.ga admin worngear
  <character>` lists the quest items too, with what the rule makes of each.

### Darkmoon cards

the owner (2026-11-25, §7 DM1): [quote left out]. The
Darkmoon Faire cards (the Ace through Eight of a set: Beasts, Portals, Elementals, Warlords, Blessings, Storms, Furies,
Lunacy, Prisms, Nobles, Chaos, Undeath) and their decks are quest items to the game, so the storage refused them and
the loot filter kept them in the bags (S3); 20 of his bots carried some, up to ten kinds each.

- **Which items** (`StorageService::IsDarkmoonCard`, found at startup in the item and spell data, one line in
  Server.log: "12 Darkmoon Faire decks and 96 cards go into the guild storage": four sets each of the classic game,
  the Burning Crusade and Wrath): a deck is a quest item that starts
  a quest and that eight or more quest items make with their use spell; the cards are those eight. Map fragments and
  the halves of a book make their quest item too, in threes and twos: they are not cards.
- **Into the guild storage:** a card or deck an AI-played guild character loots (auto loot, a roll it wins) goes in at
  once, and what it carries goes in at a look once a minute (`LootService::StoreDarkmoonCards`, any number of free bag
  slots); a line of Recent loot per stack ("a Darkmoon card", "a Darkmoon deck") and an activity line per look ("put
  the Darkmoon cards it carried into the guild storage: 6 x Six of Beasts, ..."). The owner's own character keeps what
  it loots (his bags are his, S2); he puts cards in by hand (Alt+click in the bags, the storage takes them now).
- **No rule moves them:** quest items to the game, no rule of the loot filter sells, disenchants or deletes them, and
  they are never junk. Neither Material Haulers (P4) nor what mod-playerbots would do with them holds them back
  (*the developers' reading*: the owner asked for it as such). A deck a quest in the bot's log hands in stays with it.
- **Out of the storage** only by the owner's own Take, Sell or Delete, or by an auction rule he sets (the category
  Darkmoon cards can have one); no bot takes them.
- **The category** *Darkmoon cards* (`darkmoon`) in the Storage tab: the server names it in the storage tuple's tenth
  field (docs/protocol.md §7.10), the class does not say it (12, quest).
- Bot setting `Storage.DarkmoonCards` (1016, guild-wide, category Loot, on; `GuildAdventure.Storage.DarkmoonCards`).
  Off: they stay in the bags as before.
- **Not built:** bots that put the eight cards of a set together into a deck (the cards must lie in one character's
  bags; the owner can take them out and combine them himself).

### Reputation turn-ins (2026-11-25)

The quest items repeatable reputation quests ask for piled up in the bags (in the owner's guild about 10,000: the Argent
Dawn's Core of Elements 2253, Bone Fragments 2776, Dark Iron Scraps 572, Crypt Fiend Parts 423, Savage Frond 347 and
Scourgestones 5759, the Darkmoon Faire's Evil Bat Eyes 3714). The storage took no quest item (S3), and QI1 would have
sold them. The owner chose [quote left out] on the decision card (the design log section 7 RT1).

- **What they are** (`StorageService::IsReputationTurnIn`, found once from the quest data; a line in Server.log): a
  quest item (class quest or bound as one) that a repeatable, daily or weekly quest with a reputation reward asks for.
  Trade goods such quests take (Runecloth) stay what they are.
- **Into the guild storage** as the Darkmoon cards: what an AI-played guild character loots goes in at once, what it
  carries at a look once a minute (`LootService::StoreReputationTurnIns`); a line of Recent loot per stack ("a reputation
  turn-in") and an activity line per look (`put the reputation turn-ins it carried into the guild storage: ..`). What a
  quest in its log asks for stays with it. No loot filter rule sells, disenchants or deletes them, and QI1 keeps them.
  Soulbound ones (the Scourgestones) go in too and come out bound to the one who takes them.
- **The category** *Reputation turn-ins* (`reputation`) in the Storage tab (the storage tuple's tenth field, docs/protocol.md
  section 7.10).
- Bot setting `Storage.ReputationTurnIns` (1017, guild-wide, category Loot, on; `GuildAdventure.Storage.ReputationTurnIns`).
  Off: they stay in the bags.
- **Not built:** bots that hand them in for the reputation (a quest the planner would take for it).

## 3. The loot filter

Every item a guild character loots (the leader and its bots; by hand, by playerbots' looting or by auto loot) fires the
core's `OnPlayerLootItem`; the hook queues it and the world thread decides one tick later:

1. The guild's rules (up to `GuildAdventure.LootFilter.MaxRules`, 30): one action plus up to eight conditions in up to
   three groups (all / any within a group, all / any between groups), checked top to bottom; the first matching rule
   whose action can handle the item decides (sell needs a vendor price under the gold cap, disenchant a disenchant id,
   storage a storable item); when none can, the first matching rule's action falls back (to the storage if possible,
   else kept). Conditions: quality, item level, sell price, item type (class + subclass), item id, name contains.
2. No rule matches and the item is **junk** (below): it is **sold** at once, the vendor price goes to the looter. This
   comes before the default and before the Material Haulers gate (selling is no storing).
3. No rule matches and it is no junk: the guild's **default** — `To storage` for a character **the AI plays** (a bot on its home session,
   the leader while the owner plays a bot, a watched character), `Keep` for the character **the owner plays himself**
   (S2). Both can be changed in the addon. *Since 2026-11-12 (the owner: [quote left out]):* the default `To storage` acts only once the guild bought the guild perk **Material Haulers**
   (`materials_storage`, docs/guild-management.md "Guild perks"); until then loot no rule decides stays in the bags, and
   the bag sweep does not run. With the perk the crafting materials (trade goods) the owner's own character loots go
   into the storage too. A rule's own action, `To storage` included, works with or without the perk.
4. Never touched: quest items. For a character the AI plays, also what it will use itself, by playerbots' item usage
   (`BotDriver::WantsItem`): an equipment upgrade (it equips it), a quest starter it can take, food, drink and potions it
   keeps, ammunition, a class reagent it needs. Trade goods it would keep for a profession go to the storage anyway: its
   crafting reaches them there (CP-7). A rule cannot send such an item away, only `Keep` keeps it (S3).
5. Actions: **Keep** (stays in the looter's bags; a bot sells what it cannot use at its next vendor visit), **Sell**
   (vendor price to the looter), **Disenchant** (no Enchanting skill needed, as in mod-loot-filter; the materials go into
   the guild storage), **Delete**, **To storage**.
6. **Gold** stays with the looter (S5): the bots pay their own repairs, training and food (decision O1).
7. The guild's totals (stored, sold for, disenchanted, deleted) and the last 50 loot lines are kept; the owner's own
   character gets one summary chat line per tick ("[Guild storage] Stored 3, sold 1 for 2s 10c."), the bots' loot shows
   in the Storage tab's "Recent loot".
8. Server switches: `GuildAdventure.LootFilter.Enable`, `.AllowSell`, `.AllowDisenchant`, `.AllowDelete` (a disallowed
   action makes its rules skip; without `AllowSell` no junk is sold), and per guild "Loot filter on" in the addon (off =
   loot stays where it lands).

### Junk

the owner (2026-11-12): [quote left out].
Before, a guild without rules (his had none: the Loot filter view stayed black in the old addon) put everything into the
storage, grey items included. Now loot no rule decides is sold when it is junk (`LootService::IsJunk`):

- **Junk:** grey items, **all white weapons and armour** (not a fishing pole; the owner, 2026-11-13: [quote left out]), and white misc items of the "junk" kind that do nothing when used. What the
  looter puts on itself is kept: a bot's upgrade (mod-playerbots' item usage, step 4), and for the character the owner
  plays a piece it can wear that beats what it wears in that place (or fills an empty one). Until 2026-11-13 white
  gear any guild character in the world would wear was kept too; that is now the bot setting
  `LootFilter.KeepWornGear` (default off; on: grey and white gear that is an upgrade for a guild character in the
  world goes into the storage, checked over the leader and every bot, cached for a minute per item).
- **Never junk:** items without a vendor price; quest items, quest starters, keys; tools (a pick, a skinning knife, a
  rod: they have a totem category); **crafting materials**: every reagent of a profession recipe (first aid, cooking
  and fishing included, whatever its quality: Ruined Leather Scraps are grey) and all white trade goods; items a quest
  asks for or hands out; consumables, bags, ammunition, recipes, gems, glyphs. What a bot will use itself stays with it
  anyway (step 4), so a level-3 bot without gloves still puts grey gloves on.
- **Settings** (bot settings, category *Loot*, per guild and per character, docs/bot-settings.md):
  `LootFilter.SellJunk` (default on), `LootFilter.SellJunkQuality` (*grey and white*, or *grey only*) and
  `LootFilter.KeepWornGear` (default off); the conf keys `GuildAdventure.LootFilter.SellJunk` / `.SellJunkQuality` /
  `.KeepWornGear` are the server's defaults. The looter's value counts for its
  loot, the guild's value for the storage.
- **The addon** shows it in the Loot filter view as built-in rows below the guild's own rules ("Built in: grey items
  and white weapons and armour (junk), except what the looter puts on → Sell", "Built in: crafting materials, quest
  items, ... → Default"), so a guild with no rules sees what happens to its loot.
- **What lies in the storage already:** the Items view's **Sell junk** button asks the server what of the storage is
  junk now (kinds, items, price), shows it, and sells it into the guild bank (§10; before 2026-11-13 into the character
  the owner played) only after his OK
  (`STORAGE_SELL_JUNK`, docs/protocol.md §7.10). Nothing in the storage is sold without that click. It always leaves
  gear a guild character in the world would put on (the storage pass hands that out, §9). Every kind it sells gets a
  line in Recent loot.

### Selling and disenchanting

the owner (2026-11-13): [quote left out]; [quote left out]. Each row of the Items view has:

- **Sell** (one) and **All** (the whole kind): sold **at once** at the vendor price; the copper goes **into the guild bank**
  (since 2026-11-13; before, to the character the owner played). Greyed for what no
  vendor buys. All, and Sell of a green or better item, ask first with the price (`STORAGE_SELL`).
- **Del** and **All** in red, in place of Sell and All, on what **no vendor buys** (the owner, 2026-11-13: [quote left out]): one, or the whole kind, is
  destroyed after a confirmation (`STORAGE_DELETE`); a line in Recent loot ("deleted from the guild storage (no vendor
  buys it)") and in the activity log name who did it. What a vendor buys cannot be deleted this way.
- **DE** (only on green and blue weapons and armour with a disenchant table): one item is disenchanted **at once** by the
  guild character in the world with the most Enchanting skill (the leader included); the materials go into the storage.
  Greyed, with the reason in its tooltip, while no guild character in the world knows Enchanting or the best one's skill
  is below what the item needs; it asks first (`STORAGE_DISENCHANT`). The addon knows the best enchanter from
  `STORAGE_BEGIN` and `STORAGE_ENCH`, the skill each kind needs from the storage tuples.
- *The developers' choices:* at once instead of on the enchanter's next town trip (the storage is reached from anywhere, and so
  is the loot filter's disenchant, which needs no skill at all). The gold went to the character the owner played until
  2026-11-13 ("nothing else puts gold there"); since the guild bank is fed (§10) it goes there.
- Every sale and every disenchant from the storage is a line in **Recent loot** (`LootService::AddStorageLog`): the
  character (for a sale the one the owner plays, for a disenchant the enchanter), the item and count, the copper or
  "disenchanted from the guild storage for Retadin: 2 x Strange Dust", and a line in the activity log. The disenchant
  itself is the craft orders' routine (`CraftOrders::DisenchantStored`, docs/orders.md "Craft orders"), which takes
  the item out of the storage, rolls the disenchant table and puts the materials in; `LootService::DisenchantFromStorage`
  adds the quality rule and the Recent loot line. A craft order's own disenchanting writes only its activity line.
- **Take and Deposit** (2026-11-13, asked by the integration server's per-item match of the storage: 1000 Sharp Arrow had
  left the owner's storage without a line, his own Take from the Storage tab): every order of the owner that moves items
  out of or into the guild storage is a line in Recent loot ("Taken out" / "Put in", `LootService::LogStorageMove`,
  outcomes 5 and 6 of docs/protocol.md §8.8) and in the activity log (`activity: Retadin took 1000 x Sharp Arrow out
  of the guild storage of owner 9 (taken by Retadin (Take))`): Take and `.ga storage take`, Deposit, Deposit materials
  and `.ga storage deposit` (one line per kind), the auction house surplus when it is listed (out) and when it comes
  back unsold (in), and an administrator's `.ga admin storage add`. The bots' own withdrawals and deposits keep their
  activity lines (`took from the guild storage: ...`, `put ... into the guild storage`) and are not in Recent loot, which
  holds only the newest 30 lines.
- Commands: `.ga admin storage sell <character> <item> [count]`, `.ga admin storage disenchant <character> <item>`
  (test aids, as the character's order).

## 4. Auto loot

A port of mod-auto-loot, for guild characters (`GuildAdventure.AutoLoot.*`, per guild "Auto loot" in the addon):

- Every `ScanIntervalMs` (250) in the character's own update: its own kills within `Range` (10) yards are emptied,
  gold included; a skinner skins them; the mining and herb nodes it can gather are gathered once the character stands in the reach of the gathering spell (the node's interact distance, a few yards; since 2026-11-12: before, the spell was cast from up to 10 yd, failed, and the node was despawned ungathered).
- Items are taken through the core's `Player::StoreLootItem`, so quest items, roll winners, loot achievements and
  `OnPlayerLootItem` behave as for a click in the loot window (mod-auto-loot created the items itself and lost their
  random suffix).
- In a dungeon or a raid the group's loot method counts (2026-11-13): with group loot or need before greed the rolls
  start when the first member's auto loot reaches the corpse, as when a loot window opens, and an item a roll runs for
  stays in the corpse until it is decided; what a character wins in a roll goes through the loot filter like any loot
  (`OnPlayerGroupRollRewardItem`). In the open world nothing is rolled for: whoever of the group stands there takes
  it, as before (docs/orders.md "Dungeon runs", Loot).
- What does not fit stays in the corpse (no mail: the loot filter keeps the bags of guild characters empty). Not with
  fewer than four free bag slots.
- A group is fine when every member in it is a character of the same guild (the leader with its following bots);
  never in a battleground or dungeon-finder group. The corpse must belong to the character or its group.
- playerbots' own looting still runs: a corpse auto loot emptied is no longer lootable, a corpse farther away is looted
  by the bot itself; both go through the loot filter.

## 5. Crafting from the storage

- **Core patch CP-7** (`Shoro2/azerothcore-ga`): the reagent check of `Spell::CheckItems` (and the missing item named in
  `SPELL_FAILED_REAGENTS`) asks `OnPlayerCheckReagent` when the bags hold fewer than the spell needs, and
  `Spell::TakeReagents` lets `OnPlayerTakeReagent` take the missing part before the rest is destroyed in the bags. The
  module answers both for guild characters from the guild storage (`GuildAdventure.Storage.Reagents`). So **every**
  spell a guild character casts on the server takes what its bags lack from the storage: a bot's crafting (playerbots,
  M7 crafting orders later), the leader's crafting, class spells with reagents (S6). mod-endless-storage documented the
  same two hooks for ForgottenLand but never wired them in; it consumed the reagents itself and cast the spell as
  triggered, which skips the normal checks.
- **The played character's client** checks its own bags before it sends a tradeskill cast, and its Create button is
  greyed out without the materials in the bags. The addon therefore shows the storage in the profession window (as
  mod-endless-storage's overlay did): reagent counts include the storage (blue when only the storage makes them enough),
  the recipe list shows how often each recipe can be made, and when the bags alone are not enough the Create and
  Create All buttons become "Create (storage)", which asks the server to start the cast (GADV `CRAFT`). The cast itself
  is a normal cast (cast time, tools, spell focus, skill-ups), its reagents come through CP-7.
- **Bots:** the bot detail's "Crafting" sub-tab lists the bot's recipes (tradeskill spells that create an item) with how
  often its bags and the storage allow them; "Craft" makes it craft n times (`CRAFT^g`), "Stop" ends it. A crafting bot
  stands still (its order pauses; the roster line says "Crafting <item>: 3 of 10") and resumes its order afterwards.
  `.ga admin craft <bot> <spell> [n]` does the same from the console.
- `CraftService` casts one after the other like the client's "Create All" and stops when a cast fails or is interrupted
  (moving, a fight). Enchanting onto an item (an item target) is not a recipe it crafts (S7).
- The products of a character the AI plays go into the guild storage when its crafting has ended (2026-11-13, the owner:
  [quote left out]; a tool and an item for its own quest stay, gear it made is
  put on at once and what it does not wear goes in, also what lay in its bags from before: docs/orders.md "Craft
  orders"). What the owner makes with the character he plays lands in its bags; they are not
  loot, so the loot filter does not move them (S8).

## 6. Commands

| Command | Who | What |
|---|---|---|
| `.ga storage [character]` | GM (console with a character) | Kinds, items, the 15 biggest stacks |
| `.ga storage deposit` | GM | Your character's materials into the storage |
| `.ga storage take <item> [count]` | GM | Items into your character's bags |
| `.ga storage junk [character]` | GM (console with a character) | What of the storage is junk now: kinds, items, vendor price, the 15 biggest |
| `.ga storage selljunk` | GM | Sells it into your character |
| `.ga admin selljunk <character>` | administrator, console, SOAP | Sells the guild's stored junk into that character (tests) |
| `.ga loot [character]` | GM (console with a character) | Settings, totals, rules |
| `.ga loot recent [character]` | GM (console with a character) | The last 15 loot lines |
| `.ga admin storage add <character> <item> [count]` | administrator, console, SOAP | Puts items into a guild's storage |
| `.ga admin loot <character> <item> [count]` | administrator, console, SOAP | The character "loots" an item (created in its bags, then the loot filter) |
| `.ga admin lootrule <character> <action> <conditions>` | administrator, console, SOAP | Adds a rule (`type:op:value:value2:text[:group];...`) |
| `.ga admin lootrules clear <character>` | administrator, console, SOAP | Deletes the guild's rules |
| `.ga admin lootoptions <character> <filter> <ai> <own> <autoloot>` | administrator, console, SOAP | Sets the guild's options |
| `.ga admin learn <character> <spell>` | administrator, console, SOAP | The character learns a spell (recipes for tests) |
| `.ga admin craft <character> <spell> [count]` | administrator, console, SOAP | The character crafts from its bags and the storage |
| `.ga admin sweep <character>` | administrator, console, SOAP | The bags of an AI-played character through the loot filter now |
| `.ga auction [character]` | GM (console with a character) | The guild's auction rules and what is listed |
| `.ga admin auction rule <character> <item> <keep> [stack] [price]` | administrator, console, SOAP | Sets an auction rule |
| `.ga admin auction norule <character> <item>` | administrator, console, SOAP | Deletes it |
| `.ga admin auction post <character>` | administrator, console, SOAP | Posts the surplus now |
| `.ga admin auction expire <character>` | administrator, console, SOAP | The guild's auctions expire at the next auction house update (tests) |

## 7. Data

Characters database (`ga_characters.sql`): `ga_guild_storage` (owner, entry, random property, amount, class, subclass),
`ga_loot_rule`, `ga_loot_condition` (mod-loot-filter's schema keyed by owner instead of character), `ga_loot_settings`
(filter on, default actions, auto loot, totals), `ga_auction_rule` (owner, entry, random property, keep, stack, price,
on), `ga_auction_listing` (auction id, owner, entry, random property, count), `ga_unworn_loot` (own file
`ga_unworn_loot.sql`: item guid, owner, character, entry, when marked; §2 "Soulbound loot"), `ga_storage_origin`
(own file `ga_storage_origin.sql`: weapons and armour the storage handed out, item guid, owner, character, entry, when;
§2 "Gear it has worn"). Nothing in the world database.

## 8. Auction house

- **Per item kind in the Storage tab ("AH"):** keep X in the storage, auction the rest in stacks of S, at a price per
  item (empty = the default price). Only kinds the auction house takes (not bind-on-pickup, no quest item, ...).
- **Per category** (the owner, 2026-11-13: [quote left out]; the Items view's button *Category AH* for the category chosen on the left, `AUCTION_CAT_SET`,
  table `ga_auction_category_rule`): keep X of **each kind** of the category (cloth, herbs, metal and stone, weapons,
  ...), auction the rest in stacks of S at the default price. A kind's own rule comes first; a category with a rule
  carries a gold "AH" in the category list, a row whose kind follows it a darker gold "AH" button.
- **Marked before it is listed** (the owner: [quote left out]): a row whose kind the next pass would post (more than X plus one stack, by its own rule
  or its category's) has a gold background and a gold `[AH n]` before its name, n = what goes at the next pass (whole
  stacks above X); its tooltip says so. What is listed already stays `(+n AH)` after the count (`AUCTION_LISTINGS`).
- **Posting** (`AuctionService`, every `GuildAdventure.Auction.IntervalSeconds`, 300; `.ga admin auction post` at once):
  full stacks of what is above X become normal auctions in the name of the **guild leader**, in the leader's faction
  auction house, for `GuildAdventure.Auction.Hours` (24), at most `MaxStacksPerItem` (10) per kind at the same time.
  The leader does not have to be online or at an auctioneer; the items leave the storage when they are posted.
- **Sold:** since 2026-11-13 the gold goes **into the guild bank** at the moment of the sale, minus the auction house's
  cut (the auction hook `OnBeforeAuctionHouseMgrSendAuctionSuccessfulMail`: no mail to the leader; S13 changed). The sale
  is a line in Recent loot ("sold in the auction house, into the guild bank", the money) and in the activity log
  ("guild bank of ...: +1g 37s 18c (sold 20 x Linen Cloth in the auction house)"). Before, the core mailed it to the
  leader with its delivery delay of an hour, and he had to take it from a mailbox (four such mails were waiting for
  Retadin on 2026-11-13: 1g 39s 46c for 20 Linen Cloth and three stacks of arrows). A guild without a WoW guild bank keeps
  the old mail. *Known gap (2026-11-14, ga-probe `guild-bank`):* when a **player** buys a storage auction out, the core
  also sends the leader its money-less "sale pending" notice (the gold would come in an hour); the module does not
  suppress it yet. The AH bot's buyout sends no such notice, so on a server where only the bot buys nothing shows.
  A bid the AH bot places (it bids the starting bid when the buyout is above what it pays) is paid at the auction's end,
  24 hours after it was posted, then likewise straight into the guild bank.
- **Unsold:** an expired auction does not reach the leader's mailbox (the auction hook
  `OnBeforeAuctionHouseMgrSendAuctionExpiredMail`). Since 2026-11-26 (the owner: [quote left out]; §7 AH2) what did not sell is sold at the vendor price, the
  copper into the guild bank (`guild bank ...: +N (sold 20 x Thorium Bar at the vendor price: it did not sell in the
  auction house)`, an activity line `auction house: 20 x Thorium Bar of owner 9 did not sell: sold at the vendor price
  for Nc into the guild bank` and a Recent loot line "Sold"). What the kind's auction rule keeps, at least its reserve,
  goes back into the storage first (the bots may have taken some out since it was put up); tools, Darkmoon cards,
  reputation turn-ins and what no vendor buys always go back whole, and so does everything for a guild without a WoW
  guild to take the copper. Bot setting `Auction.SellUnsold` (1018, guild-wide, on); off, everything comes back into the
  storage and the next pass posts it again while the rule is on (5299 returns did so from 2026-11-12 to 2026-11-26). An
  auction the leader cancels by hand reaches him by mail, as usual. The leader's own auctions are never touched (the
  service knows its own by `ga_auction_listing`).
- **Default price** (no price in the rule), after the AH bot's own valuation (mod-ah-bot-plus, set to buy only by the owner):
  the vendor price, at least `MinPrice` (1000 copper) or `MinPrice.TradeGoods` (850) per item, times the AH bot's quality
  multiplier (uncommon 1.8, rare 1.9, epic 2.1), times `PriceFactor` (0.85): a little under what its buyer pays at most,
  so it buys them; players see them like any other auction (S12).
- Buyout = price × stack; the starting bid is 90 % of it; no deposit (the server posts them).

## 9. Supplies for the bots

the owner, 2026-11-12 (DQ3): [quote left out]. `Storage/StorageSupply.cpp`, called once a minute per living AI-played guild character from
`OrderService::UpdateBot`: the bots, and the leader while the AI plays him with an order (watch mode, the offline
  window, while the owner plays a bot); the leader the owner plays himself is not served.

- **Consumables, one piece at the moment of use** (the owner, 2026-11-13, DQ11: [quote left out]). Nothing is
  stocked in a bot's bags any more (until 2026-11-13 a bot below half a stack took a whole stack of food and of drink:
  a cook's eleven clams went straight into one bot's bags). `StorageSupply::TakeOne` takes one piece out for the use
  at hand, and each piece is a line of the activity log (`<bot> took 1 x Haunch of Meat out of the guild storage to
  eat (39 left there)`). The storage is reached from anywhere (S1), in a fight too.
  - **Food and drink** (`Storage.Supply.Provisions`): when the bot eats or drinks in a rest (docs/orders.md "Rest and
    provisions": food for health below `Rest.BelowHealth`, drink for mana below `Rest.BelowMana`) and carries none,
    the best kind the storage has for its level. What it carries is used up first, whatever its kind (the stacks
    bought at vendors before this rule, looted food). While the storage has that kind for a bot it buys none: the
    planner's purchase, its trips for provisions and mod-playerbots' own purchase (the vendor hook in
    `Ammunition.cpp`) all ask `StorageSupply::Has`. A vendor is what is left when the storage has none.
  - **Potions** (`Storage.Supply.Potions`): in a fight below `Fight.PotionBelowHealth` (35 %) health a healing
    potion, and for a mana user below `Fight.PotionBelowMana` (15 %) mana a mana potion while its target still has
    more than a quarter of its health (`OrderService::StepPotion`). The best the bot can use, by what the potion
    restores: its own, or one piece out of the storage when that has a better one. The game allows one potion per
    fight and a cooldown after it; none is taken while that runs. A healing potion goes through while another cast
    runs (it ends that cast). Not a druid in a form. Fight line: `, drank Lesser Healing Potion`.
  - **Bandages** (`Storage.Supply.Bandages`): a character with First Aid bandages itself at the start of a rest it
    began for its health (once per rest, not with the "recently bandaged" mark), with the best bandage its skill
    allows, its own or one out of the storage; the food follows for what is still missing. Rest line: `, bandaged
    itself with Heavy Linen Bandage`.
  - **Scrolls and elixirs** (`Storage.Supply.Scrolls`, on; `.Elixirs`, **off** by default: the owner named scrolls, and
    in the first live round with elixirs on 30 bots used 55 of them in 22 minutes; `StorageSupply::UseBuffs`, once a
    minute). A scroll at most once per bot per the scroll's own duration (a death does not cost a second one), and
    no piece is taken while something on the bot does not stack with it by the core's spell groups (a strength or
    agility scroll and an elixir of that stat or a totem's strength; in that round 13 scrolls "did nothing" and went
    back, and scrolls and elixirs of one stat took turns on a bot). The rules: while
    the bot works on a task with fights (a quest objective, grinding, an escort, a quest event, or following its
    squad) and is out of combat and on foot, it uses the best scroll of its main stat (strength for warriors, death
    knights and paladins that strike; agility for rogues, hunters and druids and shamans that strike; intellect for
    everybody else) when no buff of that size is on the stat already (a totem's or another player's counts), and
    not a class whose own buff takes the scroll's place (a shaman with Strength of Earth Totem and a death knight
    with Horn of Winter leave strength and agility scrolls to the others, a mage with Arcane Intellect the intellect
    scrolls: a shaman's totem took its scroll's effect off in the next fight), and the best battle elixir and guardian elixir
    that do something for the way it fights when it has none of that kind on. Not on a farm order (its creatures are
    far below it), not for a walk, a hand-in or a vendor trip, and not in a dungeon, where only what the leader
    ticked for the run is used. It is the dungeon preparation's routine (`DungeonSupplies::Use`): the piece is used
    straight out of the storage, a piece that did nothing goes back. Log: `<bot> used 1 x Scroll of Strength II out
    of the guild storage while it levels (3 left there)`.
  - **Mages** (the owner, 2026-11-13, DQ12: [quote left out]): a mage that knows Conjure Water (level 4) or Conjure Food (level 6) takes none of that kind out of the
    storage and buys none (`StorageSupply::Conjures`); it conjures its own (docs/orders.md "Mages conjure"). Conjured
    pieces never go into the storage (it refuses them) and have no vendor price.
- **Gear** (`GuildAdventure.Storage.Supply.Gear`): out of combat and while no cast runs (an auto shot included) the
  bot rates the storage's weapons and armour it can
  use (per place on the body the 4 best by playerbots' score for the bot, its own stat weights with the piece's random
  property; until 2026-11-12 the 40 highest item levels over all places, so armour could crowd out every weapon, and
  until 2026-11-25 the 4 highest item levels per place: the owner's storage held about 30 green cloth helms of item
  level 57 to 62 with random suffixes, most no use to a caster, and so no caster ever looked at the epic Eye of Flame
  of item level 55, while Mageyo, a level-60 mage, wore a Holy Shroud of item level 32; the scores are kept per bot
  for half an hour, its level and its talent tree, at most 400 new kinds are scored per look, an epic first) with
  mod-playerbots' item usage; among the upgrades for one place on the body
  (hands; ranged; each armour slot, chest and robe together) it takes the one with the best playerbots score for its
  class and spec, for at most three places per look, and puts them on (playerbots' "equip upgrade"; since 2026-11-26 a
  piece beside an older copy of its kind in the bags by the module itself, `PutOnThisOne`: mod-playerbots equips by the
  item's entry and took the older, worn copy, no upgrade, so Retadin and three others put an Aquamarine Signet of the
  Bear back; once a minute an upgrade the bot carries beside an older copy is put on the same way, loot included; and
  since 2026-11-26 a caster's weapon is judged with its hands as a whole, `BotDriver::CasterHandsNoBetter`: a staff
  only when its stats are worth more than those of the one-hander and the off-hand piece it replaces, a one-hander in
  place of a staff only when it alone is worth more than the staff; mod-playerbots compares a two-hander with the main
  hand alone, a staff counting half and a caster's one-hander 65 %, the design log section 7 CW1). A
  piece it could
  not put on goes back at once; it is tried again at the next look, after a second failure in 10 minutes, after a third
  only at the next level (a second weapon without dual wield never fits; until 2026-11-12 one failure, e.g. right after
  a fight, kept a better gun from a hunter for a whole level). What it wore
  before goes through the guild's loot filter like loot (`LootService::SweepBags`), so into the storage unless a rule
  says otherwise, where the next bot may find it an upgrade.
- The storage is rated again only when its contents or the bot's level changed. Activity log: `<bot> took from the
  guild storage: 20 Tough Jerky, Long Crawler Limb; could not wear and put back: Commoner's Sword`.
- What stays with a bot is still S3: the loot filter never stores what playerbots says the bot uses itself.
- **Necklaces, rings, trinkets, relics, off-hand pieces** (the owner, 2026-11-13: [quote left out]). mod-playerbots never puts such a piece into a bare place: its armour rule
  (`RandomItemMgr::CanEquipArmor`) wants the armour kind of the class for everything but cloaks, and a necklace's
  kind is "miscellaneous", so its item usage answers "bad equip". On the integration server none of 30 guild bots
  wore a necklace or a trinket and three wore a ring, with 28 Malachite Pendants, six rings and nine off-hand pieces
  in their storage. The module's own rule, `BotDriver::FillsEmptyPlace`: a necklace, ring, trinket or relic the
  character can use (class, level) fills a bare place whatever its stats are worth to the class; something held in
  the off hand only for mages, priests, warlocks and druids, and not under a two-handed weapon; not a second one of
  a kind it may wear once. It is part of `BotDriver::IsUpgrade`, so it holds wherever the module asks "would this
  character wear it": the bots take such pieces out of the storage (the best by mod-playerbots' score per place, the
  highest item level where the score is nothing; both ring and trinket places, one per look), a looted one stays
  with the looter (`WantsItem`), a quest reward that fills a bare place counts as an upgrade, and an enchanter does
  not disenchant a piece that would fill a bare place of a guild character in the world (`CraftOrders`), nor does
  the junk rule's "a guild character would wear it" let it go. `Gear::FillEmptySlots` puts them on (always; its
  setting `Gear.FillEmptySlots` is about armour of another kind than the class's). A better piece for a place that
  is taken was mod-playerbots' matter before and still is. At a vendor a bot buys jewellery only when it is worth
  something to it by the score (GearUpkeep). Activity log as for other gear: `took from the guild storage: Elegant
  Silver Ring, Malachite Pendant`.
- **Not a second one of a kind it may wear once** (the owner, 2026-11-14: [quote left out]). 13 of his bots
  took a second jewelcrafted ring (Ring of Silver Might, Ring of Twilight Shadows, Inlaid Malachite Ring, Elegant
  Silver Ring: unique-equipped) out of the storage twice after every restart and put it back ("could not wear and
  put back"): each wore one copy on one finger and a weaker ring on the other, mod-playerbots compared the copy with
  the other finger and said "replace", and its own unique check never sees a worn copy (it tells
  `GetItemCount(entry, true)` from `GetItemCount(entry, false)`, which differ only by the bank).
  `BotDriver::WearsUniqueAlready` (the core's `CanEquipUniqueItem` for the unique-equipped flag and limit
  categories, and `MaxCount` against what is worn) is part of `IsUpgrade`, `WantsItem` and `FillsEmptyPlace`, so a
  ring, trinket or weapon of which the character wears its one copy is no upgrade anywhere: not out of the storage,
  not at a vendor, not as a quest reward, not for "a guild character would wear it", and a looted second copy goes
  the loot filter's way. The role store keeps to it as well: a second copy is no role upgrade, the dungeon set plans
  one copy and none beside a copy that stays on the other finger (docs/dungeon-roles.md).
- **A piece put on and taken off again in one look** (round 178, 2026-11-23). Shammpower, an enhancement shaman with
  a weapon in each hand, took a Heroic Guard (a green shield that binds when it is put on) out of the storage:
  mod-playerbots' upgrade check rated it above its off-hand axe, its "equip upgrade" put it on, and its next comparison
  took the weapon back. The shield lay in its bags bound to it; the supply logged "could not wear and put back", but the
  storage refuses a soulbound piece and the result of the deposit was not looked at, so the storage was one short and
  the piece stayed with the bot. The same that morning: Shadow's High Councillor's Boots (soulbound in its bags since
  10:37). Now: a shield or an off-hand item is no candidate for a bot that holds a weapon in its off hand; each piece
  is followed by its own instance (the bags may hold another of the kind); a piece the storage handed out in that look
  and that the equipping bound without it staying on goes back all the same (`StorageService::DepositItem(..,
  takeBound)`, the only soulbound piece besides never-worn loot that the storage takes: `<bot> put <item> back into the
  guild storage: it came out of it a moment ago and was bound by being put on, not worn`); a piece that still cannot go
  back is named as such (`could not wear and could not put back, it stays in its bags: <item> (<reason>)`).
- "Can use" includes the weapon skill: every guild character knows every weapon skill its class may learn
  (`ClassAbilities::LearnWeaponSkills`, docs/server-setup.md "Weapon skills and riding"); before, a paladin without
  axes could not take a two-handed axe that was an upgrade for him.
- **Ammunition** (the owner's rules of 2026-11-13, `Ammunition.h`; they replace what this section said about
  ammunition before). His words: [quote left out]. The history behind them: mod-playerbots
  buys a hunter eight stacks and anyone else with a bow two at every vendor, bag sweeps and weapon changes moved what
  was "not wanted" into the storage, and one guild's storage held 11507 Heavy Shot. For every character the AI plays:
  - **The storage takes ammunition only when a guild character made it.** `StorageService::DepositItem` refuses it
    from loot, a sweep of the bags, a full-bags deposit and a deposit by hand ("ammunition is only stored when a
    guild character made it"); what a recipe makes goes in as before (`CraftOrders`, the profession mode). The loot
    filter turns "to the storage" into "sell" for ammunition an AI-played character does not shoot (deleted when it
    has no vendor price) and into "keep" for the owner's own character.
  - **A stock in rounds** (`Ammo::Stock`): a hunter 2000, below level 20 1000, below level 5 200; any other class
    with a bow, crossbow or gun one stack (settings `Ammo.HunterStock`, `.HunterStockBelow20`, `.HunterStockBelow5`,
    `.OthersStacks`). The vendor hook `OnPlayerBeforeBuyItemFromVendor` refuses every purchase of ammunition that
    would go over it or that the character's weapon does not shoot, mod-playerbots' own included.
  - **A hunter takes the best ammunition for its weapon out of the storage** when it carries less than half its
    stock, up to the stock: the best kind it can use that is no worse than what it carries (a level-30 hunter with
    Sharp Arrows had filled its quiver with the storage's Rough Arrows) nor than the best ammunition any vendor sells
    it for gold (`Ammo::VendorBest`; since round 171: Hunterbm (60) shot the storage's Thorium Shells, 17.5 damage,
    while every general goods vendor sells Impact Shot, 22, from level 55). While the storage holds such ammunition
    the hunter buys none and makes no trip for it. At a vendor what it carries that is worse than the vendor's best
    does not count against its stock: it fills the stock with the better kind, and `Ammo::Trim` parts with the worse
    one, what it took out of the storage or a guild character made back into the storage. Other classes do not take ammunition out of the storage. A bow or gun
    out of the storage is handed to a hunter only when it has ammunition for that kind of weapon or the storage does;
    the ammunition follows once the weapon is in its hands.
  - **What a character must not carry is sold at vendor price** once a minute out of a fight (`Ammo::Trim`):
    ammunition its weapon does not shoot or it cannot use, and what is over its stock, the worst kind first
    (damage, then level). Most ammunition below level 40 has no vendor price: it is deleted. One line each time:
    `Hunterbm sold ammunition at vendor price: 1144 x Rough Arrow (over its stock, no vendor price: deleted), ...; 0c
    (it stocks 2000, carries 2000 now)`. A hunter without a ranged weapon in its hands is not judged.
  - **The weapon first, and nothing that came out of the storage is deleted** (live round 58: a hunter took 1603
    Sharp Arrow for its bow, a gun and 2000 Crafted Heavy Shot out of the storage in one look, and the 2000 arrows
    it then carried were deleted in the same second). A look at the storage takes the gear first and the ammunition
    after it, for the ranged weapon the hunter ends up with. When a character parts with ammunition (the wrong kind
    after a change of weapon, or over its stock), two kinds go back into the storage instead of being sold: a kind a
    recipe makes (Crafted Heavy Shot: a guild character made it), and as much of any kind as this character took
    out of the storage since the server started (`Ammo::NoteTaken`). Line: `Hunterbm put ammunition back into the
    guild storage: 1306 x Razor Arrow (it had taken it out)`. The storage never gets more of a bought kind than it
    gave. A sweep of the bags leaves ammunition alone: it is `Ammo::Trim`'s.
  - Buying at vendors and the trips for it: docs/orders.md "Vendor trips". `.ga admin ammo <character>` shows how
    the rules judge a character (its weapon, rounds and stock, what the storage holds for it, the vendors in reach);
    `buy` and `trim` are test aids.
  - What the storage already held when the rules came (bought ammunition) stays there as stock the hunters draw
    from; nothing sells it.
  - **Engineers keep it stocked** (the owner, 2026-11-22: [quote left out]): the kind each of the guild's gun hunters would shoot, below `Professions.AmmoStock` (5000)
    in the storage; docs/orders.md "Engineers make ammunition for the guild".
- **A piece the bot could not put on** (2026-11-13). mod-playerbots calls some pieces an upgrade and then does not
  put them on; the bot took such a piece out and put it back twice in a row, a third time ten minutes later, and
  all of that again at every level (one hunter: a Forester's Axe 14 times; a never-worn bound blade went out and in
  at every look of a dungeon test). After one failure the piece now waits ten minutes, after the second until the
  bot is three levels on. (The memory is the server's: after a restart a piece is tried twice again.)
- **A profession's tools stay in the bags** (2026-11-13). The loot filter kept "a tool" by the item's totem
  category (a pick, a knife, a hammer, a rod). The Jeweler's Kit has none in the item data: a sweep stored it every
  minute and the profession plan handed out the next (71 to 114 kits in one guild's storage in a round).
  `LootService::IsToolOf` also knows the module's own tool list (`Professions::IsTool`) and any item a recipe the
  character knows names as its tool (`SpellInfo::Totem`); one of such a tool stays, whatever the rules say. The
  junk rule, the bag space the planner makes and the profession goods it stores ask the same.
- **A line per bag sweep** (2026-11-13). Every sweep that moves something writes one activity line: `<name> bags
  through the loot filter: into the guild storage 1 x Lesser Moonstone, 1 x Small Brown Pouch; sold ... (N copper);`
  (4365 bullets had gone into a storage without a line in any log, which looked like the storage making
  ammunition).
- **Storage and bags are written together** (2026-11-13). The storage is written every five seconds, a character's
  bags only with its next save, up to a quarter of an hour later. A worldserver that was killed in between came back
  with a deposited stack both in the storage and in the bags, and without what had been taken out. The bags and the
  money of every character that deposited, took or sold something since the last flush are now saved in the same
  transaction as the storage's rows (`StorageService::Touch`, `Player::SaveInventoryAndGoldToDB`).
- Limits: first come, first served (a warrior may take the dagger a rogue would use better; bots of one guild are
  served in roster order); no reservation by the owner; shirts, tabards and bags are not handed out;
  the score is mod-playerbots' own (it likes item level and its default spec's stats).

## 10. The guild bank and the guild tax

the owner, 2026-11-13: [quote left out] `GuildBank.cpp`.

- **What it is:** the bank money of the adventure guild's WoW guild (`Guild::GetTotalBankMoney`), which the Overview
  shows as "Guild bank". Until 2026-11-13 nothing of the module ever put gold there: junk the loot filter sells paid the
  looter, the Storage tab's Sell paid the character the owner played, auction sales were mailed to the leader, and "the
  guild paid" of a vendor purchase meant the purses of the leader and the other bots in the world.
- **What goes in:** the guild tax; what the guild storage sells (Sell, All and Sell junk on the Storage tab); the gold
  of the storage's auctions (§8). Every deposit and withdrawal is a line in the activity log: `activity: guild bank of
  <guild>: +12s 40c (<why>), now 1g 3s 7c`; the addon's Overview gets the new number at once (`STATE^bank`).
- **The guild tax** (bot setting `Guild.TaxPercent`, guild-wide, in the Settings tab's category "Gear and guild gold";
  conf `GuildAdventure.Guild.TaxPercent`, default 10): that share of every copper a **roster bot** gets goes into the
  guild bank (the owner, 2026-11-14: [quote left out]). Every gain of a character's money passes
  `Player::ModifyMoney` (looted coin, the module's auto loot and opened containers, quest rewards, vendor sales and
  mod-playerbots' own sales, junk the loot filter sells, surplus ammunition sold, mail money, trade money, a group's
  share of coin); `PlayerScript::OnPlayerMoneyChanged` holds the tax back from the amount before it reaches the bot
  (any map thread) and queues a booking, and the world thread books every queued gain into the guild bank by itself
  at its next tick, one activity line each, which names what the gain was where the module can tell:
  `activity: guild bank of ...: +5s 2c (guild tax 50% of 10s 4c Hunterbm got: looted coin), now 301g 55s 80c`
  (`the reward of quest 22 "Goretusk Liver Pie"`, `selling Linen Cloth to a vendor`, `junk the loot filter sold`,
  `by mail from ...`, `by trade from ...`, `income` where it cannot). Until 2026-11-14 the queue was booked once a
  minute as one sum per guild, so the guild bank moved in steps while the bots' gold moved with every gain. Fractions
  of a copper are carried over per character. The addon gets the new bank money once per tick and guild.
  `.ga guildbank [character]` shows the bank, the percentage and what still waits for its booking.
- **What is not taxed** (no income; each such gain is a line `... got 1g (by trade from Retadin): no guild tax on it
  (money handed over by the guild's own characters)`): the guild bank's own gold, its part of a purchase (handed over
  under `GuildBank::NoTax`) and a withdrawal from the WoW guild bank (`GuildScript::OnMemberWitdrawMoney`); money a bot
  gets by trade or mail from the owner's own characters or another character of the guild (bot setting
  `Guild.TaxHandovers`, default off: it was taxed when it was earned); the auction house's refunds of a bid (outbid,
  cancelled; a sale's money is taxed). The leader pays no tax at all, whoever plays it (the owner, 2026-11-14: [quote left out]). A bot the owner plays by hand pays like any bot. Not told apart (taxed like income): an item refund of the game's own (an item bought with honor or tokens and
  given back within two hours; bots never do it) and a GM's `.modify money`.
- **What comes out:** the guild bank's part of a vendor purchase (gear, quest items: docs/orders.md "Gear and the
  guild's budget"). The buyer pays `Budget.MemberPercent` of its own money first, the guild bank pays the rest within
  `Budget.ItemPercent` of its money per item and `Budget.BotAllowance` per bot; an empty guild bank pays nothing, and a
  bot then buys only what its own share covers. That gold is handed to the buyer untaxed.
- **Commands:** `.ga guildbank [character]` (GM, console); `.ga admin guildbank add <character> <copper>` (a test aid:
  copper into the guild bank).

## 11. Containers

the owner, 2026-11-13: [quote left out]; and to the question whether the bots should open the containers already in the storage: [quote left out].
`Storage/AutoLoot.cpp` (`LootService::OpenContainer`, `OpenContainersStep`, `OpenStoredContainers`).

- **What a container is** (`LootService::ContainerKindOf`): an item with loot of its own that a right click opens (bags
  of loot, crates, junkboxes, quest reward boxes: `ITEM_FLAG_HAS_LOOT`), the same with a lock (lockboxes), and an item
  whose use spell hands out loot and uses it up (the clams: Small Barnacled, Thick-shelled, Big-mouth, Soft-shelled
  Clam and the Outland and Northrend ones; `spell_loot_template`). Wrapped gifts are not opened.
- **When a bot loots one** (an AI-played guild character, bot setting `LootFilter.OpenContainers`, on): it opens it at
  once, as the core does for a player: the item's loot is generated (its money too; what does not fit stays in it, as
  the core keeps it), every item goes through `Player::StoreLootItem` and so through the loot filter like other loot,
  the container is used up (one of a stack for a clam). A lock is picked first as the rogue's Pick Lock does it (the
  spell's effect for an item, Spell::EffectOpenLock: the lock's level against Lockpicking, one skill-up; not a cast,
  whose effect comes later while the playerbots AI opens an unlocked box by itself, without the guild's log line), so only a rogue whose Lockpicking is high enough opens a lockbox; a lockbox
  nobody of the guild can pick goes the usual way (into the guild storage), and so does a container a rule of the
  guild sends elsewhere (keep, sell, delete). One the bot cannot open right now (a fight, fewer than 2 free slots)
  stays in its bags and is opened at its next look (every 15 s, at most 5 per character).
- **The guild storage's containers** (bot setting `Storage.OpenContainers`, guild-wide, on): every 15 s one AI-played
  guild character with 6 free slots and not in a fight (the bots first, the leader last) takes up to 5 containers out
  of the storage and opens them; lockboxes only a rogue that can pick them or a blacksmith with a skeleton key for
  them, regularly (the owner, 2026-11-13: [quote left out]; 2026-11-26: [quote left out]): one per character every `Storage.LockboxMinutes`
  (2 since 2026-11-26, was 10; bot setting, guild-wide; 0 = as many as it can). A lockbox a rogue loots itself it
  opens at once. *Lockpicking:* a rogue has Pick Lock from level 16 and its Lockpicking at five points a level, 300
  at 60, raised at its login and every level-up (`ClassAbilities::RaiseLockpicking`, line `has its Lockpicking for
  its level: Lockpicking 67 -> 300`; the owner's two level-60 rogues stood at 67 and 25 and so opened no lockbox of the
  storage, which holds 84 Iron, 92 Strong Iron, 78 Steel, 87 Reinforced Steel, 721 Mithril, 904 Thorium and 5
  Eternium Lockboxes). *Skeleton keys:* a key the character may use (a skeleton key asks for Blacksmithing) in its
  bags or the guild storage, the weakest that opens the lock (the lockpicking its use spell opens with, against what
  the lock asks for; Silver, Golden, Truesilver and Arcanite Skeleton Key open up to 25, 125, 200 and 300; only items
  that ask for Blacksmithing, never an engineer's Seaforium Charge, which opens a lock the same way: the first build
  used all 69 of the owner's Small and Large Seaforium Charges on Iron Lockboxes), is used up and the
  lock is open (`LootService::KeyFor`, `KeySkill`; line `opened its lock with a Truesilver Skeleton Key`). A blacksmith
  makes the keys for the storage's lockboxes (`Professions::KeysForGuild`: each lockbox kind counted for the weakest
  key it knows that opens it, up to 10 of a kind in the storage; materials from the storage, a material made first,
  an anvil walked to), at a hub after a hand-in, in the profession mode, and at the level cap the autopilot goes into
  the profession mode for it (lines `makes skeleton keys for the lockboxes of the guild storage`); the key goes into
  the storage and the opener takes it out (a Recent loot line "taken out to open a lockbox of the guild storage").
  It uses the storage's bars (a Truesilver Bar a Truesilver key). The withdrawal is a Recent loot line
  ("taken out to be opened"), each container opened another one.
- **The log:** every opened container is a line in Recent loot ("Opened", outcome 7 of docs/protocol.md §8.8: the
  container, its money, what came out) and in the activity log (`activity: Probemage opened Thick-shelled Clam
  (looted): 1 x Clam Meat, 1 x Iridescent Pearl`); what came out has its own loot filter lines.
- The owner's own character opens what it loots by hand, as before.

## 12. Shopping list

the owner, 2026-11-14: [quote left out]. Built as `ShoppingList` (`src/Storage/`).

- **The list** (the Storage tab's *Shopping list* view; GADV `SHOP_SET` / `SHOP_DEL`, docs/protocol.md §7.25;
  `.ga admin shopping set|del`): a minimum stock per item for the guild storage, per guild, at most 60 items
  (`ga_shopping_list`). An item can be on it when a vendor the guild's side can trade with sells it for gold; never
  ammunition (it goes into the storage only when a guild character made it, docs/storage.md §9), nothing that binds
  when bought, nothing the storage refuses (quest items). The view searches what vendors sell (`SHOP_SEARCH`) and puts
  the vendor drinks or breads of level 1, 5, 15, 25, 35 and 45 on the list in one go (*Drink tiers*, *Food tiers*,
  each with the minimum in the box; a tier already on it keeps its minimum). Each row shows what the storage holds
  against the minimum (red below it), the vendor price, *Min*, *Pause* / *Resume*, *Remove*.
- **When bots buy:** at a vendor an AI-played guild character visits anyway, the planner's vendor visit (to sell, to
  repair, to restock, for a material, poisons or a recipe; `OrderService::StepNpcTask`). No trip is made for the
  list (§7 SL1, decided by the owner). The owner's own character buys by hand; the test aid `.ga admin shopping buy <character>` buys at
  the vendor a character stands at.
- **What it buys:** for every item on the list this vendor sells, the shortfall up to the minimum, in whole vendor
  bundles (five drinks per purchase), at most what the vendor's limited stock has. One stack per purchase, which goes
  straight into the guild storage (the bags need room for one stack); the core's own vendor purchase does it, so its
  reputation discount and checks hold.
- **Who pays:** the WoW guild bank (`GuildBank::Withdraw`, a line in the activity log; the money is handed to the bot
  untaxed for the purchase and what the vendor did not take goes back). Bounds per guild: `Shopping.MaxPerVisit`
  (5 gold per vendor visit) and `Shopping.MaxPerDay` (50 gold per day, server time UTC, kept across restarts in
  `ga_shopping_spent`). What the guild bank or the budgets do not cover is not bought, with one line per guild and item
  in half an hour (`does not buy for the guild's shopping list at ...: ...`); only with `Shopping.FromPurse` on (off by
  default) does the bot pay what the guild bank lacks from its own gold.
- **Two bots at one vendor:** purchases run one after the other on the world thread and the storage counts each at
  once, so the second bot sees the first one's purchase and buys only what is still missing.
- **The log:** per item an activity line (`activity: Probemage bought 20 x Morning Glory Dew at Innkeeper Allison for
  the guild's shopping list (16s from the guild bank): the guild storage has 100 of 100`) and a line in Recent loot
  ("Bought", outcome 8 of docs/protocol.md §8.8, with what it cost).
- **With the drink rule** (docs/orders.md "Out of drink"): unchanged. A bot still drinks out of its bags first, then
  takes single pieces out of the storage, and buys its own stack at a town; the list only keeps the storage filled, so
  the storage runs dry less often. The vendor hook that holds a bot's own food and drink purchases back while the
  storage has some for it (DQ11, `Ammunition.cpp`) lets a purchase for the list through (`ShoppingList::IsBuying`):
  without that a bot could fill the storage only while it was empty of its kind. Reserves (§2) count as stock: the
  minimum is the whole amount in the storage.
- **Settings** (bot settings, category Loot): `Shopping.Enable` (1010, per guild and per bot), `Shopping.MaxPerVisit`
  (1011), `Shopping.MaxPerDay` (1012), `Shopping.FromPurse` (1013, advanced).

## Status

See the design log §10 for the evidence of each step.
