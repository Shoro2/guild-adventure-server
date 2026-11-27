# GuildAdventure addon

WoW 3.3.5a (Interface 30300) client addon: the management UI of WoW Guild Adventure. It talks
to `mod-guild-adventure` over the GADV addon-message protocol ([docs/protocol.md](../docs/protocol.md))
and contains a mock server so the UI works before the server module exists.

## Install

Copy or junction `addon/GuildAdventure` into the client's `Interface\AddOns\GuildAdventure`
(the project client lives in `_work\client`, see the design log §5). Enable "GuildAdventure" on
the character select screen.

## Use

| Command | Effect |
|---|---|
| `/ga` | open or close the window (also: minimap button, left-click) |
| `/ga roster`, `/ga overview`, `/ga recruit`, `/ga progression`, `/ga settings` | open a tab |
| `/ga reconnect` | send a new handshake to the server |
| `/ga watch [on\|off]` | watch mode: let the server's AI play the bot you control (or your leader, once it has an order) while you watch, or take over again (no argument: toggle) |
| `/ga mock on` / `/ga mock off` | demo mode with a simulated server inside the client |
| `/ga mock reset` | wipe the demo guild |
| `/ga mock renown <n>` | demo: add renown (unlock slots and features) |
| `/ga mock away` | demo: simulate an offline window and show "While you were away" |
| `/ga debug` | print every protocol message to chat |

Without a server the window shows "No server found" (or "Server module missing" when the realm
echoes the addon whispers back) with a **Start demo mode** button.

Key bindings (Key Bindings menu, section "Guild Adventure"): open or close the window, open
the roster, return to your leader, watch mode (let the AI play / take over).

## Taking control

Swap requests go through one place (`UI.TakeControl` / `UI.ReturnControl` in
`UI/ControlBar.lua`). With the warm-relog swap (Path R, `docs/take-control.md`) the client
goes through the character screen and every addon loads fresh, so the addon:

- remembers the open page and bot in `GuildAdventureDB.ui.resume` when you ask for a swap and
  reopens it on the new character once the roster has synced (within 3 minutes);
- stores the target name from `CONTROL_PENDING` in the CVar `gaSwapTarget` for the patched
  GlueXML that enters the character automatically, and clears it on every entering-world and
  on every `CONTROL_STATE` with `busy=0` that arrives while the UI is still in the world.

**In a fight.** The server refuses a swap while either character is in combat (`IN_COMBAT`,
the swap guards of the design log §2.8; the chat command `.ga swap` is refused the same way). The addon
then keeps the request pending and sends it again every 2 seconds until the server takes it: the
switch runs by itself as soon as the fight is over. The chat and the error line say so, and the
control bar shows "Waiting for the fight to end". Pressing the same button again cancels it; it
gives up after 2 minutes. Any other refusal is told in the chat as well as in the red error line,
so a refused "Return to leader" no longer looks like a button that does nothing.

All saved variables are account-wide (`GuildAdventureDB`), so they are the same on the
leader and on every bot.

## Screens

- **Overview** — guild, renown bar, slots, offline window, guild bank, next unlocks, event feed, and the
  **leader panel** (`UI/LeaderPanel.lua`): the leader's order (Hold position, the default, Autopilot, Level in zone,
  Idle, Follow; `ORDER_SET` for the leader, record `LEADER`), its why line and Watch / Take over while you play it
  (docs/orders.md "The leader").
- **Roster** — sortable/filterable table (name, level, gear level, dungeon role, the two primary professions with
  their skill, zone, order, state, XP) with a **Play** button per bot, free slots and the next locked slot step. The
  role is bright when you gave the bot a dungeon role and grey when it comes from its levelling talents; a benched bot
  shows its gear level of when it was last seen, in grey; the row's tooltip lists every profession with skill and cap;
  the search also finds professions and roles ("mining", "tank").
- **Bot detail** (click a roster row) — header with the bot's "why" line, Take control /
  Return, Bench / Activate; sub-tabs **Orders** (Autopilot, Idle, Follow, Level in zone with a
  level-aware zone picker, auto-advance, Autopilot limits), **Professions** (two primaries with
  target rank, secondaries, skill bars, guild coverage), **Crafting** (the bot's recipes with how often its bags
  and the guild storage allow them; Craft / Stop, `UI/Crafting.lua`), **Character**, **Talents** (the talent
  template the bot plays by: class default, manual, the built-in templates of its class and the guild's own, Assign
  with a confirmation because it resets the talents; "Save as template" stores the talents of the character you
  play, `UI/Talents.lua`, docs/specs.md), and **Dungeon role** (the role the bot fills in a dungeon group, tank or
  healer as far as its class can; the talent template it changes to for a run; the gear level of its dungeon set and
  its role store piece by piece; also `UI/Talents.lua`, docs/dungeon-roles.md), and **Combat** (the bot's fight
  profiles, docs/fight-profiles.md: what it fights by and why, its *Solo* and *Group* profile, *Copy to bots...* for
  other bots of its class, *Copy as new profile* from its template, the template or a profile read-only with divider
  lines per relevance band, and *Options for this bot* with its own values; *Edit...* opens the fight profile editor,
  a window of its own: the profiles of the class, both lists with up / down / on-off / remove, add spell and add item,
  the selected rule's settings and conditions, the profile's options, save / discard / assign; also `UI/Talents.lua`). The Orders sub-tab's dungeon text
  counts the squad's roles by the dungeon role first and names each tank and healer with the readiness of its
  dungeon set. The order *Farm item* (docs/orders.md "Farm orders") has a search box in the Orders sub-tab: a name,
  an item id or a shift-clicked item, *Search*, and the list below it offers what creatures drop, with "(from level
  N)" where the bot is too low; *Target stock* ends the order when the guild storage holds that many (0 = it farms
  on and on).
