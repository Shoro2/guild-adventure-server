# Orders

Every bot has exactly one order: what it is doing right now. You give it in the bot's **Orders** sub-tab (choose the
order, set its options, **Apply order**). New recruits start on the **Autopilot**. The bot's "why" line on the roster and
in its header says what the order makes it do at the moment.

## The orders

| Order | What the bot does |
|---|---|
| **Autopilot** | Plans its own next step: picks the best zone for its level, quests, grinds when no quest is left, visits vendors and trainers, follows its profession plan. The default, and the best order for levelling |
| **Level in zone** | Travels to the zone you pick and levels there with quests and grinding. The zone list marks which zones fit the bot's level. With **auto-advance** it moves on by itself once it has out-levelled the zone |
| **Idle** | Goes to a safe spot (an inn) and stays there |
| **Follow** | Follows the character you are playing and joins your party, like a companion |
| **Professions** | The profession mode: works on the professions of its plan until they reach their targets, then goes back to the Autopilot ([Professions](professions.md#the-profession-mode)) |
| **Dungeon** | Leads its squad into a dungeon and from boss to boss. The **Dungeons** tab does the same with more choices ([Dungeons and raids](dungeons-and-raids.md)) |
| **Farm item** | Farms one item for the guild storage (see below) |

Orders shown greyed out (Grind, Quest, Gather, Train, Vendor) are not available yet.

## The Autopilot

A bot on the Autopilot:

- **chooses its zone** by its level and the quests it can take there, and moves on when the zone runs dry or it has
  out-levelled it;
- **quests**: takes the quests of the givers it meets, works their objectives (killing, collecting, using items and
  objects, escorts and quest events), hands them in, and leaves alone quests it cannot finish;
- **grinds** when no quest for its level is left nearby, on creatures of about its level or a little below;
- **travels** like a player: on foot along roads, by flight path, ship, zeppelin and portal, and by hearthstone when it
  is far from home; it makes the inn of a hub it hands quests in at its home;
- **looks after itself**: rests and eats and drinks between fights, sells and repairs at a hub after handing in quests
  (or at once when its bags are full or its gear is breaking), buys food and drink from level 8, takes better gear and
  consumables out of the guild storage;
- **learns** the spells its class trainer would teach it on every level-up, where it stands, for free; weapon skills,
  riding at 20 and an epic mount at 40 come by themselves too;
- **is careful**: it avoids places and camps that are too much for it, leaves fights it would lose, and remembers where
  it died.

**Autopilot limits** (unlocked at renown level 3) keep a bot below a level cap or on one continent.

**At level 60** a bot does its remaining quests, then goes into the profession mode until both primary professions of
its plan reach their target (300 if you set none), and after that grinds creatures of level 50 to 60. It looks at its
professions again every half hour.

## Farm item

The bot hunts the creatures that drop the item you choose, in groups it can reach on foot, only creatures at least four
levels below it, on another continent if it must. What it loots of that item goes into the guild storage.

1. In the Orders sub-tab choose **Farm item**.
2. Search for the item (a name, an item id or a shift-clicked item). The list shows what creatures drop, with "(from
   level N)" where the bot is still too low.
3. Set a **Target stock**: when the guild storage holds that many, the order ends and the bot goes back to the
   Autopilot. 0 means it farms on and on.

When nothing drops the item within the bot's reach, you get a message that says why, and the bot waits at an inn.

## Squads

Two to five bots can level together as a **squad**: one leads and plays its order, the others follow it. In a bot's
Orders sub-tab, **Squad: follow** picks the bot it follows.

- The members fight beside their leader, take the quests it takes and hand them in with it.
- The leader waits for members that rest, are dead or fall behind; a member far behind travels after it.
- The members of a squad should be no more than three levels apart; a member too far apart plays its own order until
  the levels fit again.
- A squad is much safer than a bot alone, at the cost of some experience per kill.

A squad is also the group of a dungeon run.

## Quest partners

Elite quests, group quests and quests your guild's characters died on several times are hard for a bot alone. A bot
keeps such a quest in its log and skips it until another bot of your guild has the same quest; then the two team up for
it, meet, finish it together and part again. The bot setting **Squads.QuestPartners** switches this off for the guild
or one bot.

## Your leader

Your own character can have an order too, on the **Overview** tab's leader panel:

- **Hold position** (the default): your leader stays where it is while you play a bot, and logs out with you.
- **Autopilot**, **Level in zone**, **Idle** or **Follow**: the AI plays your leader with that order whenever you are
  not playing it yourself: while you play a bot, in watch mode, and in the offline window after you log out.

When you log in to a leader the AI was playing, it goes on playing in watch mode until you press **Take over**.

## Offline play

When you log out, your bots keep playing their orders for the length of your guild's **offline window**: one hour at
the start, up to eight hours with renown ([Progression](progression.md)). Then they log out. If you log in again within
the window, they are simply still there. The "While you were away" window tells you what they did.

If you log out while you are playing a bot, the bot goes back to its order and plays on.
