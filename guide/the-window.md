# The window

Everything you manage is in one window. Open and close it with `/ga`, the minimap button or a key binding. It can be
moved, resized and scaled (Settings, Addon). Features your guild has not unlocked yet are greyed out and say what
unlocks them.

## Tabs

| Tab | What you do there |
|---|---|
| **Overview** | Your guild at a glance: renown and its bar, bot slots, offline window, guild bank, the next unlocks, the latest events. The **leader panel** gives your own character an order (see [Orders](orders.md#your-leader)) |
| **Roster** | All bots in a sortable, searchable table: name, level, gear level (iLvl), dungeon role, the two primary professions with their skill, zone, order, state and experience. **Play** takes a bot over. Free slots and the next slot step are listed at the end. Click a row for the bot's details |
| **Recruitment** | Recruit a bot into a free slot ([Getting started](getting-started.md#your-first-bots)) |
| **Progression** | The renown track and every unlock with what it needs; **Buy** for unlocks that cost gold ([Progression](progression.md)) |
| **Achievements** | The guild's tiered achievements: every series with its value, the next tier and its reward |
| **Perks** | Guild points and the perks they buy for every member of the guild |
| **Storage** | The guild storage, the loot filter, recent loot, craft orders and the shopping list ([Storage and gold](storage-and-gold.md)) |
| **Dungeons** | The dungeon run window: send a group or a raid into an instance and follow its runs ([Dungeons and raids](dungeons-and-raids.md)) |
| **Map** | Not built yet; it stays locked |
| **Settings** | **Bot behaviour** (how your bots behave, [Bot behaviour](bot-behaviour.md)) and **Addon** (scale, minimap button, chat output, the away popup, the connection) |

### Roster tips

- Click a column title to sort by it. The search box finds names, zones, orders, professions and roles ("mining",
  "tank").
- The gear level turns yellow when it lies three levels below the bot's level and red at eight.
- The role is bright when you gave the bot a dungeon role, grey when it only comes from its levelling talents.
- A benched bot shows the values of when it was last seen, in grey.

## The bot's details

Click a bot on the roster (your guild leader is its first row and has the same details, without Bench and Dismiss).
The header shows its "why" line and the buttons **Take control**, **Bench** /
**Activate** (a benched bot logs out and keeps its slot) and **Dismiss** (see below). The sub-tabs:

| Sub-tab | What you do there |
|---|---|
| **Orders** | Give the bot an order, set its squad, its autopilot limits, the dungeon it leads or the item it farms ([Orders](orders.md)) |
| **Professions** | Its profession plan: two primary professions with a target rank, the secondary ones, its skill bars and which professions the guild already covers ([Professions](professions.md)) |
| **Crafting** | Its recipes and how often its bags and the guild storage allow each; **Craft** makes them now |
| **Character** | Its gear, gear level, readiness for the dungeons in its reach, its wand if it is a caster |
| **Talents** | The talent template it plays by ([Talents, roles and combat](talents-roles-combat.md)) |
| **Combat** | Its fight profiles and the fight profile editor |
| **Dungeon role** | Tank, healer or none, the talents for the role, its dungeon set and role store |
| **Behaviour** | Its own values of the bot settings, where they differ from the guild's |

### Dismissing a bot

**Dismiss** in the bot's header lets a bot go for good, to free its slot for another recruit. You are asked first and
have to type the bot's name. Then:

- everything it carries that is not soulbound goes into the guild storage (its bags, its bank, its dungeon set's role
  store; worn and soulbound pieces and quest items go with it),
- its gold goes into the guild bank,
- the character is deleted, and its slot is free at once,
- your renown, unlocks, guild achievements and perks stay: they belong to the guild.

**This cannot be undone.** A benched bot logs in for a moment to hand its things over. You cannot dismiss the bot you
play right now or one on a dungeon run. In chat, `.ga bot dismiss <name> confirm` does the same (GM accounts).

## The control bar

While you play a bot, a bar at the top of the screen shows "Playing: *bot*" and the bot's "why" line, with:

- **Return to leader** and a quick switch to another bot;
- **Watch / Take over**: let the AI play the bot while you watch, or take the controls back
  ([Taking control](taking-control.md#watch-mode));
- **Details**: a window with what the bot does now and next (its task, its quests with their progress and what they
  wait for, its squad, its upkeep). It refreshes every few seconds while it is open.

## While you were away

After you log in, a window lists what your guild did while you were offline: levels, quests, deaths, zone changes,
renown and unlocks. The **Overview** tab opens it again.

## Slash commands

| Command | What it does |
|---|---|
| `/ga` | Open or close the window |
| `/ga roster`, `/ga overview`, `/ga recruit`, `/ga progression`, `/ga settings` | Open that tab |
| `/ga watch [on\|off]` | Watch mode on, off, or toggled |
| `/ga reconnect` | Ask the server again when the window says it has no connection |
| `/ga mock on` / `/ga mock off` | A demo with a simulated server, to look around without one |

Key bindings (Key Bindings, section "Guild Adventure"): open the window, open the roster, return to your leader, watch
mode.

Chat commands that start with `.ga` are for the server's game masters; they are listed in the technical
documentation.
