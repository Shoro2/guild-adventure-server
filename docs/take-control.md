# Take control

The owner can play any of his guild bots at any time and give the character he played before to the bot AI. Neither
character leaves the world: the one he leaves plays on with its order, the one he takes keeps its place, bags,
spellbook, talents, quest log and pet. This document describes the server side as it is in this repository.

The section numbers follow the internal design document the other documents refer to; sections that only described
client-side work are summarised in §5.3.

## 1. Summary

- **The core already knows the hard part.** Its reconnect-after-disconnect feature attaches a new client session to a
  character that is still in the world and replays the in-world part of the login without the database
  (`WorldSession::HandlePlayerLoginToCharInWorld`). The swap reuses it.
- **The player moves, not the connection.** Every guild character the AI drives has a headless *home session* on its
  real account (a bot account for bots, the owner's account for the leader). The client session borrows the character
  it controls; connection-level state (time sync, latency, addons, security) never moves.
- **mod-playerbots needs no patch.** Its AI is keyed by the player's GUID. The module's `GuildBotHolder` (a
  `PlayerbotHolder`) creates the AI on the character that goes to the AI and deletes it on the one the client takes.
- **The core patches** CP-1 to CP-5 (`docs/core-patches.md`): a logout hook that keeps a character in the world, the
  character's own account for account-scoped saves, a character-list hook, an option to skip the login effect, and
  custom opcodes for an optional client component.

## 3. Findings

### 3.4 The 3.3.5a client

The client fixes its active player when it logs in; a stock client cannot change it in the world. The default way
(Path R, §5.2) therefore goes through the character screen. A stock client drops opcodes above its own table quietly,
so the custom opcodes of CP-5 do nothing on a client that does not know them.

## 4. Design

### 4.2 Home sessions

- Each AI-driven guild character has exactly one home session, made by `GuildBotHolder` with the character's real
  account id.
- When the client takes a character, its home session is *parked* (no player, its queued bot packets dropped, its
  pending callbacks frozen until the character comes back).
- When the character goes back to the AI, it returns to its parked home session.
- A home session is deleted only when the character really logs out (bench, end of the offline window, shutdown).

Invariants after every swap step: a player is referenced by exactly one session and back; per owner at most one
character is attached to the client session; the client-attached character has no `PlayerbotAI` and every other guild
character in the world has one; no swap writes `characters.account`; swaps run on the world thread outside map updates.

### 4.3 Where the code lives

- `ControlSwapService`: request queue, guards, the swap's state machine, path choice, timeouts, logout and disconnect
  handling. Its update runs after the maps (`WorldScript::OnUpdate`).
- `GuildBotHolder`: home sessions, `HandToAI` / `TakeFromAI`, watch mode (§5.12), the placeholder AI (§5.14).
- `ControlSwapScripts.cpp`: the core hooks, forwarded to the service.
- The core: only the patches named in §1.

### 4.4 Path choice

`.ga swap <name> [relog|reload|switch|auto]` or `GuildAdventure.Swap.Path` (default `relog`). With a stock client
`auto` always takes Path R; the other paths need a client component that is not part of this project (§5.3).

## 5. The swap

### 5.1 Common steps

**Guards** (`CanSwap`): the target belongs to the owner's guild and is in the world; neither character is in combat
(`GuildAdventure.SwapAllowedInCombat`), being teleported, on a vehicle or a flight, charmed or possessing, in a duel,
trade or loot window, in a battleground or a dungeon finder queue, or logging out; the character the owner plays is not
in GM mode; no other swap of the owner runs. A refused swap answers with a reason (`SWAP_REFUSED`, docs/protocol.md).

**Hand-over:** the character the owner leaves is stopped (movement, casts, gossip, trade, loot), moved to its home
session, gets its AI and its order (the leader holds its position unless it has an order). The target loses its AI and
is moved to the client session; its per-character account data (macros, bindings) is loaded into the session. Both
characters are saved before anything goes to the client.

### 5.2 Path R: through the character screen

1. The owner presses *Play* in the addon's roster (or `.ga swap <name>`).
2. The server hands his character to the AI (it plays on at once) and sends the client to the character screen.
3. The character-list hook (CP-3) lists the target there, although it lives on a bot account.
4. The owner selects it and enters the world: the core's in-world login attaches the client to the character that is
   already in the world. No database load, no logout of either character.

Path R works at any distance, in instances and on ships. Back to the leader and from bot to bot work the same way.

### 5.3 Paths that need a client component

The module also has a world-reload path (Path W: one loading screen, no character screen) and an in-world switch
(Path S: no loading screen). Both need a client-side component that changes the 3.3.5a client; it is **not part of
this project**, and the module works without it:

- `auto` takes Path R when no client component announced itself (`.ga client` shows what the connection announced).
- An explicit `.ga swap <name> reload` works with a stock client, but the client keeps the old character's name and
  class for the `player` unit; the server tells the owner so.
- `.ga swap <name> switch` is refused without the client component.
- The addon writes the CVar `gaSwapTarget` before the character screen for an optional auto-enter; a stock client
  ignores it, and the owner selects the character himself.

### 5.6 Logout, disconnect, crash

- **Logout while controlling a bot:** the bot goes back to the AI and plays on; the leader logs out with the owner,
  unless it has an order (then it plays on in the offline window, docs/orders.md "The leader").
- **Disconnect:** the same once the connection is gone (after the core's tolerance for a reconnect).
- **Crash:** there is no persisted swap state. After a restart every guild character is offline and starts again as a
  bot (inside the owner's offline window they log in by themselves).

### 5.7 Commands

| Command | Effect |
|---|---|
| `.ga swap <name> [relog\|reload\|switch\|auto]` | Take control of a guild bot (or return to the leader) |
| `.ga watch [on\|off]` | Watch mode (§5.12) |
| `.ga status` | The swap in progress, the guild bot counts, what the client announced |
| `.ga bot login <name>` / `.ga bot logout <name>` / `.ga bot list` | Log a character in or out as a guild bot; the guild bots and their state |
| `.ga client` / `.ga client ping` | What an optional client component announced on the connection; a round trip to it |

### 5.12 Watch mode

While the owner controls a guild bot he can let the AI play it on with its order and only watch: `.ga watch` or the
control bar's *Watch / Take over*. The character stays on the client session (no swap, no loading screen); the client's
movement keys are locked the way a fear effect locks them (re-sent after teleports, flights and crowd control). *Take
over* gives the controls back at once; a swap, a logout or a lost connection end watch mode. mod-playerbots withholds
object updates from every player that has an AI; `ObjectUpdateScripts.cpp` lets a watched character's client have them.
A login into a leader the AI was playing starts in watch mode (`GuildAdventure.Leader.WatchOnLogin`).

### 5.13 A character the AI left under the ground

An AI-driven character has no client physics and can stand under the terrain or keep a fall that never ended. Before
every take-over `GuildBotHolder::SettleForClient` clears that movement state and puts the character on the floor it
stands on or under, and the next seconds of the take-over are logged with position, floor and fall.

### 5.14 Placeholder AI

A character the owner plays by hand inside a party of bots (the guild leader in a dungeon run) carries a playerbots AI
that never runs. mod-playerbots then treats the party as bots only and does not make the owner's character the bots'
master; the module stands in for mod-playerbots' AI update hook for such a character (`GuildBotHolder::StartPlaceholder`).

## 6. State that moves with the character

### 6.4 Account-scoped data

Bots live on bot accounts and stay there while the owner plays them. Data the core saves through the session's account
(instance lock times) would land on the owner's account; CP-2 writes it under the character's own account. Bots that
share a bot account share its account-wide limits.
