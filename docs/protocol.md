# GADV protocol (addon <-> mod-guild-adventure)

Version **1**. This document is the contract between the client addon
(`addon/GuildAdventure`) and the server module (`modules/mod-guild-adventure`).
The server is authoritative for everything; the addon only renders state and sends intents.

| Side | Status |
|---|---|
| Addon | Implemented against this spec, including a built-in mock server (`MockServer.lua`) that speaks the same wire format. Evidence: see the design log §10. |
| Server | Implemented in PR #5 (`modules/mod-guild-adventure/src/Protocol/`; T1, codec checked against `protocol-vectors.txt`, not run with the addon yet). The transport assumptions in §1 are confirmed only once the two run together (spike **S3**). Until later milestones the server sends `role=none` and an empty `spec`, sends `sk` only while the bot is in the world. Since 2026-11-14 `UNLOCK_BUY` buys gold unlocks (it answered `NOT_AVAILABLE` before). Since M4 the orders have behaviour, `why` is the order planner's live line and `xph`/`gph` are rolling one-hour rates ([orders.md](orders.md)). |

Contents: [1 Transport](#1-transport) · [2 Chunk layer](#2-chunk-layer) ·
[3 Message layer](#3-message-layer) · [4 Semantics](#4-semantics) ·
[5 Handshake and sync](#5-handshake-and-sync) · [6 Client to server](#6-client--server-opcodes) ·
[7 Server to client](#7-server--client-opcodes) · [8 Enumerations](#8-enumerations) ·
[9 Server rules](#9-server-rules-validation-and-limits) · [10 Versioning](#10-versioning) ·
[11 Examples](#11-wire-examples) · [12 Mock server](#12-mock-server)

---

## 1. Transport

Addon messages, prefix **`GADV`**, sent as a *whisper to the player's own character*.

**Client → server.** `SendAddonMessage("GADV", chunk, "WHISPER", <own name>)`.
The client sends `CMSG_MESSAGECHAT` with type `CHAT_MSG_WHISPER`, language `LANG_ADDON`,
target = own name, text = `"GADV\t" .. chunk`.

*Own name* is the name of the character the client plays, resolved through
`UnitGUID("player")`: the server's `CONTROL_STATE` (`cn` when `g` is our GUID, `ln` when
`leader` is), else the roster entry for our GUID, else `UnitName("player")`. Measured on the
real client (2026-11-09, take-control T2): after a world-reload swap (Path W, §9)
`UnitGUID("player")` is the new character's, but `UnitName("player")` and `UnitClass` still
report the login character, also after `/reload`. Whispering `UnitName("player")` would then
address the old, now AI-driven character. The module additionally treats an addon whisper
to any guild character owned by the sender's account as the sender's own traffic (PR #5).
Addon whispers to a guild character the AI drives are not delivered (nobody reads them there);
a guild character its owner plays right now gets them like any other player. Text that starts
with `GADV\t` is never delivered to another player, whoever the receiver is.

Server side (checked against AzerothCore `Handlers/ChatHandler.cpp` and
`Player::Whisper` in a current AzerothCore checkout; re-verify on the Playerbot branch in S3):
- `LANG_ADDON` is valid for `CHAT_MSG_WHISPER` as long as `AddonChannel` is enabled in
  `worldserver.conf` (it is by default).
- The whole text (`"GADV\t" .. chunk`) is dropped if it is longer than **255** bytes.
- Whispering yourself skips the `ChatLevelReq.Whisper` check (`receiver != sender`).
- Addon traffic counts against `ChatFlood.AddonMessageCount` (default 100 per
  `ChatFlood.AddonMessageDelay` = 1 s); exceeding it mutes the player. The client therefore
  throttles itself (§9).
- **Intercept hook:** `PlayerScript::OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang,
  std::string& msg, Player* receiver)` is called from `Player::Whisper` before delivery. The
  module handles the message when `type == CHAT_MSG_WHISPER && lang == LANG_ADDON &&
  receiver == player && msg` starts with `"GADV\t"`, and **returns `false`** so the whisper is
  never delivered back to the client.

**Server → client.** Build the same packet the core builds for an addon whisper and send it
to the player only:

```cpp
WorldPacket data;
ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, "GADV\t" + chunk);
player->SendDirectMessage(&data);
```

The client fires `CHAT_MSG_ADDON(prefix = "GADV", message = chunk, distribution = "WHISPER",
sender = <own name>)`. The addon accepts a chunk only if the prefix is `GADV`, the distribution
is `WHISPER` and the sender (realm suffix stripped) is one of the owner's own characters: the
own name above, `UnitName("player")`, the leader (`CONTROL_STATE^ln`) or a roster bot. Right
after a swap the client may not yet know which of them it plays, and the server always speaks
as the character the session plays now. After a control swap the active character changes,
so the server must use the **currently controlled** player as sender and receiver.

**Echo detection.** If the module is not installed, the core delivers the client's own whisper
back to it. Every chunk carries a direction marker (§2), so the addon ignores its own `C` chunks;
it uses them only to report "server module not installed" instead of a plain timeout.

---

## 2. Chunk layer

One logical message is split into one or more chunks. Every addon message is exactly one chunk:

```
<dir><id>,<seq>,<total>:<body>
```

| Part | Meaning |
|---|---|
| `dir` | `C` = client → server, `S` = server → client. A receiver drops chunks with the wrong direction. |
| `id` | Message id, decimal `1..9999`, per sender and direction; increments per logical message and wraps from 9999 to 1. |
| `seq` | Chunk number, `1..total`. |
| `total` | Number of chunks of this message, `1..64`. A message that needs more than 64 chunks is a bug on the sender side. |
| `body` | The next slice of the encoded logical message (§3). |

Rules:
- A chunk (header + body) is at most **240 bytes**, so `"GADV\t" .. chunk` stays at 245 ≤ 255.
  The header is at most 12 bytes (`C9999,64,64:`); senders slice the body in pieces of at most
  **228 bytes**.
- Bodies are sliced on **UTF-8 boundaries**: a cut never falls in front of a continuation byte
  (`0x80..0xBF`). Slicing may happen anywhere else, including inside an escape sequence (§3),
  because the receiver concatenates the bodies before decoding.
- A sender sends all chunks of one message **contiguously and in order**, and sends logical
  messages in the order they were produced.
- The receiver keys partial messages by `id`, ignores duplicate `seq` values, and discards a
  partial message **15 s** after its first chunk arrived.
- A chunk that does not match the header pattern, or has `seq > total`, `total > 64` or
  `seq < 1`, is dropped silently (logged in debug mode).

---

## 3. Message layer

```
<OP>[^<key>=<value>]*
```

- `OP` is an upper-case opcode (`[A-Z_]+`). Field separator is `^`. A field is split at its
  **first** `=`; a field without `=` is a key with an empty value.
- Keys are short lower-case identifiers. Field order is irrelevant (the addon sorts keys when it
  encodes, which only matters for tests).
- **Escaping** (applies to keys and values, one pass, left to right):

  | Raw | Escaped |
  |---|---|
  | `~` | `~~` |
  | `^` | `~c` |
  | `\|` | `~p` |
  | newline (LF) | `~n` |
  | carriage return | `~r` |
  | tab | `~t` |
  | NUL | `~0` |

  `|` is escaped so a payload never contains WoW chat escape sequences, even when it is printed
  for debugging. Decoders map an unknown `~x` sequence to itself.
- **Value types** (the schema in §6/§7 says which type a key has):
  - `int` — decimal integer, optional leading `-`, no exponent, no thousands separator.
    Copper amounts can exceed 2^31; the server writes them as 64-bit integers and the addon
    reads them as Lua numbers (exact up to 2^53).
  - `bool` — `1` or `0`.
  - `str` — escaped text, UTF-8.
  - `list` — comma-separated items. Items are integers or enum tokens and never contain `,`,
    `:` or text.
  - `tuples` — comma-separated items, each item is colon-separated integers
    (e.g. `171:120:150,182:95:150`).
- Missing keys mean "not sent" (keep the old value for merges, use the default for requests).

---

## 4. Semantics

- **Records merge.** A record message (`STATE`, `BOT`, `CONTROL_STATE`, `UNLOCK`, `ZONE`) is
  an upsert: only the keys present are changed. The server may send a `BOT` with just
  `g` and the changed fields as a delta.
- **Blocks replace.** `<X>_BEGIN … <X>_END` frames a full list (`ROSTER`, `ZONES`, `UNLOCKS`,
  `EVENTS`, `AWAY`). Records between the markers replace the client's whole list on `_END`.
  `_BEGIN` carries `n` = number of records that follow (informational; the client trusts
  `_END`). The server must not send records of the same type that are *not* part of the block
  between `_BEGIN` and `_END`.
- **Requests.** Client intents may carry `r` (request id, decimal, per client session). The
  server answers every request that carries `r` with exactly one `ACK^r=…` or
  `ERROR^r=…`, **after** it has pushed the resulting record changes. The addon times a request
  out after 10 s. Exception: control swaps are acknowledged when they *start* (§9), because a
  swap can outlive the client session that requested it.
- **Pushes.** The server pushes changes as they happen (`BOT` deltas, `STATE`, `EVENT`,
  `CONTROL_STATE`, `UNLOCK`); the client never polls. Full blocks are only sent during the
  sync (§5) or when the client asks with `GET`.
- **Unknown input is ignored.** Unknown opcodes, unknown keys and unknown enum values are
  ignored by both sides (forward compatibility, §10). Client opcodes received by the client and
  server opcodes received by the server are ignored.
- **Identifiers.** `g` is always a character's low GUID (`characters.guid`) in decimal. Zone
  ids are AreaTable zone ids (the `zone` a player reports, not sub-areas). Map ids are Map.dbc
  ids. Race/class/skill ids are the DBC ids (§8).
- **Time.** All times are Unix seconds (server clock). `WELCOME^now` lets the client compute
  its offset to the server clock.

---

## 5. Handshake and sync

The client sends `HELLO`:
- 2 s after `PLAYER_ENTERING_WORLD` while not connected, and 2 s after every
  `PLAYER_ENTERING_WORLD` that follows a swap which kept the UI loaded (`UnitGUID("player")`
  or the character name differs from the last `HELLO`, or a swap is still `busy`);
- when the UI opens while not connected;
- after every `CONTROL_STATE` whose `g` changed (the active character changed);
- on `/ga reconnect`.

If no `WELCOME` arrives within 5 s the client sends `HELLO` again, waits 10 s, sends a third
`HELLO`, waits 20 s, and then shows "no server found" (or "server module missing" if it saw its
own echo).

The server answers a valid `HELLO` with this **sync sequence**, in order:

1. `WELCOME`
2. `STATE`
3. `CONTROL_STATE`
4. `ZONES_BEGIN` … `ZONES_END` — only if the client's `zv` differs from the server's zone
   template version
5. `UNLOCKS_BEGIN` … `UNLOCKS_END`
6. `ROSTER_BEGIN` … `ROSTER_END`
7. `EVENTS_BEGIN` … `EVENTS_END` — the newest ≤ 30 events, oldest first
8. `AWAY_BEGIN` … `AWAY_END` — only if an offline-window summary is pending for this owner
9. `LOOT_SETTINGS`, `LOOT_RULES_BEGIN` … `LOOT_RULES_END` — the guild's loot filter (§7.11)
10. `STORAGE_BEGIN` … `STORAGE_END` — the guild storage (§7.10); it can be long
11. `AUCTION_RULES_BEGIN` … `AUCTION_RULES_END` — the storage's auction rules (§7.14)

`SPECS_BEGIN` … `SPECS_END` — the talent templates (§7.16) — is sent after the away summary, before the loot filter.
`PERKS_BEGIN` … `PERKS_END` (§7.15) and `MILESTONES_BEGIN` … `MILESTONES_END` (§7.18) follow the unlocks, before the
roster. After the talent templates come the bot behaviour settings (§7.19): `SETDEFS_BEGIN` … `SETDEFS_END` only if the
client's `HELLO^sv` differs from the server's schema version, and always `SETVALS_BEGIN` … `SETVALS_END`.

The client considers itself connected after `WELCOME` and the UI fills in as the blocks arrive.
A `HELLO` with an unsupported version gets `ERROR^code=BAD_VERSION` instead.

---

## 6. Client → server opcodes

All intents accept an optional `r` (int). Defaults apply to keys that are missing.

| Opcode | Fields | Server reply |
|---|---|---|
| `HELLO` | `v` int protocol version · `ui` str addon version · `zv` int cached zone-template version (0 = none) · `sv` int cached bot-settings schema version (0 = none, §7.19) | sync sequence (§5) or `ERROR BAD_VERSION` |
| `GET` | `what` = `state` \| `control` \| `roster` \| `bot` \| `zones` \| `unlocks` \| `events` \| `storage` \| `loot` \| `lootlog` \| `recipes` \| `auctions` \| `shopping` \| `perks` \| `milestones` \| `settings` \| `dungeons` \| `fightprofiles` \| `fightprofile` · `g` int (for `bot`, `fightprofiles`, `fightprofile`; for `recipes` the guild bot, default the character the client plays) · `id` int (for `fightprofile`, 0 = the bot's template) | the record or full block; `zones` is always sent, `settings` sends the schema and the values |
| `ORDER_SET` | `g` int · `o` order type (§8.1) · `zone` int (`LEVEL_ZONE`, `GRIND`, `QUEST`, `GATHER`) · `aa` bool auto-advance · `lc` int AUTO level cap (0 = none) · `cont` int AUTO continent map id (-1 = anywhere) · `nd` bool AUTO avoid dungeons · `gcap` int AUTO max copper per trip (0 = no limit) | `BOT` delta, `EVENT ORDER`, `ACK` |
| `BOT_STATE` | `g` int · `s` = `active` \| `benched` | `BOT` delta, `STATE`, `ACK` |
| `PROF_SET` | `g` int · `p1`, `p2` int primary skill ids (0 = none) · `t1`, `t2` int target skill (75/150/225/300/375/450) · `sec` list of secondary skill ids to learn | `BOT` delta, `ACK` |
| `RECRUIT` | `race` int · `class` int · `gender` int (0 male, 1 female) · `name` str | `BOT` (new), `STATE`, `EVENT RECRUIT`, `ACK` |
| `CONTROL_TAKE` | `g` int — the bot to play | `CONTROL_STATE^busy=1`, then `ACK` (swap accepted and started), or `ERROR SWAP_REFUSED` with `reason`; the result follows as `CONTROL_STATE` with the new `g` (see §9) |
| `CONTROL_RETURN` | — (switch back to the leader) | same as `CONTROL_TAKE` |
| `ORDER_SET` with `o` = `DUNGEON` | `g` int the bot that leads the group (its squad) · `zone` int the dungeon's id (§7.20) · `runs` int how often the dungeon is run (optional, 1 to `GuildAdventure.Dungeons.MaxRuns`; the `BOT` record then carries `druns` / `drund`) | as `ORDER_SET`; the `BOT` delta carries `oz` = the dungeon's id and `ozn` = its name. Refused with `DUNGEON_INVALID` and the reason: the dungeon cannot be run, the bot follows a squad leader, the guild leader, somebody of the group is not ready. docs/orders.md "Dungeon runs" |
| `ORDER_SET` with `o` = `FARM` | `g` int the bot · `zone` int the item to farm · `n` int (optional) the target stock: the order ends when the guild storage holds that many, 0 = none | as `ORDER_SET`; the `BOT` delta carries `oz` = the item's id, `ozn` = its name, `ofn` = the target stock and `ofs` = what the guild storage holds of the item. Refused with `FARM_INVALID` and the reason when the item is unknown or no creature bots can grind drops it for the guild's side. docs/orders.md "Farm orders" |
| `SHOP_SET` | `e` int the item · `min` int the minimum stock (0-100000) · `on` bool (false pauses it) | the block `SHOPPING` (§7.25), `ACK`; or `ERROR SHOP_REFUSED` with `reason` `NOT_SOLD` (no vendor of the guild's side sells it for gold) / `NOT_STORED` (ammunition, it binds, the storage refuses it) / `INVALID` / `LIMIT` (60 items) |
| `SHOP_DEL` | `e` int | the block `SHOPPING`, `ACK`; or `ERROR SHOP_REFUSED` `NOT_FOUND` |
| `SHOP_SEARCH` | `q` str a text of at least two letters or an item id | the block `SHOP_ITEMS` (§7.25), `ACK`; or `ERROR BAD_REQUEST` |
| `FARM_SEARCH` | `q` str a text of at least two letters (items whose name holds it) or an item id · `g` int (optional) the bot the search is for | the block `FARM_ITEMS` (§7.23), `ACK`; or `ERROR BAD_REQUEST` for a shorter text |
| `ORDER_SET` for the leader | `g` = the leader's guid, `o` = `HOLD` (no order, the default) or any order above, with its keys (`aa` is ignored: the leader always auto-advances) | `LEADER` delta, `EVENT ORDER`, `ACK`; `HOLD` also ends watch mode of the leader (`CONTROL_STATE^watch=0`). docs/orders.md "The leader" |
| `CONTROL_WATCH` | `on` bool — watch mode of the character the client plays: `1` lets the server's AI play it with its order while the client watches, `0` takes the controls back | `CONTROL_STATE` (with `watch`), the bot's `BOT` delta, `ACK`; or `ERROR WATCH_REFUSED` with `reason` (see §9 "Watch mode") |
| `UNLOCK_BUY` | `id` int unlock id | `STATE` (slots, `bank`, `ulgold`), the `UNLOCKS` block (covered tiers change too), `EVENT UNLOCK`, `ACK`; or `ERROR` `LOCKED` (renown level not reached) \| `COVERED` (the guild has as many slots / as long a window already) \| `NOT_ENOUGH_GOLD` (the message names what the payer has) \| `BAD_REQUEST` (unknown, owned) |
| `AWAY_ACK` | — (the player closed the "while you were away" summary) | none; the server clears the pending summary |
| `STORAGE_TAKE` | `e` int item entry · `rp` int random property (0 = none) · `n` int how many (as many as fit) | `STORAGE_DELTA` (within a second), `ACK`; or `ERROR STORAGE_REFUSED` with `reason` (the server's reason text, e.g. `not enough bag space`) |
| `STORAGE_DEPOSIT` | `all` bool — every material of the played character's bags (stackable trade goods and gems, recipes, stackable food); or `b` int bag (0 backpack, 1-4) and `s` int slot (1-based) — that one stack | `NOTICE` (`all`), `STORAGE_DELTA`, `ACK`; or `ERROR STORAGE_REFUSED` with `reason` (`soulbound`, `quest item`, `enchanted`, `nothing`, …) |
| `STORAGE_SELL` | `e` int item entry · `rp` int random property · `n` int how many (0 = all of the kind) | sold at once at the vendor price, the copper into the guild bank (docs/storage.md "Selling and disenchanting", §10): `NOTICE`, `STORAGE_DELTA`, a `LOOT_LOG` line, `ACK`; or `ERROR STORAGE_REFUSED` with `reason` = `not_guild` \| `no_price` (no vendor buys it) \| `gold_limit` \| `nothing` |
| `STORAGE_RESERVE` | `e` int item entry · `rp` int random property · `n` int the reserve (0-1000000, 0 = none) | how many of the kind the bots leave in the guild storage (2026-11-13, docs/storage.md "Reserves"): `NOTICE`, a `STORAGE_DELTA` of the kind when the storage holds it, `ACK`; or `ERROR STORAGE_REFUSED` with `reason` = `not_guild`, `ERROR BAD_REQUEST` |
| `STORAGE_DELETE` | `e` int item entry · `rp` int random property · `n` int how many (0 = all of the kind) | only for what no vendor buys: destroyed (2026-11-13, docs/storage.md "Selling and disenchanting"): `NOTICE`, `STORAGE_DELTA`, a `LOOT_LOG` line, `ACK`; or `ERROR STORAGE_REFUSED` with `reason` = `not_guild` \| `has_price` (a vendor buys it: sell it) \| `nothing` |
| `STORAGE_DISENCHANT` | `e` int item entry · `rp` int random property | one of the kind disenchanted at once by the guild's best enchanter in the world, the materials into the storage: `NOTICE`, `STORAGE_DELTA`, a `LOOT_LOG` line, `ACK`; or `ERROR STORAGE_REFUSED` with `reason` = `not_guild` \| `not_disenchantable` (only green and blue weapons and armour) \| `no_enchanter` \| `skill` \| `nothing` |
| `STORAGE_SELL_JUNK` | `sell` bool (false: only ask) | `STORAGE_JUNK` (what is junk, or what was sold), then with `sell`: `NOTICE`, `STORAGE_DELTA`, `LOOT_SETTINGS`, `LOOT_RULES`; `ACK`; or `ERROR STORAGE_REFUSED` with `reason` = `not_guild` \| `off` (the guild sells no junk) \| `nothing` \| `gold_limit` |
| `LOOT_RULE_SET` | `id` int (0 = a new rule) · `pos` int position (0 = at the end; for an existing rule: move there) · `act` int action (§8.8) · `en` bool · `lg` int logic bits · `c` str the conditions (§7.11) | `LOOT_SETTINGS`, the `LOOT_RULES` block, `ACK`; or `ERROR LOOT_REFUSED` with `reason` = `INVALID` \| `LIMIT` \| `ACTION` \| `NOT_FOUND` |
| `LOOT_RULE_DEL` | `id` int | the same |
| `LOOT_RULE_MOVE` | `id` int · `pos` int (1-based) | the same |
| `LOOT_RULE_ENABLE` | `id` int · `en` bool | the same |
| `LOOT_OPTIONS` | `on` bool loot filter on · `dai` int default action for a character the AI plays · `dpl` int default action for the character the owner plays · `al` bool auto loot (all optional: missing = unchanged) | `LOOT_SETTINGS`, `LOOT_RULES`, `ACK`; or `ERROR LOOT_REFUSED` `ACTION` |
| `CRAFT` | `s` int recipe spell id · `n` int how often (1-100) · `g` int guild bot (default: the character the client plays) · `stop` bool — end the bot's running queue instead | `NOTICE` ("X crafts n x Y."), `ACK`, a `NOTICE` when the queue ends; or `ERROR CRAFT_REFUSED` with `reason` = `UNKNOWN_SPELL` \| `NOT_A_RECIPE` \| `NOT_KNOWN` \| `DEAD` \| `BUSY` \| `REAGENTS`; `UNKNOWN_BOT` for a `g` that is not a guild bot in the world |
| `AUCTION_RULE_SET` | `e` int item · `rp` int random property · `keep` int stays in the storage · `stack` int items per auction (0 = up to 20, at most the item's stack size) · `price` int copper per item (0 = the default price) · `on` bool | the `AUCTION_RULES` block, `ACK`; or `ERROR AUCTION_REFUSED` with `reason` = `NOT_AUCTIONABLE` \| `INVALID` \| `LIMIT` |
| `AUCTION_RULE_DEL` | `e` int · `rp` int | the same (`reason` `NOT_FOUND`) |
| `AUCTION_CAT_SET` | `cat` str a storage category (`cloth`, `leather`, `metal`, `herb`, `meat`, `elemental`, `enchanting`, `gems`, `trade`, `recipes`, `consumables`, `weapons`, `armor`, `bags`, `darkmoon`, `other`) · `keep` int per kind · `stack` int (0 = up to 20) · `on` bool | the auction blocks (§7.14), `ACK`; or `ERROR AUCTION_REFUSED` with `reason` = `CATEGORY` \| `INVALID` \| `LIMIT` |
| `AUCTION_CAT_DEL` | `cat` str | the same (`reason` `NOT_FOUND`) |
| `PERK_BUY` | `id` int a perk (§7.15) | `STATE` (the points), the `PERKS` block, an `EVENT` `PERK`, `ACK`; or `ERROR` `UNKNOWN_PERK` \| `NOT_AVAILABLE` (needs core patch CP-8) \| `MAX_RANK` \| `NO_POINTS` |
| `SPEC_SET` | `g` int guild bot · `t` int talent template: `0` = the default of its class, `1` = manual (the server spends no points), else a template id of §7.16 | the bot's `BOT` delta (`spt`, `spec`, `role`, later `tal`), `ACK`; or `ERROR SPEC_INVALID` (no such template, a template of another class). The bot's talents are reset for free and learned again by the template, at once when it is in the world and out of combat, else at its next login (docs/specs.md) |
| `SPEC_SAVE` | `n` str name (1-40 characters) · `p` int purpose (0 leveling, 1 dungeon and raid, 2 raid, 3 PvP) | the `SPECS` block, `ACK`; or `ERROR SPEC_INVALID` (no talents, bad name, 40 templates already). Saves the talents of the character the client plays as a template of the guild for that character's class |
| `SQUAD_SET` | `g` int guild bot · `l` int the guild bot it follows as a squad member, `0` = out of its squad | the `BOT` deltas of the bot, its new and its old leader (`sq`, `sqs`, `sqm`), `ACK`; or `ERROR SQUAD_INVALID` (`msg`: the leader follows someone, the bot leads a squad itself, the squad is full, too many levels apart, squads switched off). Applied at once (docs/orders.md "Squads") |
| `FIGHT_ASSIGN` | `g` int guild bot · `slot` int `0` solo, `1` group · `id` int a fight profile of the guild of its class, `0` = the default; or instead `to` intlist guild bots of its class ("Copy to bots": they get its two slots and its own option values) | the bot's `BOT` delta (`fp`, `fpg`), block `FPROFILES`, `ACK`; or `ERROR NOT_OWNER` \| `BAD_REQUEST` \| `FIGHT_INVALID` (`msg`: no profile of its class with that id, nobody to copy to). Docs: docs/fight-profiles.md, §7.26 |
| `FIGHT_OPTION` | `g` int guild bot · `id` int a fight profile · `k` str an option's key · `v` str a number, or `def` bool back to the profile's value | blocks `FPROFILE` and `FOPTS` of that profile, `ACK`; or `ERROR FIGHT_INVALID` (no such option, outside its range) \| `BAD_REQUEST` |
| `FIGHT_COPY` | `g` int guild bot in the world · `n` str a name (optional) · `from` int a profile of the guild of its class (optional; the bot then need not be in the world, the name defaults to "<name> copy") | a new fight profile of the guild from the bot's template (its class and talent tree) or a copy of `from` (a wowbot335 template too), block `FPROFILES`, `ACK`; or `ERROR FIGHT_INVALID` (not in the world, 20 profiles of its class already, no such profile) |
| `FIGHT_SAVE` | `g` int guild bot · `id` int a fight profile of the guild of its class · `n` str its name · `note` str · `rc` int the rules, `r1`..`rN` str each a rule (§7.26) · `oc` int the options, `o1`..`oN` str each an option (§7.26) | the whole profile replaced as the editor has it: the bots on it get their `BOT` delta, blocks `FPROFILES`, `FPROFILE` and `FOPTS` of that profile, `ACK`; or `ERROR FIGHT_INVALID` (`msg` "Not saved: ..." with up to four reasons, a rule named as `rule i` counted over list 0 and then list 1 from 0; more than 60 rules or 12 options; "Not saved: a template is read-only ..." for a wowbot335 template) |
| `FIGHT_DEL` | `g` int guild bot · `id` int a fight profile of the guild (not a template) | the profile is deleted, the bots on it play the default again (their `BOT` delta) and lose their own values of its options, block `FPROFILES`, `ACK`; or `ERROR FIGHT_INVALID` \| `BAD_REQUEST` |
| `ROLE_SET` | `g` int guild bot · `role` str `none` \| `tank` \| `heal` · `t` int the talent template of the role, `0` = the default of its class for it | the bot's `BOT` delta (`dr`, `drt`, `drn`, `drp`), `ACK`; or `ERROR ROLE_INVALID` (`msg`: its class cannot fill the role, no such template, the bot wears its dungeon set right now). Docs: docs/dungeon-roles.md, §7.22 |
| `SPEC_DEL` | `t` int a template of the guild | the `SPECS` block, `BOT` deltas of the bots that played by it (back to the class default), `ACK`; or `ERROR SPEC_INVALID` |
| `SETTING_SET` | `g` int scope: 0 = the guild's values, else a guild character (the leader or a roster bot) · `v` tuples `id:value` to store · `del` list of setting ids back to the guild's value (character) or the default (guild) · `all` bool drop every value of the scope first (all optional) | the scope's `SETVAL` record, `ACK`; or `ERROR SETTING_INVALID` with `reason` = `UNKNOWN` \| `RANGE` \| `GUILD_ONLY` \| `NOT_MEMBER`. All or nothing: one bad item and nothing changes (§7.19) |

Order-specific keys that are not relevant for the chosen order are ignored. AUTO limit keys
(`lc`, `cont`, `nd`, `gcap`) require the `auto_limits` feature; when it is locked the server
ignores them and keeps the defaults. Reserved for later orders: `skill` (`GATHER`),
`inst` + `grp` (`DUNGEON`). (`CRAFT` takes none: crafting one recipe n times is the intent `CRAFT`, §6.)

---

## 7. Server → client opcodes

### 7.1 Control and errors

| Opcode | Fields |
|---|---|
| `WELCOME` | `v` int protocol version the server speaks · `min` int oldest client version it accepts · `srv` str module version · `now` int server time · `zv` int zone-template version |
| `ACK` | `r` int |
| `ERROR` | `r` int (if the failed message had one) · `code` (§8.5) · `reason` (for `SWAP_REFUSED`, §8.6) · `msg` str human-readable English text |
| `NOTICE` | `msg` str · `kind` = `info` \| `warn` — free text shown to the player |

### 7.2 `STATE` — guild overview (record)

| Key | Type | Meaning |
|---|---|---|
| `guild` | str | WoW guild name |
| `fac` | `A` \| `H` | guild faction (same as the leader, D9) |
| `rl` | int | renown level (1 = start) |
| `rp` | int | renown points earned inside the current level |
| `rn` | int | points needed for the next level (0 = max level reached) |
| `rt` | int | total renown ever earned |
| `slots` | int | unlocked bot slots |
| `used` | int | bots in the roster (benched bots keep their slot) |
| `max` | int | hard slot maximum (`GuildAdventure.MaxSlots`, 40) |
| `ow` | int | current offline window in minutes |
| `owmax` | int | largest offline window that can be unlocked, minutes |
| `bank` | int | guild bank money in copper (pays gold-cost unlocks; since 2026-11-13 fed by the guild tax, the storage's sales and auctions, and it pays the guild's part of the bots' vendor purchases: docs/storage.md §10) |
| `tax` | int | the guild tax in percent (bot setting `Guild.TaxPercent`): that share of every gain of the roster bots goes into the guild bank, each gain booked at once (`bank` follows a moment later) |
| `ulgold` | int | what the guild can pay for a gold unlock right now, in copper (2026-11-14): the guild bank, the guild leader's purse (only while the leader is in the world), or both, by `ulfrom` |
| `ulfrom` | `bank` \| `bankleader` \| `leader` | who pays for a gold unlock (bot setting `Unlocks.PayFrom`): the guild bank; the guild bank and what it lacks from the leader's purse; the leader's purse |
| `feat` | list | unlocked feature keys (§8.4) |
| `orders` | list | order types the server accepts right now (§8.1) |
| `dk` | bool | Death Knight recruits unlocked (a guild member reached level 55) |
| `ach` | int | guild achievements: achievements any guild character (leader or bot) has earned, each counted once; every member has them in its own achievement list too |
| `achp` | int | the achievement points of those achievements |
| `achr` | int | the renown they gave the guild (part of `rt`) |
| `ms` | int | tiers of the tiered guild achievements the guild reached (§7.18) |
| `msa` | int | tiers the level cap allows, over every series |
| `msp` | int | guild points those tiers gave |
| `msr` | int | the renown they gave (part of `rt`) |
| `gp` | int | guild points to spend on perks: `achp` + `msp` + `gpg` - `gps` (§7.15) |
| `gps` | int | guild points spent on perks |
| `gpg` | int | guild points a GM granted (tests) |
| `stk` | int | kinds of items in the guild storage (docs/storage.md) |
| `sti` | int | items in the guild storage |

### 7.3 `CONTROL_STATE` — who the client is playing (record)

| Key | Type | Meaning |
|---|---|---|
| `leader` | int | the leader's GUID |
| `ln` | str | the leader's name |
| `g` | int | GUID of the character the client controls now (`== leader` when playing the leader) |
| `cn` | str | its name |
| `busy` | bool | a swap is in progress (the UI disables control buttons) |
| `path` | `relog` \| `reload` \| `switch` | optional, with `busy=1`: how the running swap reaches the target — `relog` = warm relog (Path R, character screen + loading screen), `reload` = world reload (Path W, loading screen, UI stays loaded), `switch` = in-world switch (Path S). The server picks the path (module setting relog / reload / auto); the addon only uses it for the wording of its notice |
| `watch` | bool | watch mode: the server's AI plays the character `g` (a guild bot the owner controls) and the client only watches (§9 "Watch mode"); always `0` on the leader and while a swap runs. Servers before watch mode leave it out (= `0`) |

**`CONTROL_PENDING^g=<guid>^cn=<name>`** (not a record) — warm relog only (Path R,
`docs/take-control.md`): sent once, right before the server moves the client to the character
screen (`LeavingWorld`), naming the character the client is about to enter. The addon stores
`cn` in the client CVar **`gaSwapTarget`** (`RegisterCVar` + `SetCVar`) so the patched GlueXML
can select that character and press "Enter World" itself; the addon resets the CVar to `""`
on every `PLAYER_ENTERING_WORLD` and on every `CONTROL_STATE` with `busy=0` it receives while
still in the world (a swap that ended without the relog), so a stale value never auto-enters a
later login. Whether a
CVar set from FrameXML is readable in GlueXML is experiment E5; the message is harmless if
it is not.

### 7.4 `BOT` — one roster entry (record; `g` is required)

| Key | Type | Meaning |
|---|---|---|
| `g` | int | bot GUID |
| `n` | str | name |
| `race`, `class` | int | DBC ids (§8.7) |
| `gender` | int | 0 male, 1 female |
| `slot` | int | roster slot 1..40 |
| `lvl` | int | level |
| `xp`, `xpmax` | int | XP into the level and XP needed for the next level (`xpmax` 0 at the level cap) |
| `s` | enum | state (§8.2) |
| `zone` | int | current zone id |
| `zn` | str | current zone name (the client has no zone-name lookup) |
| `map` | int | current map id |
| `o` | enum | current order (§8.1) |
| `oz` | int | order zone id (0 = none) |
| `ozn` | str | order zone name |
| `aa` | bool | auto-advance to the next zone when out-levelled |
| `lc`, `cont`, `nd`, `gcap` | int/int/bool/int | AUTO limits, same meaning as in `ORDER_SET` |
| `why` | str | one-line explanation of what the bot does right now ("Questing in Westfall: 6 eligible quests left") |
| `hp` | int | health percent 0..100 |
| `dead`, `combat` | bool | dead / in combat |
| `gold` | int | the bot's own money in copper |
| `role` | enum | `tank` \| `heal` \| `dps` \| `none` |
| `spec` | str | name of the talent template the bot plays by (§7.16), `Manual`, or empty if its class has none |
| `spt` | int | what is assigned: `0` the default of its class, `1` manual, else a template id |
| `tal` | str | talent points per tree now, `5/0/12`; empty while the bot is not in the world |
| `sq` | int | the guild bot it follows as a squad member, `0` = none (docs/orders.md "Squads") |
| `sqs` | enum | squad state: `none` \| `active` (follows its leader, its own order waits) \| `leads` (members follow it) \| `benched` / `away` / `gap` (its leader is benched, not in the world, too many levels apart: it plays its own order meanwhile) \| `off` (squads switched off) |
| `sqm` | int | how many members follow it (a leader) |
| `sqq`, `sqqn` | int, str | the quest of the quest partner squad it is in (as member or as leader), `0` = none, and its title (docs/orders.md "Quest partners") |
| `qw`, `qwn` | intlist, str | the hard quests it keeps in its log until another guild bot has them too (skipped meanwhile), and their titles joined by `; `; empty while it is not in the world |
| `p1`, `p2` | int | planned primary professions (skill ids, 0 = none) |
| `t1`, `t2` | int | target skill for `p1` / `p2` |
| `sec` | list | planned secondary professions |
| `sk` | tuples | known professions `skillId:value:max`, primary ones first; while the bot is not in the world, the ones it had when it was last seen (read from the characters database at startup), omitted when unknown |
| `gl` | int | gear level: the average item level of its armour and weapons (docs/orders.md "Readiness"); only while the bot is in the world (the addon takes its presence as "in the world") |
| `glk` | int | the last known gear level: the same as `gl` while the bot is in the world, else the one of when it was last seen (its equipment in the characters database at startup); omitted when unknown. The roster's iLvl column (2026-11-14; an older server sends only `gl`) |
| `rdy` | tuples | readiness for the dungeons in its reach, `dungeonId:percent` (§7.20); only while the bot is in the world |
| `dr` | enum | dungeon role the owner gave the bot: `none` \| `tank` \| `heal` (§7.22). In a dungeon group a bot without one counts by `role` |
| `drs` | str | the roles its class can fill, comma separated (`tank,heal`; empty: damage only) |
| `drt` | int | the talent template of the role as stored (`0` = the default of its class for the role) |
| `drn` | str | the name of the talent template it changes to for a dungeon run (empty without a role) |
| `drw` | bool | it wears its dungeon set right now (a dungeon run) |
| `drp` | int | how many pieces of its own the dungeon set has |
| `drgl` | int | gear level in its dungeon set (as `gl`, over the set it would wear in a run); only in the world |
| `drrdy` | tuples | readiness per dungeon in its dungeon set, `dungeonId:percent`; only in the world and with a role |
| `druns`, `drund` | int | a dungeon order: the runs it still has (the one that is going included) and the runs it has done; 0 without a dungeon order |
| `fp`, `fpg`, `fpn`, `fpsl`, `fpw`, `fpid`, `fpd` | int, int, str, int, str, int, bool | fight profiles (§7.26): the profile of its solo and its group slot (0 = nothing of its own), what it plays now and from which slot, why it does not play its slot's profile, the id of what it plays and whether by default |
| `kills`, `deaths`, `quests` | int | lifetime counters since recruitment |
| `xph` | int | XP per hour (rolling) |
| `gph` | int | copper per hour (rolling) |
| `since` | int | recruitment time |

`BOT_REMOVE^g=<guid>` removes a bot from the roster (dismissed or deleted).

### 7.5 `ZONE` — one zone template row (`ga_zone_template`)

`id` int zone id · `n` str name · `min`, `max` int level band · `fac` = `A` \| `H` \| `C`
(contested / both) · `cont` int map id of the continent · `orders` list of order types allowed
in this zone. Sent only inside `ZONES_BEGIN^zv=<version>^n=<count>` … `ZONES_END`.

### 7.6 `UNLOCK` — one progression step (`ga_unlock_template` + owner state)

`id` int · `lvl` int renown level that makes it available · `kind` = `slots` \| `offline` \|
`feature` · `val` (`slots`: total slots after the unlock; `offline`: window in minutes;
`feature`: a feature key) · `gold` int cost in copper (0 = free, unlocked automatically) ·
`st` = `owned` \| `available` (level reached, can be bought) \| `locked` \| `covered` (2026-11-14: a `slots` or
`offline` tier no larger than what the guild has already, so there is nothing to buy; the addon shows it as included) ·
`desc` str.
Sent in `UNLOCKS_BEGIN` … `UNLOCKS_END`; a single `UNLOCK` outside a block is an upsert.

### 7.7 `EVENT` — event feed entry

`id` int (increasing per guild) · `t` int time · `type` (§8.3) · `g` int bot GUID (0 = guild
event) · `msg` str English text, already formatted by the server · optional typed extras:
`lvl` int, `zone` int, `pts` int.
A single `EVENT` is a new event (push); `EVENTS_BEGIN^n` … `EVENTS_END` is the recent history.

### 7.8 `AWAY` — "while you were away" summary

```
AWAY_BEGIN^from=<t>^to=<t>^lv=<levels gained>^rp=<renown gained>^gold=<copper earned>^q=<quests>^d=<deaths>^n=<lines>
AWAY_LINE^g=<guid>^msg=<one line per bot, e.g. "Aldric: 12 -> 15, Westfall -> Redridge Mountains">
AWAY_END
```

### 7.10 Guild storage (docs/storage.md)

```
STORAGE_BEGIN^n=<records>^kinds=<kinds>^items=<items>^ench=<skill>^enchn=<name>
STORAGE_ITEMS^it=<entry>:<rp>:<amount>:<class>:<subclass>:<sell>:<ds>:<inv>:<res>[:<cat>],...^qi=<q>:<il>,...     (up to 10 per record)
STORAGE_END
STORAGE_DELTA^it=<entry>:<rp>:<amount>:<class>:<subclass>:<sell>:<ds>:<inv>:<res>[:<cat>],...^qi=<q>:<il>,...^kinds=<kinds>^items=<items>
STORAGE_ENCH^ench=<skill>^enchn=<name>
```

`rp` is the random property (`ItemRandomPropertyId`: > 0 a property, < 0 a suffix, 0 none); `class` and `subclass`
are `item_template`'s, so the addon can sort a kind into its category without knowing the item yet. The block
replaces the client's list; a `STORAGE_DELTA` (at most one burst per second) sets the amounts of the kinds it names
(`amount` 0 = gone); `kinds` and `items` (the totals) come on the last record of a burst. `STATE` carries the same
totals as `stk` / `sti`. `sell` is the vendor price of one (copper, 0 = no vendor buys it), `ds` the Enchanting skill
needed to disenchant one from the storage (-1 = it cannot be: only green and blue weapons and armour with a disenchant
table can; an older server sends five fields, the addon takes 0 and -1 then). `inv` (2026-11-13) is the item's
`InventoryType` (1 head, 5 chest, 20 robe, 13 one-hand, ...), for the Storage tab's slot filter; an older server
sends seven fields and the addon takes the slot from the client's item cache then. `res` (2026-11-13) is the kind's
reserve: how many the bots leave in the storage (docs/storage.md "Reserves"; 0 none, also from an older server).
`cat` (2026-11-25, only when it is there) names the kind's category where its class does not say it: `darkmoon` for
the Darkmoon Faire cards and decks (class 12, quest; docs/storage.md "Darkmoon cards"), `reputation` for the
reputation turn-ins (docs/storage.md "Reputation turn-ins"). `qi` (2026-11-25) holds one
`quality:itemLevel` per tuple of `it`, in the same order (a list beside the tuples, so an older addon, which reads the
tenth field of a tuple as its category, is not disturbed); the Storage tab filters by quality and sorts by both, and
takes them from the client's item cache when an older server sends no `qi`. `ench` / `enchn` on `STORAGE_BEGIN` and
the record `STORAGE_ENCH` (sent when it changes) name the guild's character in the world with the most Enchanting and
its skill (-1 and empty: none), so the addon greys Disenchant with the reason.

```
STORAGE_JUNK^kinds^items^money^to^sold^cap
```

The answer to `STORAGE_SELL_JUNK` (docs/storage.md "Junk"): `kinds`, `items` int what of the storage is junk now (or,
with `sold` = 1, what was sold), `money` int copper a vendor pays for it, `to` str where the gold goes ("the guild
bank" since 2026-11-13; before, the name of the character the client plays), `cap` bool something was not sold (no
guild bank).

### 7.11 Loot filter

```
LOOT_SETTINGS^on^dai^dpl^al^sold^de^del^st^max^allow^srv^salr^reag^junk^junkok^kwg
LOOT_RULES_BEGIN^n
LOOT_RULE^id^pos^act^en^lg^c
LOOT_RULES_END
```

`LOOT_SETTINGS` (record): `on` bool loot filter on for this guild · `dai` / `dpl` int default action (§8.8) for a
character the AI plays / the character the owner plays · `al` bool auto loot · `sold` int copper, `de`, `del`, `st` int
totals (disenchanted, deleted, stored) · `max` int rules allowed · `allow` list of the actions the server allows · `srv`
bool the server's loot filter is on · `salr` bool the server's auto loot is on · `reag` bool spells take missing reagents
from the storage (CP-7) · `junk` int the highest quality the guild sells as junk (0 grey, 1 grey and white, -1 none: the
bot settings `LootFilter.SellJunk` / `.SellJunkQuality`, docs/storage.md "Junk") · `junkok` bool the server lets junk be
sold at all (loot filter on, selling allowed) · `kwg` bool grey and white gear a guild character would wear is no junk (bot
setting `LootFilter.KeepWornGear`, default off). The addon shows the junk rule as a built-in row after the guild's rules
and takes the guild's values from the bot settings (§7.19) once they are loaded.

`LOOT_RULE`: `id` int · `pos` int 1-based evaluation order · `act` int action · `en` bool · `lg` int logic (bit g:
group g matches when any of its conditions does, else all; `16`: the rule matches when any group does, else all
groups) · `c` str the conditions: `type:op:value:value2:text[:group]` separated by `;`, at most 8, groups 0-2 in order
(mod-loot-filter's format). `type` 0 quality (0-7), 1 item level, 2 sell price (copper), 3 item type (`value` class,
`value2` subclass or 255 = any; `op` 0), 6 item id (`op` 0), 7 name contains (`text`: letters, digits, space,
`'-.,`, at most 40; `op` 0); `op` 0 is, 1 at least, 2 at most.

### 7.12 Recent loot

```
LOOT_LOG_BEGIN^n
LOOT_LOG^t^g^e^rp^n^o^m^why
LOOT_LOG_END
```

`t` int time · `g` int the looter's GUID · `e` int item · `rp` int random property · `n` int count · `o` int outcome
(§8.8: what happened) · `m` int copper (sold) · `why` str a short reason when the outcome is not what a rule asked
for (`the bot uses it`, `a tool`, `soulbound`, `from the bags`, …). The block is the newest ≤ 30 lines (oldest
first); a single `LOOT_LOG` outside a block is a new line (pushed once a second, ≤ 30 per burst).

### 7.13 Recipes (crafting from the storage)

```
RECIPES_BEGIN^g^n^busy
RECIPE^s=<spell>^i=<item>^max=<how often>^nm=<name>
RECIPES_END
```

`g` the crafter, `busy` bool it is crafting now; per recipe (tradeskill spells that create an item, at most 150, the
craftable ones first): `s` int spell · `i` int the item it makes · `max` int how often the crafter's bags and the
guild storage allow it · `nm` str the item's name.

### 7.14 Auction house surplus

```
AUCTION_RULES_BEGIN^n^srv^hours^every
AUCTION_RULE^e^rp^keep^stack^price^on^auto^listed
AUCTION_RULES_END
AUCTION_CATS_BEGIN^n
AUCTION_CAT^cat^keep^stack^on
AUCTION_CATS_END
AUCTION_LISTINGS_BEGIN^n
AUCTION_LISTED^e^rp^n
AUCTION_LISTINGS_END
```

`srv` bool the server posts surpluses · `hours` int auction duration · `every` int seconds between the passes; per rule:
`e`, `rp` the kind · `keep` int · `stack` int · `price` int copper per item (0 = default) · `on` bool · `auto` int the
default price per item · `listed` int items of this kind in the auction house now. The block is sent with the sync, after
every change and after every pass that posted something (docs/storage.md §8). Right after it come (2026-11-13) the
rules for whole storage categories (`AUCTION_CAT`: `cat` the category, `keep` per kind, `stack`, `on`; a kind's own rule
comes first) and the items of each kind in the auction house now (`AUCTION_LISTED`: `e`, `rp`, `n`; also for kinds a
category rule posted). The gold of a sale goes into the guild bank (`STATE^bank`), not by mail to the leader.

### 7.15 Guild perks

```
PERKS_BEGIN^n
PERK^id^key^n^rank^max^cost^unit^desc^next^avail
PERKS_END
```

Per perk: `id` int · `key` str what it does (`move_speed`, `experience`, `gold`, `extra_material`, `kill_regen`,
`no_reagents`, `instant_flight`, `all_flight_paths`, `materials_storage`, `drop_chance`, `craft_speed`, `gather_speed`,
`water_breathing`, `no_fall_damage`) · `n` str name · `rank` int bought ranks (0 = none) · `max` int (1 for a single
unlock) · `cost` int guild points of the next rank (0 at the highest) · `unit` str (`%` or empty) · `desc` str the effect
now (empty at rank 0) · `next` str the effect of the next rank · `avail` bool offered on this server (the movement,
crafting, gathering, breathing and falling perks need core patch CP-8). Sent with the sync (after `UNLOCKS`), on
`GET^what=perks` and after every purchase. The points are `STATE^gp`; `STATE^gps` is what the guild spent, `STATE^gpg`
what a GM granted, `STATE^achp` what its achievements earned and `STATE^msp` what its tiered achievements earned
(docs/guild-management.md "Guild perks").

### 7.18 Tiered guild achievements (2026-11-12)

```
MILESTONES_BEGIN^n
MILESTONE^s^k^grp^sub^n^v^th^pt^act^got
MILESTONES_END
```

One record per series (docs/guild-management.md "Tiered guild achievements"): `s` str the series key (`level_class_1`,
`level_race_3`, `skill_186`, `storage_items`, `guild_gold`, `kills_7`, `quests`, `dungeons`, `raids`) · `k` str its kind
(the key without the subject) · `grp` str how the UI groups it: `class` \| `race` \| `prof` \| `kills` \| `guild` · `sub`
int class, race, skill line or creature type (0 for guild-wide series) · `n` str its name (`Warrior`, `Mining`,
`Beasts`; empty for guild-wide series) · `v` int the guild's value now (highest level, highest skill, items in the
storage, gold of the guild in gold, kills, quests, runs) · `th` intlist the threshold of every tier · `pt` intlist the
guild points of every tier (the last tier the level cap allows already counts double) · `act` int how many tiers the
level cap allows (the rest wait for 70 / 80) · `got` int how many tiers the guild reached. The series of the other
faction's races are sent too (they cannot be reached; the addon hides them). Sent with the sync (after `PERKS`), on
`GET^what=milestones` (the Achievements tab asks every 30 s while it is open) and whenever the guild reaches a tier.

### 7.20 Dungeons (readiness; 2026-11-12)

```
DUNGEONS_BEGIN^n
DUNGEON^id^map^n^min^max^tgt^size^raid^gl^run
DUNGEONS_END
```

The dungeons and raids the `rdy` numbers of the `BOT` records refer to (docs/orders.md "Readiness"): `id` int the
client's LFG dungeon id · `map` int · `n` str its name · `min`, `max`, `tgt` int lowest, highest and target level ·
`size` int players · `raid` bool · `gl` int the gear level it asks for · `run` bool a run can be ordered into it
(`ORDER_SET` with `DUNGEON`; a client treats a missing `run` as true). Not part of the sync: sent on
`GET^what=dungeons` (the bot detail's Character sub-tab asks once per connection). A `rdy` entry whose dungeon the
client does not know is not shown.

### 7.21 Craft orders (2026-11-13)

```
CRAFT_ORDERS_BEGIN^n
CORDER^id^item^n^amt^made^note
CRAFT_ORDERS_END

CRAFTABLES_BEGIN^n
CRAFTABLE^item^n^il^cls^sub^by
CRAFTABLES_END
```

The guild's craft orders (docs/orders.md "Craft orders"): `id` int · `item` int · `n` str the item's name · `amt`
int asked for · `made` int in the storage so far · `note` str who makes it or what it waits for. And what the guild's
characters in the world can make: `item` · `n` · `il` int item level · `cls`, `sub` int item class and subclass ·
`by` str the names of those who know a recipe. Neither is part of the sync: `GET^what=craftorders`,
`GET^what=craftables`; the orders are pushed again after every change (an order placed, taken back, worked on, done).

| Intent | Keys | Answer |
|---|---|---|
| `CRAFT_ORDER_ADD` | `item` int · `n` int count (1-200; the same item again adds to its order) | `CRAFT_ORDERS` block, `ACK`; `ERROR CRAFT_ORDER_INVALID` (no profession recipe makes the item, a bad count) or `CRAFT_ORDER_LIMIT` (30 orders) |
| `CRAFT_ORDER_DEL` | `id` int | `CRAFT_ORDERS` block, `ACK`; `ERROR CRAFT_ORDER_INVALID` |

`BOT` records of characters in the world carry `nw` bool (a mage, priest or warlock with nothing in its ranged slot)
and `wm` str (who of the guild could make the best wand it can use, "Priestholy can make Lesser Magic Wand"; empty
when nobody can). Event type `CRAFT`: an order is done.

### 7.16 Talent templates (docs/specs.md)

```
SPECS_BEGIN^n=<count>
SPEC^id^class^n^p^ro^t^d^own^sum
SPECS_END
```

Every built-in template (all classes) and the guild's own: `id` int · `class` int · `n` str name · `p` int purpose
(0 leveling, 1 dungeon and raid, 2 raid, 3 PvP) · `ro` int role (0 damage, 1 tank, 2 healer) · `t` int the tree with
the most points · `d` bool the default of the class · `own` bool saved by this guild (can be deleted) · `sum` str
points per tree at level 60 (`0/0/51`). The block replaces the list; it is sent with the sync and after `SPEC_SAVE` /
`SPEC_DEL`.

### 7.23 Farm orders: the item picker (docs/orders.md "Farm orders"; 2026-11-13)

The order itself is an `ORDER_SET` with `o = FARM`, `zone` = the item and optionally `n` = a target stock (§6); the
`BOT` record names it (`o`, `oz`, `ozn`) and carries `ofn` int the target stock (0 = none) and `ofs` int what the guild
storage holds of the item now (both 0 under any other order). The picker asks with `FARM_SEARCH` and gets:

```
FARM_ITEMS_BEGIN^q=<text>^g=<bot>^n=<count>
FARM_ITEM^item^n^q^il^cls^k^lv^need^ok
FARM_ITEMS_END
```

One record per item some creature that bots can grind drops for the guild's side, names that begin with the text
first, at most 30: `item` int item entry · `n` str name · `q` int quality · `il` int item level · `cls` int item class ·
`k` int kinds of creature that drop it and that a bot of this side can grind · `lv` int the level of the lowest of
them · `need` int the level a bot needs for that one (`lv` + the bot's `Farm.LevelsBelow`, or `lv` without a bot) ·
`ok` bool the bot of the request is at least that level (true without a bot). `FARM_ITEMS_BEGIN` repeats the text and
the bot of the request. The block replaces the picker's list; it is not part of the sync.

When a bot with the order has nothing it can farm the item from, the leader gets an `EVENT` of the type `FARM` (§8.3)
and the bot's `why` says so. The same event type tells him that a target stock is reached (the bot's order is `AUTO`
again).

### 7.22 Dungeon roles (docs/dungeon-roles.md; 2026-11-13)

The role itself travels in the `BOT` record (`dr`, `drs`, `drt`, `drn`, `drw`, `drp`, `drgl`, `drrdy`, §7.4) and is
set with `ROLE_SET` (§6). The role store of a bot comes on request, `GET^what=rolegear^g=<bot>`:

```
ROLEGEAR_BEGIN^g=<bot>^n=<count>
RG^g^id^k^s^e^rp^n^q^il^it
ROLEGEAR_END
```

One record per piece: `g` int the bot · `id` int the piece · `k` int `1` = a piece of its dungeon set, `2` = a
levelling piece put aside while the dungeon set is on · `s` int the equipment slot it was worn in (`k` 2); for `k` 1:
255, or 254 for a soulbound piece that was never worn ·
`e` int item entry · `rp` int random property · `n` str name · `q` int quality · `il` int item level · `it` int
inventory type. The block replaces what the addon knows of that bot's store.

---

### 7.24 Dungeon runs (the run window; docs/orders.md "The dungeon run window"; 2026-11-13)

Intents:

| Intent | Fields | Reply |
|---|---|---|
| `DUNGEON_START` | `d` int the dungeon (section 7.20) · `bots` intlist the bots of the group (1 to the dungeon's size; the guild leader's own character, `LEADER^g`, may be one of them beside at least one bot: it comes along and never leads, docs/orders.md "The guild leader in a dungeon run") · `runs` int how often (1 to `GuildAdventure.Dungeons.MaxRuns`; 0 = until it is stopped; default 1) · `lead` int the bot that leads (optional: else the first tank, else the first bot) · the preparation step, all optional: `ia` bool with `noitems` intlist: every kind of item of the guild storage that a run uses is used, but the kinds of `noitems` (what the leader took out in the preparation step; item ids of the block `DPREP`, others are dropped); without `ia`, `items` intlist lists the only kinds the group may use (an addon from before 2026-11-14); with neither every kind is used · `tl` int seconds the others wait for the tank (0 = off, at most 30) · `em` int the healer's emergency limit in percent · `cpon` bool careful pulls · `cp` int from how many creatures (2 to 10) · `ff` bool creatures that run away are focused · `md` int how much the group fights on its way: 0 the bosses (what would attack it on its way), 1 every room on the way, 2 every room on the way in big pulls (2026-11-19) · `gr` int a big pull's yards around its first creature (10 to 40) · `gc` int a big pull's most creatures (2 to 15) (rules that are not sent are the server's; when one is sent, the others count with their defaults) | the `BOT` deltas of the group, block `DRUNS`, `ACK`; `ERROR DUNGEON_INVALID` with the reason (the dungeon cannot be run, a bot is not in the world, played by the owner, on a run already or below 50 % readiness, a squad that cannot be formed) |
| `DUNGEON_STOP` | `g` int the bot that leads the run · `now` bool: the run ends at once and the group leaves; else the run that is going is the order's last | block `DRUNS`, `ACK`; `ERROR DUNGEON_INVALID` |
| `GET` with `what` = `dungeonruns` | | block `DRUNS` |
| `GET` with `what` = `dungeonprep` | | block `DPREP` |

Block `DRUNS` (replaces; in the sync, after `DUNGEON_START` / `DUNGEON_STOP`, and pushed whenever a run changes: its
phase, a boss, a wipe, the runs left, its group, the leader's why line):

```
DRUNS_BEGIN^n
DRUN^g^d^dn^ph^b^of^nb^w^left^done^min^m^why
DRUNS_END
```

| Field | Type | Meaning |
|---|---|---|
| `g` | int | the bot that leads the run (its order is `DUNGEON`) |
| `d`, `dn` | int, str | the dungeon and its name |
| `ph` | int | 0 not started (its leader is not in the world), 1 getting ready (or between two runs), 2 inside |
| `b`, `of` | int | bosses done, bosses of the dungeon |
| `nb` | str | the boss the group is after ("" outside, or when none is left) |
| `w` | int | wipes of the run that is going |
| `left`, `done` | int | runs the order still has (the one that is going included; 255 = until it is stopped), runs done |
| `min` | int | minutes of the run that is going (not counted as a change of the block) |
| `m` | intlist | the other bots of the group |
| `why` | str | the leader's why line |

`ORDER_SET` with `o` = `DUNGEON` takes `runs` = 0 as "until it is stopped" too; the `BOT` record then carries `druns`
255.

The block `DUNGEONS` leaves out a dungeon that lies in the other side's capital (Ragefire Chasm for an Alliance
guild, the Stockade for a Horde one): no run of the guild can be ordered into it.

The dungeons in their order (`GuildAdventure.Dungeons.UnlockInOrder`): a `DUNGEON` record (section 7.20) also carries
`ord` int its place in the guild's order of dungeons (1 = the first; 0 = not in it: a raid, the other side's, or it
cannot be run yet), `fin` int how often the guild has finished it (the end boss down), `lock` bool it is not open
yet, and `need` str the dungeon to finish first. `DUNGEON_START` and `ORDER_SET` refuse a locked dungeon with
`DUNGEON_INVALID`. The block `DUNGEONS` is sent again when the guild finishes a dungeon, and after every `HELLO` of a link that had it (a push that falls into the client's loading screen never arrives: the end of a run with the leader's own character in it; the Dungeons tab also asks for `dungeons` and `dungeonruns` each time it is shown).

Block `DPREP` (replaces; only on `GET^what=dungeonprep`, which the addon sends when the window's preparation step
opens): the server's fight rules as the defaults of the step, and what the guild storage holds for a run.

```
DPREP_BEGIN^n^tl^em^cp^cpon^ff^md^gr^gc
DITEM^e^n^q^k^amt^lvl
DPREP_END
```

| Field | Type | Meaning |
|---|---|---|
| `tl`, `em`, `cp`, `cpon`, `ff`, `md`, `gr`, `gc` | int, int, int, bool, bool, int, int, int | the server's fight rules and mode (as in `DUNGEON_START`) |
| `e`, `n`, `q` | int, str, int | the item, its name, its quality |
| `k` | int | 1 elixir or flask, 2 scroll, 3 stone or oil for a weapon, 4 scope |
| `amt` | int | pieces in the guild storage now |
| `lvl` | int | the level the item asks for |

The talents of the preparation step are the dungeon role of each bot (`ROLE_SET`, section 7.22).

### 7.25 The shopping list (docs/storage.md section 12; 2026-11-14)

Block `SHOPPING` (replaces; part of the sync after the storage, on `GET^what=shopping`, after `SHOP_SET` / `SHOP_DEL`,
and pushed after a bot bought for the list):

```
SHOPPING_BEGIN^n^srv^day^cap^visit^max
SHOP^e^n^min^on^have^price^bc
SHOPPING_END
```

| Field | Type | Meaning |
|---|---|---|
| `srv` | bool | the bot setting `Shopping.Enable` of the guild |
| `day`, `cap`, `visit` | int | copper spent on the list today, the day's budget (`Shopping.MaxPerDay`), the most per vendor visit (`Shopping.MaxPerVisit`) |
| `max` | int | items the list may hold |
| `e`, `n` | int, str | the item and its name |
| `min`, `on` | int, bool | the minimum stock, paused when false |
| `have` | int | what the guild storage holds of it now (every random property); the addon follows the storage's own deltas after that |
| `price`, `bc` | int, int | copper at a vendor for one purchase, items one purchase gives |

Block `SHOP_ITEMS` (replaces; the answer to `SHOP_SEARCH`, at most 30 items, best match first): items vendors of the
guild's side sell for gold that the list can take.

```
SHOP_ITEMS_BEGIN^n^q
SHOP_ITEM^e^n^q^lvl^price^bc
SHOP_ITEMS_END
```

`q` of `SHOP_ITEM` is the item's quality, `lvl` the level it asks for.

---

### 7.27 The plan of a guild character (the control bar's Details; 2026-11-25)

```
GET^what=plan^g=<character>
PLAN_BEGIN^g^n^at
PL^k=<kind>^t=<text>
PLAN_END
```

the owner (2026-11-25): [quote left out]. The control
bar's *Details* asks for it every 5 s while its window is open; `g` is a roster bot or the guild leader with an order.
`PLAN` replaces what the addon had for that character (`OrderService::DescribeNext`): one `PL` per line, `k` one of
`now` (its why line), `task` (the task it works on: kind, quest, target, distance, how long), `squad` (its squad or quest
partner), `wait` (for a squad member: what its leader does now and why it waits, for example "Wartank: waiting for its squad (Priestyo rests)" or "Wartank: the group rests (Owldudu mana 31%)"; since 2026-11-27, the owner: [quote left out]; an addon before it shows the line under "More"), `run` (the dungeon run it is on, for a member "..., led by <leader>"), `errand` (a hand-in it travels to), `hand` (a finished quest to hand in),
`quest` (a quest of its log with its progress and what keeps it waiting: a level, a quest partner, or "now"), `upkeep`
(bags, gear, quest log, money) and `aside` (zones and places it keeps away from); `at` is the server's time. A
character the owner plays by hand gets the line `now` "You play it yourself" and its quest log and upkeep. An unknown character is `ERROR UNKNOWN_BOT`.

---

### 7.26 Fight profiles (docs/fight-profiles.md; 2026-11-25)

What a bot fights by travels in the `BOT` record (§7.4): `fp` int the profile of its solo slot, `fpg` int the
profile of its group slot (a dungeon or raid map, a dungeon run), `0` = the default of mod-playerbots · `fpn` str the
profile it plays now in a fight, empty = the default · `fpsl` int the slot that profile is from (`0` solo, `1` group)
· `fpw` str why it does not play the profile of its slot (another talent tree, the fallback after 10 s without an
action), empty = it does or it has none · `fpid` int the id of the profile it plays now, 0 = mod-playerbots' rotation
· `fpd` bool it plays that profile by default: nothing of its own in the slot, or its own profile is of another talent
tree, so the template of its class and talent tree (docs/fight-profiles.md §12, bot setting
`Fight.TemplatesByDefault`). The rest comes on request (the Combat sub-tab asks once per look at a bot)
and after the intents that change it.

`GET^what=fightprofiles^g=<bot>`, and after `FIGHT_ASSIGN`, `FIGHT_COPY`, `FIGHT_SAVE` and `FIGHT_DEL`: the guild's
profiles of the bot's class with its slots (replaces).

```
FPROFILES_BEGIN^g^cls^tab^solo^grp^max^dflt^dfltg^n
FPROF^g^id^n^tab^rev^rc^rn^used^opts^tpl^tr
FPROFILES_END
```

| Field | Type | Meaning |
|---|---|---|
| `cls`, `tab` | int, int | the bot's class, and its talent tree now (0-2, mod-playerbots' tree of the character; only while it is in the world) |
| `solo`, `grp` | int, int | the profiles of its two slots, as `BOT^fp` / `BOT^fpg` |
| `max` | int | profiles a guild may have per class (20) |
| `dflt`, `dfltg` | int, int | the template the bot plays by default in its solo and its group slot when the slot is empty (0 = mod-playerbots' rotation: no template of its tree and role, or `Fight.TemplatesByDefault` off) |
| `id`, `n`, `tab`, `rev` | int, str, int, int | a profile, its name, the talent tree it is for, its revision (each save) |
| `rc`, `rn`, `used`, `opts` | int | its rules in the fight and between fights, the bots it is assigned to (either slot), its options |
| `tr` | int | a template's role: `0` solo (damage), `1` tank, `2` heal (docs/fight-profiles.md §12) |
| `tpl` | bool | a template of no guild (FP-4, docs/fight-profiles.md §11, §12: the wowbot335 fight classes and the tank and healer ones of the fight class research): of no guild, ids from 1000001, listed for every guild after its own profiles; it can be assigned (`FIGHT_ASSIGN`), its options set per bot (`FIGHT_OPTION`) and copied (`FIGHT_COPY^from`), not saved or deleted |

`GET^what=fightprofile^g=<bot>^id=<profile>`, and after `FIGHT_OPTION` and `FIGHT_SAVE`: one profile's rules and its options with this
bot's values; `id` 0 is the bot's template, mod-playerbots' strategies read from the bot (only while it is in the
world: `live`).

```
FPROFILE_BEGIN^g^id^pn^tab^rev^note^rep^hid^live^n^tpl
FRULE^g^l^k^ref^a^tg^tgt^rel^en^fl^iv^c^d^src^nd^kn^lv
FPROFILE_END
FOPTS_BEGIN^g^id^n
FOPT^g^id^k^n^kind^v^min^max^own^bv
FOPTS_END
```

| Field | Type | Meaning |
|---|---|---|
| `pn`, `note` | str | the profile's name ("Default (mod-playerbots)" for the template) and note |
| `tpl` | bool | as `FPROF^tpl`: a wowbot335 template, read-only |
| `rep` | str | the template: the strategies of the fight it stands for, comma separated; a profile: the template it was copied from |
| `hid` | int | the template: the rows of those strategies that stay as they are in every profile (movement, targets, what the module's own rules cover) |
| `l` | int | the list: `0` the fight, `1` between fights |
| `k`, `ref` | int, int | the kind of rule: `0` a row of mod-playerbots (`ref` the spell its action casts, for the icon: the longest spell name the action begins with, among the spells the bot knows, then those its class can learn; 0 when none), `1` an action of mod-playerbots, `2` a spell (`ref` its first rank), `3` an item (`ref` its entry), `4` a trinket slot (`ref` 1 or 2) |
| `a`, `tg` | str | the action's and the trigger's names (mod-playerbots' names; the trigger empty for a default action) |
| `tgt` | int | the target: `0` its target, `1` itself, `2` its pet, `3` the member with the least health, `4` the tank, `5` an enemy that meets the conditions |
| `rel` | str | the relevance, a decimal (mod-playerbots' bands: emergency 90, raid 60, dispel 50, interrupt 40, critical heals and movement 30, high 20, normal 10, rotation below) |
| `en`, `fl`, `iv` | bool, int, int | switched on; flags (`1` once per target, `2` not twice in a row); at most every `iv` seconds |
| `c`, `d` | str | the conditions as text (`[!]key[:param][op value]`, joined by `;`) and in words |
| `src`, `nd` | str, int | the template's strategy the row comes from, and its row's node there (a row of mod-playerbots is found again by them) |
| `kn`, `lv` | bool, int | a rule with a spell (`ref`), while the bot is in the world: whether it knows the spell, and when not from which level (absent otherwise) |
| `k` (FOPT), `n`, `kind` | str, str, int | an option's key (`$key` in conditions), its label, its kind (`0` switch, `1` number, `2` percent) |
| `v`, `min`, `max` | str | the profile's value and its range (decimals) |
| `own`, `bv` | bool, str | whether the bot has its own value, and the value that counts for it |

**The editor (FP-3).** `GET^what=fightcatalog^g=<bot>`: what the editor offers (each block replaces).

```
FCONDS_BEGIN^n
FCOND^k^n^grp^vk^pk^min^max^tg
FCONDS_END
FSPELLS_BEGIN^g^live^n
FSP^id^hi^n^tab
FSPELLS_END
FITEMS_BEGIN^g^n
FIT^e^n
FITEMS_END
FTRIGS_BEGIN^g^n
FTRIG^n^t
FTRIGS_END
```

| Field | Type | Meaning |
|---|---|---|
| `k`, `n`, `grp` (FCOND) | str | a condition's key, its label, its group (`me`, `target`, `around`, `pet`, `group`, `other`) |
| `vk`, `pk` | int, int | what it compares (`0` nothing: it holds or not, `1` a number, `2` a percent) and its parameter (`0` none, `1` a spell, `2` an item, `3` yards, `4` a number, `5` a trigger of mod-playerbots, `6` an option's key) |
| `min`, `max`, `tg` | str, str, bool | the range of its value; whether it is asked of the rule's target |
| `id`, `hi`, `n` (FSP) | int, int, str | a spell of the bot's class it knows (not passive), by its first rank; the highest rank it knows; its name. Only while the bot is in the world (`live`), at most 250 |
| `tab` (FSP) | str | the tab of the spellbook it stands on: the class skill line it is learned under ("Enhancement", "Elemental Combat"), else "General" |
| `n`, `t` (FTRIG) | str | a trigger of mod-playerbots its class has (by name) and what it checks in plain language (`src/FightTriggerTexts.h`, generated from mod-playerbots' source; the values of playerbots.conf filled in); `t` is empty where there is no text |
| `e`, `n` (FIT) | int, str | a usable item the bot carries that a rule may use (not a potion, food, drink, bandage, elixir or scroll: the guild's own rules use those), at most 60 |

A rule of `FIGHT_SAVE` is the fields of `FRULE` joined by tabs: `l`, `k`, `ref`, `a`, `tg`, `tgt`, `rel`, `en` (`0` or
`1`), `fl`, `iv`, `nd`, `src`, `c` (13 fields; a row or an action of mod-playerbots has `ref` 0, the server keeps none).
An option is `k`, `n`, `kind`, `v`, `min`, `max` joined by tabs. The server checks the profile as the commands do
(the rules of the store: a known condition, an option a condition names, a spell of the class, an item with a use, a
target and the flags only for a spell or item, no enemy target between fights) and saves it or nothing.

---

## 8. Enumerations

### 8.1 Order types (`o`)

| Token | Parameters | v1 server |
|---|---|---|
| `AUTO` | `lc`, `cont`, `nd`, `gcap` | required |
| `IDLE` | — | required |
| `FOLLOW` | — (follows the character the owner currently controls) | required |
| `LEVEL_ZONE` | `zone`, `aa` | required |
| `GRIND` | `zone` | later |
| `QUEST` | `zone` | later |
| `GATHER` | `zone`, `skill` | later |
| `CRAFT` | — (profession mode: the bot works on the professions of its plan, docs/orders.md "Profession mode"; the addon shows it as "Professions") | accepted since 2026-11-12 |
| `DUNGEON` | `zone` = the dungeon's id (§7.20) | accepted since 2026-11-13 |
| `FARM` | `zone` = the item to farm, `n` = a target stock (§7.23, docs/orders.md "Farm orders"; the addon shows it as "Farm item") | accepted since 2026-11-13 |
| `TRAIN` | — | later |
| `VENDOR` | — | later |

The server lists the tokens it accepts in `STATE^orders`; the UI greys out the rest.

### 8.2 Bot states (`s`)

`active` (in world, executing its order) · `benched` (offline, keeps its slot) · `controlled`
(currently played by the owner) · `offline` (not benched but not in world, e.g. still logging
in or the offline window ran out).

### 8.3 Event types (`type`)

`LEVEL_UP` · `DEATH` · `QUEST_DONE` · `ZONE_CHANGE` (moved to a new zone) · `ZONE_DONE`
(out-levelled its zone) · `PROF_UP` (profession milestone) · `RENOWN` (renown gained) ·
`RENOWN_LEVEL` · `UNLOCK` · `RECRUIT` · `ORDER` (order changed) · `CONTROL` (control swap) ·
`LOGIN` · `LOGOUT` · `INFO` · `ACHIEVEMENT` (a new guild achievement, or achievements a character brought into
the guild when it entered the world; `pts` = the renown they gave) · `PERK` (the guild bought a perk rank, `lvl` = the
rank, or its perks were reset) · `MILESTONE` (the guild reached a tier of a tiered guild achievement, or several at
once in one line; `lvl` = the tier, `pts` = the renown) · `GEAR` (a bot bought a weapon or an armour piece and the guild paid part of it, or
a bot's gear is far below its level) · `FARM` (a bot with the order `FARM` has nothing it can farm its item from, the
message says why; or it has reached the order's target stock and is on the autopilot again; `lvl` = the bot's level) ·
`FIGHT` (a bot did nothing for 10 s in a fight under its fight profile and plays the default of mod-playerbots until
the profile is saved or assigned again, docs/fight-profiles.md §8; `lvl` = the bot's level).

### 8.4 Feature keys (`feat`)

`orders` · `recruit` · `auto_limits` · `professions` · `talents` · `gear` · `groups` ·
`dungeons` · `raids` · `map` · `stats` · `guild_bank`.

### 7.17 `LEADER` — the guild leader's order (record; 2026-11-12)

The leader is no roster bot (no `BOT` record, no slot), but it takes an order like one (docs/orders.md "The leader").
Sent in the sync right after the roster, after an `ORDER_SET` for the leader, on watch-mode changes and as a delta with
the bot deltas. `GET^what=leader` asks for it.

| Key | Type | Meaning |
|---|---|---|
| `g`, `n`, `race`, `class`, `gender` | | the leader |
| `s` | `active` \| `controlled` \| `offline` | `active` = the AI holds it (you play a bot, or it plays the offline window), `controlled` = your client plays it (also in watch mode), `offline` |
| `lvl`, `xp`, `xpmax`, `zone`, `zn`, `hp`, `dead`, `combat` | | as in `BOT` |
| `o` | `HOLD` \| order token | `HOLD`: no order, the default: the AI keeps the leader where it stands while you play a bot, and it logs out with you (D12) |
| `oz`, `ozn`, `lc`, `cont`, `nd`, `gcap` | | the order's zone and AUTO limits, as in `BOT` |
| `why` | str | what it does: "Holding position (no order)", "Played by you", "Watch mode: <planner>", the planner's why line, "Offline" |
| `xph`, `gph` | int | rates, as in `BOT` |

### 7.19 Bot behaviour settings (docs/bot-settings.md; 2026-11-12)

What the guild leader sets for how the bots play. The server owns the list; the addon draws whatever it is sent.

```
SETDEFS_BEGIN^n^sv
SETCAT^k^n^h^o                                         (one per category, in display order)
SETDEF^id^k^cat^t^min^max^st^sc^u^def^pc^adv^opt^n^h   (one per setting, in display order)
SETDEFS_END
SETVALS_BEGIN^n
SETVAL^g^v                                             (one per scope that has values; g = 0 always comes)
SETVALS_END
```

- `SETDEFS_BEGIN^sv` int: the schema version, a hash of everything in the block including the defaults. The client
  caches the block and sends `sv` in `HELLO`; the sync sends `SETDEFS` only when it differs. After a config reload
  that changed a default the server pushes the block again to every connected client.
- `SETCAT`: `k` str key · `n` str label · `h` str help · `o` int order.
- `SETDEF`: `id` int wire id (stable, never reused) · `k` str key = the conf key without `GuildAdventure.`
  · `cat` str category key · `t` str type: `bool` \| `int` \| `enum` \| `money` (copper) \| `dec` (value / `sc`)
  · `min`, `max` int range (an `enum`: 0 to options - 1) · `st` int step of the - / + buttons · `sc` int scale
  of a `dec` (1 otherwise) · `u` str unit of an `int` (`%`, `s`, `min`, `yd`, `levels` ...) · `def` int the
  server's default (the conf value) · `pc` bool a guild character may have a value of its own · `adv` bool a
  finer setting (the addon shows it with "Show all options") · `opt` str an `enum`'s options `token=Label;token=Label`
  · `n` str label · `h` str help.
- `SETVAL`: `g` int scope (0 = the guild, else a guild character's guid) · `v` tuples `id:value`, every value
  of the scope (a missing id has none). Inside `SETVALS` the block replaces all scopes; on its own (after
  `SETTING_SET` or a `.ga setting` command) it replaces that one scope.
- What counts for a character: its own value (only for `pc` settings), else the guild's, else `def`.

### 8.5 Error codes (`code`)

| Code | Meaning |
|---|---|
| `BAD_REQUEST` | malformed or missing fields |
| `BAD_VERSION` | `HELLO^v` below `WELCOME^min` |
| `RATE_LIMIT` | too many messages, dropped |
| `NOT_OWNER` | the GUID is not a bot of this owner |
| `UNKNOWN_BOT` | no such bot |
| `LOCKED` | the feature / order / unlock is not unlocked yet |
| `NOT_AVAILABLE` | known but not implemented by this server version |
| `NO_SLOT` | no free bot slot |
| `NAME_INVALID` | name rejected by the core's name rules |
| `NAME_TAKEN` | name already exists |
| `INVALID_COMBO` | race/class/faction combination not allowed |
| `ZONE_INVALID` | zone unknown, wrong faction, or order not allowed in that zone |
| `FARM_INVALID` | `ORDER_SET` with `FARM`: the item is unknown, or no creature that bots can grind drops it for the guild's side (it is gathered, fished, bought, crafted, or only elites and dungeon creatures have it); the message says which |
| `DUNGEON_INVALID` | `ORDER_SET` with `DUNGEON`: the dungeon is unknown or cannot be run, the bot is not the one to lead (it follows a squad leader, or it is the guild leader), or somebody of its group is not ready; the message says which |
| `NOT_ENOUGH_GOLD` | the guild bank (or the leader's purse, by `STATE^ulfrom`) cannot pay the cost |
| `COVERED` | `UNLOCK_BUY`: the tier gives no more slots / offline time than the guild has already |
| `SWAP_REFUSED` | control swap refused; see `reason` |
| `WATCH_REFUSED` | watch mode refused; `reason` is a token of §8.6 |
| `SPEC_INVALID` | a talent template request failed (`SPEC_SET`, `SPEC_SAVE`, `SPEC_DEL`); `msg` says why |
| `SQUAD_INVALID` | `SQUAD_SET` refused; `msg` says why |
| `ROLE_INVALID` | `ROLE_SET` refused; `msg` says why |
| `FIGHT_INVALID` | `FIGHT_ASSIGN` / `FIGHT_OPTION` / `FIGHT_COPY` / `FIGHT_SAVE` / `FIGHT_DEL` refused; `msg` says why |
| `SETTING_INVALID` | `SETTING_SET` refused; `reason`: `UNKNOWN` (no such setting), `RANGE` (outside its range), `GUILD_ONLY` (no per-character value for it), `NOT_MEMBER` (`g` is no guild character); `msg` says why |
| `STORAGE_REFUSED` | a guild storage request failed; `reason` is the server's short reason |
| `LOOT_REFUSED` | a loot filter change failed; `reason` = `INVALID`, `LIMIT`, `ACTION`, `NOT_FOUND` |
| `CRAFT_REFUSED` | crafting refused; `reason` = `UNKNOWN_SPELL`, `NOT_A_RECIPE`, `NOT_KNOWN`, `DEAD`, `BUSY`, `REAGENTS` |
| `AUCTION_REFUSED` | an auction rule change failed; `reason` = `NOT_AUCTIONABLE`, `INVALID`, `LIMIT`, `NOT_FOUND` |
| `UNKNOWN_PERK` | `PERK_BUY` for an id the server does not know |
| `MAX_RANK` | the perk is at its highest rank |
| `NO_POINTS` | the guild has fewer guild points than the next rank costs |
| `BUSY` | another swap is in progress |
| `INTERNAL` | server-side failure; details in the server log |

The addon additionally uses `TIMEOUT` locally when no reply arrived within 10 s.

### 8.6 Swap refusal reasons (`reason`)

`IN_COMBAT` · `LOADING` (loading screen / teleport pending) · `VEHICLE` · `CHARMED` · `DUEL` ·
`TRADE` · `DEAD` · `BENCHED` · `OFFLINE` · `SAME` (already controlled) · `FLIGHT` (on a taxi
flight) · `CONTROLS_UNIT` (controlling another unit, e.g. mind control or an Eye of Kilrogg) ·
`BATTLEGROUND` (in a battleground or arena) · `LFG` (queued for or inside a dungeon finder
group) · `GM_MODE` · `SELF_BOT` (the character runs as a playerbots self-bot) · `SHUTDOWN`
(server shutdown pending) · `PATH_UNAVAILABLE` (the server is set to the world-reload path
and the target is where it cannot work: inside an instance, on a ship or zeppelin, or in
Acherus; with the `auto` setting the server falls back to the warm relog instead) ·
`NOT_GUILD_BOT` (watch mode only: the character the client plays is not one of the guild's characters — a test
character logged in with `.ga bot login`) · `NO_ORDER` (watch mode only: the leader has no order; give it one first,
docs/orders.md "The leader").

`BUSY` and `NOT_OWNER` are error codes of their own (§8.5); a server may also send them as
`reason` of `SWAP_REFUSED`, and the addon treats both forms the same.

### 8.7 DBC ids used by the addon

- Races: 1 Human, 2 Orc, 3 Dwarf, 4 Night Elf, 5 Undead, 6 Tauren, 7 Gnome, 8 Troll,
  10 Blood Elf, 11 Draenei.
- Classes: 1 Warrior, 2 Paladin, 3 Hunter, 4 Rogue, 5 Priest, 6 Death Knight, 7 Shaman,
  8 Mage, 9 Warlock, 11 Druid.
- Primary professions: 164 Blacksmithing, 165 Leatherworking, 171 Alchemy, 182 Herbalism,
  186 Mining, 197 Tailoring, 202 Engineering, 333 Enchanting, 393 Skinning, 755 Jewelcrafting,
  773 Inscription. Secondary: 129 First Aid, 185 Cooking, 356 Fishing.
- Continents (`cont`): 0 Eastern Kingdoms, 1 Kalimdor, 530 Outland, 571 Northrend.

---

### 8.8 Loot actions (`act`, `dai`, `dpl`, `o`)

`0` keep (stays in the looter's bags) · `1` sell (vendor price to the looter) · `2` disenchant (materials into the
guild storage) · `3` delete · `4` to the guild storage. In the storage's log (`LOOT_LOG^o`) only, since 2026-11-13:
`5` taken out of the guild storage by an order of the owner (Take, `.ga storage take`, listed in the auction house) ·
`6` put into it by an order of the owner (Deposit, Deposit materials, back from the auction house unsold, an
administrator's test aid) · `7` a container a guild character opened (`n` 1, `m` its money, `why` what came out;
docs/storage.md "Containers") · `8` bought at a vendor for the shopping list (`m` what it cost; docs/storage.md
"Shopping list"); `why` names the order. An older addon shows them as "Kept".

## 9. Server rules (validation and limits)

- **Never trust the client.** Validate every field: enum tokens, integer ranges, name rules
  (`ObjectMgr::CheckPlayerName`), race/class/faction (the client's tables are only a
  convenience).
- **Ownership.** Only the owner's session may command the owner's bots. Messages from bot
  sessions are ignored. Every `g` must be a bot of the sender's guild (`NOT_OWNER` otherwise).
- **Rate limit.** At most `GuildAdventure.Protocol.MaxMsgPerSecond` logical messages per session
  and second; drop the rest and answer at most one `ERROR RATE_LIMIT` per second. The addon
  sends at most **10 chunks per second** on its own (well below `ChatFlood.AddonMessageCount`).
- **Reassembly.** Per session at most 4 partial messages; drop the oldest beyond that.
- **No side effects before validation.** A rejected intent changes nothing and produces exactly
  one `ERROR`.
- **Swaps.** `CONTROL_TAKE` / `CONTROL_RETURN` run the guards from the design log §2.8 and answer
  `SWAP_REFUSED` with a `reason`. An accepted swap is answered in this order: push
  `CONTROL_STATE^busy=1`, send the `ACK` **before** the client is moved, then run the swap.
  Further control requests get `BUSY` until it ends. The result is the next `CONTROL_STATE`
  with the new `g` and `busy=0`, plus `BOT` deltas for the two characters and `EVENT CONTROL`:
  - in-world switch (Path S, `docs/take-control.md` §5.11; as built 2026-11-11): the world
    reload of Path W without its loading screen (the client DLL keeps the map). For the addon it
    is the same as Path W below: the addon stays loaded, receives that `CONTROL_STATE`, sends a
    fresh `HELLO` and gets a full sync; `PLAYER_LEAVING_WORLD` / `PLAYER_ENTERING_WORLD` fire
    once each. `CONTROL_STATE^path` is `switch` when the server asked the DLL for it; a DLL that
    could not keep its map turns it into a Path W reload with a loading screen;
  - warm relog (Path R): after the `ACK` the server sends `CONTROL_PENDING` (§7.3) and moves
    the client to the character screen; the addon loads fresh on the new character and its
    `HELLO` gets the sync, whose `CONTROL_STATE` already carries the new `g`;
  - world reload (Path W, experimental): no `CONTROL_PENDING` and no character screen; the
    client gets a loading screen and the addon stays loaded. On completion the server pushes
    the `CONTROL_STATE` and `BOT` deltas from the new character; the addon accepts them
    because the sender is one of the owner's characters (§1), and the changed `g` triggers a
    fresh `HELLO` to the new character. If that push is lost, the changed `UnitGUID("player")`
    triggers the `HELLO` 2 s after the next `PLAYER_ENTERING_WORLD` (§5). The client keeps
    the login character's name and class in `UnitName`/`UnitClass` (§1), so the addon never
    uses them to identify the played character.
  A failed swap ends with `CONTROL_STATE^busy=0` and the old `g`, plus an `EVENT INFO` that
  says why.
- **Watch mode** (`docs/take-control.md` §5.12; as built 2026-11-12). `CONTROL_WATCH^on=1` while
  the client plays one of the owner's guild bots, or the leader once it has an order (2026-11-12,
  docs/orders.md "The leader"): the server gives that character a bot AI that
  plays its order, locks the client's movement keys (as under a fear effect) and keeps the
  character on the client — no swap, no loading screen, the addon stays as it is. `on=0` takes the
  controls back and is always accepted. Answer order: `CONTROL_STATE` (with `watch`), the bot's
  `BOT` delta (its `why` reads `Watch mode: <the planner's why line>`), then `ACK`; a request for
  the state the character already has is answered with the same `CONTROL_STATE` and `ACK`.
  Refused with `WATCH_REFUSED` and a `reason`: `NO_ORDER` (the leader without an order),
  `NOT_GUILD_BOT` (a test character),
  `BUSY` (a swap runs), `LOADING`, `GM_MODE`, `VEHICLE`, `CHARMED`, `CONTROLS_UNIT`, `DUEL`,
  `TRADE`, `BATTLEGROUND`, `LFG`, `SELF_BOT`, `SHUTDOWN`. Combat, death and flight paths do not
  refuse it. A swap requested while watching ends watch mode first (`CONTROL_STATE^watch=0`); so do
  a logout and a lost connection (the bot plays on as an ordinary guild bot). The server may also
  change it on its own (`.ga watch`); every change is pushed as `CONTROL_STATE`.

---

## 10. Versioning

- `v` is an integer. Adding opcodes, keys or enum values is **not** a breaking change and does
  not bump `v` — both sides ignore what they do not know.
- Removing or changing the meaning of a key, opcode or value bumps `v`. The server keeps
  accepting older clients down to `WELCOME^min`; older clients get `ERROR^code=BAD_VERSION` and
  the addon tells the player to update.
- The zone template has its own version `zv` so the client can cache zones between sessions.

---

## 11. Wire examples

Logical messages are shown first, then the chunk(s) as they travel (each chunk is the text
of one addon message after the `GADV` prefix).

```
HELLO^ui=0.1.0^v=1^zv=0
  -> C1,1,1:HELLO^ui=0.1.0^v=1^zv=0

WELCOME^min=1^now=1794229200^srv=0.1.0^v=1^zv=3
  -> S1,1,1:WELCOME^min=1^now=1794229200^srv=0.1.0^v=1^zv=3

BOT^g=101^n=Aldric^race=1^class=1^gender=0^lvl=12^xp=5120^xpmax=9800^s=active^zone=40^zn=Westfall^o=AUTO^why=Questing in Westfall: 6 eligible quests left^sk=186:88:150,182:40:75
  -> S7,1,1:BOT^class=1^g=101^gender=0^lvl=12^n=Aldric^o=AUTO^race=1^s=active^sk=186:88:150,182:40:75^why=Questing in Westfall: 6 eligible quests left^xp=5120^xpmax=9800^zn=Westfall^zone=40

ORDER_SET^r=4^g=101^o=LEVEL_ZONE^zone=44^aa=1
  -> C5,1,1:ORDER_SET^aa=1^g=101^o=LEVEL_ZONE^r=4^zone=44

ERROR^r=5^code=SWAP_REFUSED^reason=IN_COMBAT^msg=Aldric is in combat.
  -> S9,1,1:ERROR^code=SWAP_REFUSED^msg=Aldric is in combat.^r=5^reason=IN_COMBAT

EVENT^id=88^t=1794229260^type=INFO^g=0^msg=Rate: 50% ^ 2 ~ |cff
  (escaped)  EVENT^g=0^id=88^msg=Rate: 50% ~c 2 ~~ ~pcff^t=1794229260^type=INFO
```

A message longer than 228 body bytes is split, e.g. a 500-byte logical message with id 12 from
the server becomes `S12,1,3:<228 bytes>`, `S12,2,3:<228 bytes>`, `S12,3,3:<44 bytes>`.

**Machine-readable test vectors:** [`protocol-vectors.txt`](protocol-vectors.txt) holds 14
vectors (op + raw fields → exact encoding → exact chunks), produced by a reference encoder
written independently of the addon (`addon/tests/gen_protocol_vectors.py`). They cover escaping of every special character, empty
values, `=` inside values, field-less opcodes, 64-bit integers, tuples, a cut that has to
move back in front of a UTF-8 sequence, a cut inside an escape sequence and a three-chunk
message. The addon tests check against them; the module's tests should too.

---

## 12. Mock server

`addon/GuildAdventure/MockServer.lua` implements the server side of this spec inside the client
so the UI can be developed and demonstrated before the module exists. It receives the encoded
client chunks, answers with encoded server chunks and feeds them through the same receive path
as `CHAT_MSG_ADDON` (direction, sender, chunking and decoding are all exercised). It also
simulates progress (XP, levels, quests, deaths, renown, profession skill-ups) every few
seconds. Its zone list is a realistic sample of what `ga_zone_template` should contain; its
unlock list mirrors the example curve in the design log §2.2.

`/ga mock on|off|reset|renown <n>|away` controls it (see [addon/README.md](../addon/README.md)).
When the real server answers a `HELLO`, demo mode is not needed and stays off.