- **Recruitment** — race, class (3.3.5a combinations, Death Knight gate), gender, name, preview.
- **Progression** — renown track and every unlock with its status; gold unlocks are bought with *Buy* (from the guild
  bank, or by the bot setting `Unlocks.PayFrom` the leader's gold; greyed while the payer has too little), and a slot or
  offline tier the guild has more than already shows as *Included*.
- **Storage** (`UI/Storage.lua`, docs/storage.md) — the guild storage: **Items** (categories, search, Take: one stack,
  Shift ten, Ctrl everything; Deposit materials; **Sell junk**: shows what of the storage is junk and its price, sells it
  into your character after your OK; per row **Sell** (one) and **All** (the kind) at the vendor price into your
  character, asking first for All and for green and better items, and **DE** on green and blue weapons and armour: the
  guild's best enchanter in the world disenchants one into the storage, greyed with the reason while nobody has the
  skill; each a line in Recent loot; Alt+click an item in your bags while the tab is open stores that
  stack), **Loot filter** (filter on/off, what happens to loot no rule decides for the bots and for you, auto loot,
  totals, the rules with on/off, up/down, edit, delete and a rule editor with up to four conditions, and below them the
  built-in rows: junk is sold, the rest goes the default way; docs/storage.md "Junk"), **Recent loot**, **Craft
  orders**, **Shopping list** (a minimum stock per item vendors sell: item search, *Drink tiers* / *Food tiers* for the
  vendor waters and breads of level 1-45, per row the stock against the minimum with *Min*, *Pause*, *Remove*; bots buy
  what is missing at vendors they visit anyway, paid from the guild bank; docs/storage.md "Shopping list").
- **Profession window** (`UI/Crafting.lua`) — reagent counts include the guild storage (blue when only the storage
  makes them enough), the recipe list shows how often each recipe can be made with it, and "Create (storage)" /
  "Create All (n)" replace the buttons when the bags alone are not enough: the server starts the craft and takes the
  materials from the storage (intent `CRAFT`, core patch CP-7).
- **Dungeons** — the dungeon run window (docs/orders.md "The dungeon run window"): choose the dungeon, click the
  bots of the group, prepare (what each bot goes as: as it levels, tank or healer with a talent template; which
  elixirs, scrolls, stones, oils and scopes of the guild storage the group may use; how it fights), choose how often
  (1 to 10 runs, or until you stop it) and start; the group is taken into the dungeon. The list on the right shows every run that is going, with *Last run* and *Stop*.
- **Map** — locked until its unlock; content comes with M9.
- **Settings** — two views: **Bot behaviour** (`UI/Behaviour.lua`, docs/bot-settings.md: the guild's bot settings as the server sends them, categories on the left, search over all of them, "Changed only", "Show all options", per row a checkbox / choice / number with - and + and where the value comes from, Reset, Reset category, Reset all) and **Addon** (window scale, minimap button, chat output, away popup, connection/demo mode). The bot detail has the same panel as its **Behaviour** sub-tab for the values of one bot.
- **Control bar** — shown while you play a bot: "Playing: <bot>", Return to leader, switch
  (bots in combat can be picked too: the switch waits for the end of the fight), and
  **Watch / Take over** (`UI/WatchMode.lua`): in watch mode the server's AI plays the bot with
  its order while your client only watches (movement keys locked, "Watching: <bot>" and the bot's
  why line on the bar); Take over gives you the controls back at once, and a swap ends watch mode
  (`docs/take-control.md` §5.12, intent `CONTROL_WATCH`).
- **While you were away** — popup after login with the offline-window summary.

## Code layout

| File | Role |
|---|---|
| `Core.lua` | namespace, saved variables, callback bus, timers, slash commands |
| `Data.lua` | DBC ids, race/class combinations, professions, orders, features, formatting |
| `Codec.lua` | message layer: `OP^key=value`, escaping, typed readers |
| `Transport.lua` | chunk layer, send throttle, reassembly, live/mock backends |
| `State.lua` | client model of the server state + change notifications |
| `Protocol.lua` | handshake, handlers, blocks, requests/ACK, intent API |
| `MockServer.lua` | simulated server speaking the same wire format |
| `UI/*.lua` | widgets, main frame and the screens above |
| `Bindings.xml` | key bindings (labels in `Core.lua`) |

The addon defines one global table `GuildAdventure`; the only other globals are the saved
variables, the slash command and frames named `GuildAdventure*` (the stock templates need named
frames).

## Tests

`addon/tests` runs the addon offline against a 3.3.5a API stub — see
[tests/README.md](tests/README.md).
