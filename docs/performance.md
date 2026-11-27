# Server performance with many bots: diagnosis and plan

Written 2026-11-21 by the optimisation thread (branch `claude/project-thread-2ckwcz`), after the
capacity test of the same day (the design log §0 "Server capacity"). Research and plan only: nothing
in the module, the core or mod-playerbots is changed by this document. A separate thread
implements the plan; a second stress test follows. **The implementation (same day, branch
`claude/project-thread-v818j3`, core branch `claude/perf-cp11-cp12-v818j3`) and its measurements are §10, the
second stress test §11, an estimate for more than 1600 bots §12.**

Goal (the owner): many bots at once and a few extra human players (friends want to play), by
technical optimisation of the server, not by game or zone design.

Everything below was measured on this PC on 2026-11-21 with the harness in
[`tools/loadtest`](../tools/loadtest/README.md) on a throwaway instance `perf` (the integration
server's binaries of 00:58, core `ga/main` `2035b0ea0`, mod-playerbots `405f8119`, module =
rolling branch round 15; `MapUpdate.Threads = 4`; 40 test guilds of 40 guild bots, levels
10-60, order `AUTO`; no client). The integration server and the dungeon thread's instance ran
beside it, so **tick times in this document are not capacity numbers** (those are in the design log
§0, measured with every other server stopped); what the measurements show is where the time
goes, what makes ticks slow and how two runs compare. The steady state of the instance (after a
restart, five minutes settled) matches the capacity test's 1600-bot row (mean 36 ms against 30,
median 23 against 20, p99 328 against 352); it is the reference of this document ("E0").

## 1. Result in short

1. **The hitches are ours.** Once a second the order planner (`OrderService::Update`) steps every
   bot of every guild in one world tick. With 1600 bots that tick takes about 390 ms (289 ms of
   planner work on the world thread, then 97 ms of map updates that catch up), with 800 bots
   about 250 ms. These ticks are the p99 and the maximum of every stage of the capacity test. At
   1600 bots they are 4 % of the ticks and 38 % of the wall time.
2. **Two thirds to three quarters of the planner's time is route search**: `RoadMap::FindRoute`
   asks the navmesh path finder (`WalkReaches` -> `PathGenerator`) for the walks to and from road
   nodes, again for every search, and `FindFlight` runs five to eight such searches.
3. **The planner is the largest single load on the critical path.** A tick is the world thread's
   own work plus the wait for the slowest map. The planner takes 0.30 of every second of the
   world thread at 1600 bots (0.38 while many bots travel), and nothing runs beside it.
4. **In the map threads the bot AI's trigger scan costs as much as its decisions.**
   mod-playerbots re-checks about 400 triggers per bot five times a second in its reaction pass
   (694 trigger checks per bot per second); 97 % of those checks are chat command triggers that
   cannot fire, each with a string copy for a performance monitor that is switched off.
5. **Bots are updated like clients.** Every bot runs the whole `Player::Update` (all auras,
   position, regeneration, our hooks) on every world tick, three times per update of the
   creatures around it, and keeps 100 yards of creatures awake and visibility lists for a
   client it does not have. Creatures, their sight checks and visibility are 44 % of the map
   threads' CPU.
6. **Not a limit:** the database (0.02 cores), locks (no contention found), memory, the network.
   More map threads do not help: there are three big maps and each is one task.

The plan (§6) in one line: spread the planner over the ticks and cache its route searches
(module only: removes the hitches and about a fifth of the mean tick at 1600 bots, more while
bots travel), then make bots cheaper in the map threads (one small mod-playerbots patch, one
small core patch, one activation range patch), measuring after each step. Expected at 1600 bots
with four map threads: mean tick about 41 -> 16 ms, p99 about 450 -> under 100 ms.

## 2. How a tick is built

Read in the core at the pinned commit (`src/server/apps/worldserver/Main.cpp`,
`game/World/World.cpp`, `game/Maps/MapMgr.cpp`, `MapUpdater.cpp`, `Map.cpp`):

- `WorldUpdateLoop` calls `World::Update(diff)` again as soon as `MinWorldUpdateTime` (1 ms on
  our servers) has passed. **It never sleeps under load, and a lightly loaded server runs
  hundreds of short ticks a second.** CPU use therefore says little: at 800 bots the process
  burns 1.8 cores on 100 ticks a second and at 1600 bots 1.7 cores on 25.
- `World::Update`: sessions of characters that are in no map, then `MapMgr::Update`, then the
  rest (battlegrounds, query callbacks, ...) and last `ScriptMgr::OnWorldUpdate`, where every
  module's world script runs: **our planner runs here, on the world thread, alone**.
- `MapMgr::Update` schedules **one task per map** on the `MapUpdater` pool and **waits for all
  of them**. So `tick = world thread's own work + slowest map`. Eastern Kingdoms and Kalimdor
  are one task each; more threads than big maps bring nothing (the capacity test: 8 = 4).
- Maps are updated in a rotation of three steps (`mapUpdateStep`): continents get their full
  update (creatures, game objects, respawns, object updates) in one world tick of three, the
  other two are for battlegrounds and dungeons (a fourth, idle step fills the time up to
  `MapUpdateInterval`, 10 ms, on a lightly loaded server). **Players are updated in every world
  tick** whatever the step (`Map::Update`: `session->Update`, `player->Update(s_diff)`), and
  `OnPlayerAfterUpdate`, where mod-playerbots' AI runs, with them.
- The navmesh query object of a map is shared by whoever asks; the planner's path queries on the
  world thread are safe only because the map tasks are idle at that moment.

## 3. Measurements

Method: ETW CPU sampling of the whole machine at 1 kHz with stacks (`xperf`, symbols from
`worldserver.pdb`), 60 s per window; a second kind of window (30 s) with the stack of every
context switch, which shows each wait of the world thread for the maps and so every tick
exactly; mod-playerbots' own monitor (`.playerbots pmon`) for 60 s. Reports:
`analyze_profile.py`. The traces were deleted with the instance; the numbers below are copied
from the reports, whose text files are kept in `_work\perf-reports-2ckwcz` (git-ignored: call
trees and tables of every window, for the comparison after each step of the plan).

Three states were profiled: 800 bots, 1600 bots in the steady state (E0), and 1600 bots in the
quarter of an hour after all of them had been put to their levels at once ("travelling": every
bot looks for its zone, twice the route searches). The last one is the worst case a live server
sees after a restart or when many guilds change zones together.

### 3.1 Ticks

| State | Ticks in 30 s | mean | median | p90 | p95 | p99 | max (ms) |
|---|---|---|---|---|---|---|---|
| 800 bots | 3162 | 9.7 | 5.0 | 14.3 | 17.8 | 44.8 | 590 |
| 1600 bots, steady (E0) | 753 | 40.7 | 25.1 | 54.9 | 85.9 | 446 | 792 |
| 1600 bots, travelling | 430 | 71.0 | 37.5 | 87.3 | 456 | 602 | 679 |

What a tick consists of (mean ms per tick of that class):

| State | Ticks | share of wall time | length | world thread works | waits for the maps | busiest map worker |
|---|---|---|---|---|---|---|
| 800 | fastest half (1582) | 16 % | 3.0 | 0.3 | 2.7 | 2.6 |
| 800 | p50 to p95 (1421) | 49 % | 10.5 | 0.3 | 10.2 | 9.2 |
| 800 | p95 to p99 (127) | 10 % | 24.1 | 0.7 | 23.2 | 20.5 |
| 800 | slowest 1 % (32) | 26 % | 245 | 149 | 42 | 39 |
| 1600 steady | fastest half (377) | 18 % | 14.7 | 1.0 | 13.7 | 13.6 |
| 1600 steady | p50 to p95 (338) | 42 % | 37.6 | 2.1 | 35.4 | 35.2 |
| 1600 steady | p95 to p99 (30) | 26 % | 265 | 179 | 86 | 85 |
| 1600 steady | slowest 1 % (8) | 14 % | 550 | 423 | 127 | 125 |
| 1600 travelling | fastest half (216) | 16 % | 22.3 | 1.2 | 21.1 | 19.3 |
| 1600 travelling | p50 to p95 (192) | 45 % | 71.9 | 11.8 | 56.2 | 51.2 |
| 1600 travelling | 200 ms and more (30) | 50 % | 507 | 274 | 137 | 123 |

(Where work and wait do not add up to the length, the world thread was pre-empted by another
process of the box.)

In every state there is **one tick of 200 ms or more per second** (30 in 30 s; at 800 bots 30
ticks of 50 ms or more with a mean of 258 ms). In 30 of the 38 slowest ticks of the steady state
the world thread's own work is the larger part. 93 % of the world thread's CPU is our planner
(`ScriptMgr::OnWorldUpdate` -> `OrderService::Update`); in the slow ticks of the travelling
state 99 % of its samples are in `OrderService::Step`, 69 % in `RoadMap::FindRoute` (67 % in the
path finder below it) and 47 % in `OrderService::FindFlight`.

### 3.2 CPU by kind of work

Milliseconds of CPU per second of wall time, all threads together:

| Thread | Work | 800 bots | 1600 steady | 1600 travelling |
|---|---|---|---|---|
| world | planner: route search (`FindRoute`, `WalkReaches`, `PathGenerator`) | 165 | 206 | 266 |
| world | planner: the rest | 50 | 93 | 109 |
| world | everything else of the world thread | 30 | 22 | 17 |
| map | bot AI: reaction pass (trigger scan) | 156 | 189 | 218 |
| map | bot AI: main engine (triggers, values, actions) | 105 | 208 | 227 |
| map | bot AI: the rest of `UpdateAI` | 21 | 16 | 10 |
| map | our per-tick player hook (auto loot scan) | 27 | 30 | 23 |
| map | players: auras (`Unit::_UpdateSpells`) | 207 | 159 | 122 |
| map | players: movement and position | 79 | 42 | 33 |
| map | players: the rest of `Player::Update` | 95 | 84 | 71 |
| map | creatures: sight checks after moves (`AIRelocationNotifier`) | 87 | 103 | 102 |
| map | creatures, pets, game objects: the rest | 643 | 364 | 260 |
| map | visibility after moves (`HandleDelayedVisibility`) | 97 | 122 | 134 |
| map | sessions, the rest | 40 | 31 | 30 |

Read it with §2 in mind: work that is done once per tick (auras, creature updates) costs more
per second at 800 bots, where the loop spins at 100 ticks a second, than at 1600 bots with 25.
Work that follows the clock (the planner's 1 Hz pass, the AI's delays) grows with the bots.

The same per tick, steady state at 1600 bots (ms of CPU of all threads in one tick):

| Work | fastest half (15 ms) | p50 to p95 (38 ms) | slowest 5 % (325 ms) | all ticks |
|---|---|---|---|---|
| planner: route search | 0.0 | 0.0 | 166 | 8.4 |
| planner: the rest | 0.3 | 1.1 | 62 | 3.8 |
| bot AI: reaction pass | 3.0 | 10.8 | 27 | 7.7 |
| bot AI: main engine | 5.2 | 7.1 | 53 | 8.5 |
| our per-tick player hook | 0.9 | 1.2 | 4.5 | 1.2 |
| players: auras, movement, the rest of `Player::Update` | 10.6 | 11.6 | 21 | 11.6 |
| creatures, sight checks | 4.3 | 32.4 | 46 | 19.0 |
| visibility after moves | 0.9 | 7.0 | 27 | 5.0 |

The fastest half are the ticks without a full continent update (players and AI only), the
middle the ticks with one, the slowest the planner's pass and the burst after it.

Who owns the sampled code in the steady state: core 51 %, mod-playerbots 13 %, Detour (navmesh)
7 %, the C++ runtime and system libraries 26 % (allocations, strings, hashing, most of it called
by mod-playerbots and the path finder), our module's own functions 2.5 %. But 17 % of all
samples run under our planner and 29 % under mod-playerbots' `UpdateAI`.

Hottest functions in the steady state (the function itself plus runtime code it calls
directly): `Engine::ProcessTriggers` 3.7 %, `Trigger::needCheck` 3.4 %, `Unit::_UpdateSpells`
3.3 %, `dtQueryFilterExt::getCost` 2.7 %, `Acore::AIRelocationNotifier::Visit` 2.6 %,
`ChatCommandTrigger::Check` 2.4 %, `Object::GetGuidValue` 2.4 %, `AuraEffect::Update` 2.2 %,
`Aura::UpdateTargetMap` 2.0 %, `EventProcessor::Update` 1.6 %,
`AiObjectContext::GetUntypedValue` 1.1 %, `PerfMonitor::start` 0.9 % (the monitor is off),
`AiObjectContext::GetTrigger` 0.9 %. In the travelling state `RoadMap::FindRoute` is 14.9 % of
all samples of the process (inclusive), `OrderService::FindFlight` 9.3 %, `FindErrand` 4.3 %,
`ChooseZone` 1.4 %.

### 3.3 mod-playerbots' monitor (60 s, 1600 bots)

- 66.6 million trigger checks = 694 per bot per second, 402 different triggers. 128 of them are
  chat command triggers, each checked about five times a second per bot (64.5 million of the
  checks) although no bot got a command.
- Time inside the checks 7.7 s, in values 5.9 s (`new rpg quest giver` 0.68 s, `nearest enemy
  players` 0.48 s, `possible new rpg targets` 0.45 s, `grind target` 0.41 s), in actions 4.8 s
  (`new rpg go camp` 1.57 s, `add gathering loot` 0.91 s for 28 309 calls, `new rpg do quest`
  0.46 s). The sampling profile charges the scan around the checks (the loop, `needCheck`, name
  strings, the monitor's own call) with more than the checks themselves.
- The monitor is expensive: while it was on, the mean tick rose from about 80 to 138 ms. Never
  leave it on.

### 3.4 Waits

No lock contention: of 98.8 thread-seconds the four map workers were off the CPU in 30 s, 84.7
were waiting for work; the world thread waited 18.6 of its 21.9 s for the maps. What is left are
pre-emptions in the middle of code (other processes on the box), not locks.

## 4. Findings

**F1. The planner's 1 Hz pass is the hitch.** `OrderService::Update` collects `diff` until
`TickMs` (1000) and then calls `Step` for every bot of every guild in that one world tick
(`OrderService.cpp`, the loop over `GetGuilds()`); the 250 ms loop above it (pull check, dungeon
conductor, ground effects) does the same four times a second. A guild of 40 bots costs a few
milliseconds this way, which nobody sees; 1600 bots cost 289 ms on average and up to 420 ms.
After such a tick every bot's AI timers have run out together, so the map update that follows is
a burst too (main engine 53 ms of CPU in a slow tick against 5-7 ms in the others).

**F2. Route search is most of the planner.** `RoadMap::FindRoute` scans all 2585 nodes, then
proves the walk from the start to its nearest nodes and from the nodes nearest to the
destination with `WalkReaches` (up to 16 path queries per end until 8 nodes are usable, each a
navmesh path of up to 800 yd; only a node within 8 yd is trusted without one), and allocates
per-node arrays for its Dijkstra. `OrderService::FindFlight` (once when a travel begins and once
a minute while it walks; also per candidate in `FindErrand`) calls `WalkLength` for the
destination and for up to three flight masters and `FindRoute` for up to three landings: five
to eight route searches from the same spot, each proving the same start walks again. Nothing is
remembered between calls or between bots, although the navmesh, the road nodes, the flight
masters and the landings never change.

**F3. The world thread's work is serial.** While the planner works, the four map workers
sleep. In the steady state the world thread uses 0.31 cores (each map worker 0.35); while the
bots travel it is the busiest thread of the process (0.40 against 0.31).

**F4. The reaction pass re-checks every trigger.** `ReactionEngine::Update` runs
`Engine::ProcessTriggers` every `Playerbots.ReactDelay` (100 ms; under load once per tick of the
bot, about five times a second) for a bot that is "active", and every guild bot is (the design log §0
"Server capacity" (e)). The scan walks about 400 trigger nodes, builds a `std::unordered_map`
per call, and for every trigger that is due calls `sPerfMonitor.start(PERF_MON_TRIGGER,
trigger->getName(), ...)`: `getName()` returns a `std::string` by value, so each check allocates
and frees a string for a monitor that is off. **97 % of all trigger checks are chat command
triggers**: 128 of the 402 triggers (`c`, `reputation`, `craft`, `ready check`, `wts`, `wipe`
...). `ChatCommandTrigger::Check()` returns an empty event unless a command set its `triggered`
flag, and `Trigger::needCheck` says "always" for them (their check interval is 1).

**F5. Bots run the whole `Player::Update` on every world tick.** `Unit::_UpdateSpells` walks
every aura the character owns, passive ones included, on every call; position updates look up
terrain and liquid; our `LootService::OnCharacterUpdate` looks up a string-keyed timer
(`player->CustomData.GetDefault<AutoLootTimer>("ga-auto-loot")`) per bot per tick. Creatures are
updated once per rotation of three ticks, the bots three times.

**F6. Every bot keeps a client's surroundings alive.** `GetGridActivationRange()` of a player is
the map's visibility range (100 yd on continents, 4 x 4 cells of 66 yd), so the creatures of
those cells are updated, creatures on waypoints for as long as their grid is loaded, and every
move of a bot runs the visibility notifiers (`PlayerRelocationNotifier`, `UpdateVisibilityOf`,
`CanSeeOrDetect`) for a client that is not there. Creatures and visibility are 44 % of the map
threads' CPU at 1600 bots. Measured (§7 E1): with 60 yd the three kinds of work cost a third
less per tick and the mean tick falls by a quarter.

**F7. The loop spins.** With `MinWorldUpdateTime = 1` an 800-bot server runs 100 ticks a second
and uses 1.8 cores where the same simulation at 10 ms ticks would need well under one. It costs
no latency, only CPU that builds and other servers on this PC could use.

**F8. Ruled out by measurement:** database (0.02 cores, no synchronous query in the hot path),
lock contention (§3.4), player saves (0.0 %), packets to bot sessions (1 %), logging (0.1 %),
`GroundGuard` (0.2 %), `GuildChatThrottle` and the other packet hooks (below the report's 0.5 %
line), the number of map threads beyond the number of big maps.

## 5. What decides the tick time

Under load the loop never sleeps, so a second of wall time is filled by two kinds of work on the
critical path (the world thread plus the slowest map task, which carries about 51 % of the map
work):

- work that follows the clock, whatever the tick rate: the planner (299 ms per second at 1600
  bots) and the AI's passes (413 ms across the map threads, about 213 on the slowest map).
  With the rest of the world thread a fraction `F = 0.53` of every second;
- work per tick: auras, positions, creature updates, visibility, 38 ms of CPU per tick across
  the map threads, `c = 19.6 ms` on the slowest map.

The mean tick is then `T = c / (1 - F)`: `19.6 / 0.47 = 42 ms`, against 40.7 measured. The same
sum for the other measured states: 1600 bots travelling `30 / 0.35 = 86 ms` (71-90 measured),
with a visibility distance of 60 yd (§7 E1) `14.7 / 0.48 = 30.5 ms` (29.6 measured), with the
AI's delays doubled (§7 E2) `16.8 / 0.55 = 30.7 ms` (30.1 measured).

Two things follow. `F` reaches 1 at roughly 3000 bots (2500 while they travel): beyond that the
server falls behind for good, whatever the number of threads. And near `F = 1` every change of
the clock-driven work swings the tick a lot, so **lowering `F` (planner, AI passes) is worth
more than lowering `c`**, and more so the more bots there are.

The model is rough (the per-tick work is not quite linear in the bots, the share of the slowest
map moves, and a mean says nothing about hitches), so the gains in §6 are estimates to be
checked by the second stress test, not promises.

## 6. Plan

In this order. Each step is measured with the harness before the next one starts (same instance
recipe, 800 and 1600 bots, steady state, a `-Waits` window for the ticks). "Gain" is the
expected effect at 1600 bots in the steady state with four map threads unless it says otherwise;
"mean" is the model's mean tick after this step and all steps before it (today 41 ms).

| # | Step | Where | Expected gain | Effort | Risk |
|---|---|---|---|---|---|
| PF-0 | Counters of our own: planner time per tick, route searches, cache hits, a line for a slow planner tick | module | none by itself; makes PF-1/2 checkable without xperf | S | none |
| PF-1 | Spread the planner's passes over the ticks, with a budget per tick | module | p99 450 -> about 120 ms, max 790 -> about 200 ms; at 800 bots p99 45 -> about 20 ms, max 590 -> under 60 ms; mean unchanged (41 ms) | S | low |
| PF-2 | Remember route searches (leg cache, landing routes, one `FindFlight` scope) | module | route search 206 -> about 60 ms per second; mean about 32 ms (-22 %); while bots travel 86 -> about 48 ms | M | low-medium |
| PF-3 | Our per-tick hook in the map threads (auto loot timer and scan) | module | about -1.5 % of the map threads' CPU; mean about 31 ms | S | none |
| PF-4 | `MinWorldUpdateTime = 10` | config | no latency gain; an 800-bot server uses about a third of the CPU it burns today | - | none |
| PF-6 | mod-playerbots: the trigger scan (chat command triggers only when a command is pending: 97 % of the checks; no name string for a monitor that is off) | patch file for the pinned module | reaction pass 189 -> about 35 ms per second, main engine about -25 %: -15 % of the map threads' CPU; mean about 26 ms | S | low |
| PF-5 | Bots without a client are updated on their map's full step only | core patch | per-tick work on the slowest map -4.4 ms (two of three `Player::Update` calls per rotation saved); mean about 20 ms | S-M | medium |
| PF-7 | Bots nobody watches keep fewer creatures awake (activation range 60 yd) | core patch | creatures, sight checks and visibility about -30 % (measured stand-in: -33 %, §7 E1); mean about 16 ms | S-M | medium |
| PF-8 | Route searches off the world thread (a worker with its own navmesh queries, while the maps update) | module + core patch | the rest of the planner's route time off the critical path | L | high |
| PF-9 | Second stress test with every other server stopped | harness | the new capacity table | S | none |

Module-only steps PF-0 to PF-3 come first and need no decision by the owner. PF-5 to PF-7 change
the core or mod-playerbots and are his call (§8); PF-6 comes before PF-5 because it changes no
behaviour. PF-8 is parked: only if the world thread is still the limit after PF-2.

With all of PF-1 to PF-7 the model gives a mean tick of about 16 ms at 1600 bots (today 41) and
the smoothness of today's 1300 bots at about 2000; `F` would reach 1 at about 5700 bots instead
of 3000.

### PF-0 Counters of our own

`OrderService` measures its own work per world tick with a steady clock (planner ms in this
tick, bots stepped, route searches and their ms, cache hits and misses) and keeps the last
minute. `.ga perf` (GM, console, SOAP) prints: ticks, planner ms per tick (mean, p95, max),
route searches per second and ms each, cache hit rate, the slowest bot step of the minute with
its task kind. One activity-log line when the planner's part of a tick is above
`GuildAdventure.Perf.SlowTickMs` (default 50; 0 = never), naming the three most expensive steps.
Cost: two clock reads per bot step.

### PF-1 Spread the planner over the ticks

`OrderService::Update` today: `_timer >= TickMs` then all guilds, all bots; `_pullTimer >= 250`
then all runtimes.

- Give every bot a phase (`guid` counter modulo 1000 ms, squad members the phase of their leader
  plus a few ms so that a leader is stepped before its members) and a due time. Every world tick
  steps the bots whose due time has passed, in due order, and sets the next due time one period
  later (not `now + 1000`, so the rate holds). Guild-level work (`sQuestPartners.Update`,
  `AutoPair`, `LearnStoredRecipes`) gets a phase per guild the same way.
- The 250 ms loop the same with a phase modulo 250.
- A budget per world tick (`GuildAdventure.Planner.BudgetMs`, default 4): when the planner has
  used it, the bots still due wait for the next tick (first in, first out). Under overload the
  period stretches instead of the tick; PF-0 counts how often and by how much. The budget is at
  least a quarter of the last tick's length, so the planner cannot fall behind without bound.
  (At 1600 bots before PF-2 the planner needs about 12 ms per tick: its period stretches to
  about 1.2 s until PF-2 halves the need.)
- At most one expensive search (`FindFlight`, `FindErrand`, `ChooseZone`) per bot step already
  holds; add at most `GuildAdventure.Planner.SearchesPerTick` (default 2) per world tick, the
  others keep their task and ask again at their next step.
- Code that assumes "one step = one second" (`+= TickMs` in five places: the stuck and approach
  clocks, `PauseAI(2 * TickMs)`) uses the real time since the bot's last step.

Acceptance: at 800 bots p99 below 30 ms and max below 80 ms; at 1600 bots no tick above three
times the median in a 30 s `-Waits` window; quests and deaths per bot hour of a 20-minute run
unchanged within noise (the activity log's counters).

### PF-2 Remember route searches

- **Leg cache.** `WalkReaches(from, to)` answers and lengths in a table keyed by map, the road
  node (or flight master, landing, bridge point) and the other end rounded to 8 yd. Static ends
  (node to node, master to node) never expire; an end at a character's position lives ten
  minutes; the table is capped (LRU). Cleared when `ga_road_block` or `ga_path_bridge` change.
- **One scope per `FindFlight`.** The start walks from the bot's position are proven once and
  reused by its five to eight route searches (today each search proves them again).
- **Landing routes.** `FindRoute(landing -> destination)` depends on nothing of the bot but team
  and level band: cached for a few minutes by landing node, destination rounded to 8 yd, team,
  level.
- **Cheaper search itself.** A grid over the nodes for "nearest nodes" instead of the scan of
  2585; the Dijkstra arrays kept per thread instead of allocated per call.
- Negative answers are cached as well (they are the expensive ones: a path that does not exist
  searches the whole node budget).

Acceptance: route search CPU per second at 1600 bots at or below 70 ms (PF-0), cache hit rate
above 70 %; the routes of a fixed list of 200 start/destination pairs equal before and after (a
test command that dumps route `via` strings).

### PF-3 Our per-tick hook in the map threads

`LootService::OnCharacterUpdate` runs for every player on every tick: keep the scan timer where
no string lookup is needed (a member of a per-player struct fetched once, or a due time checked
against `getMSTime()`), and arm the corpse scan from the kill (`OnPlayerCreatureKill` sets "loot
due" for a few seconds) instead of scanning the grid four times a second for every bot. Check
`GuildAdventureAfterUpdateScript::OnPlayerAfterUpdate` for the same pattern.

### PF-4 `MinWorldUpdateTime`

`tools\configure-server.ps1` writes `MinWorldUpdateTime = 10`. A tick is then at least 10 ms and
a continent's full update comes at most every 30 ms (the rotation of §2); a loaded server is not
affected. The capacity table's CPU column changes its meaning (it then shows work, not
spinning).

### PF-6 mod-playerbots: the trigger scan

Two small changes, kept as patch files in this repository (`patches/mod-playerbots/*.patch`,
applied by `tools\setup-server.ps1` after the checkout of the pinned commit; D13 and DQ5 allow
it) and offered upstream:

1. A trigger can say that it has nothing to check: `Trigger::needCheck` becomes virtual (or gets
   an `IsPending()` beside it) and `ChatCommandTrigger` answers with its `triggered` flag, so the
   128 chat command triggers cost one flag test each per pass and nothing else. Behaviour is
   unchanged: `Check()` returned an empty event in exactly those cases.
2. `Engine::ProcessTriggers` (and the action and value calls that do the same): build the
   monitor's name only when `sPlayerbotAIConfig.PerfMonEnabled` is on; a small vector instead of
   the `std::unordered_map` of fired triggers.

A third change is an option, not part of the step (it changes behaviour; §7 E2 measures its
effect): the reaction pass of a bot without a client master repeats after a config value
(for example 300 ms) instead of `Playerbots.ReactDelay`.

To check: `.playerbots pmon` trigger checks per bot per second down by at least 90 %; a command
whispered to a bot by its owner (`follow`, `stay`, a `.playerbots exec`) still acts at once;
fight metrics of a 20-minute run unchanged within noise.

### PF-5 Bots without a client: one `Player::Update` per rotation (core patch CP-11)

In `Map::Update`, for a player whose session has no socket (`WorldSession::IsSocketClosed()`;
a character its owner plays has one, so take control and watch mode are untouched): on a step
that is not the map's full step, skip `player->Update(s_diff)` and add `s_diff` to a per-player
counter; on the full step call `Update` with the sum. `session->Update` (packet handling) stays
as it is. A config switch (`GuildAdventure`-independent, e.g. `Bots.UpdateOnFullStepOnly = 1`)
and the `// GuildAdventure:` marker as for every core patch; `docs/core-patches.md` gets CP-11.
The bot AI runs inside `Player::Update`'s hooks, so it is called once per rotation too; its
work follows its own delays and stays the same. Little can be seen of it: creatures look at a
bot only in their own update, which is the full step, other bots likewise, and a client draws a
bot's movement from its spline, not from the server's position updates.

To check: regeneration, aura durations and cooldowns of a bot over ten minutes against the
clock; a swap onto a bot and back (Paths R, W, S) and watch mode (ga-probe `swap-relog`,
`swap-switch`, `watch-mode`); a dungeon run (dungeon maps have their own full step).

### PF-7 Fewer creatures awake around bots nobody watches (core patch CP-12)

`WorldObject::GetGridActivationRange()` returns a configured range (default 60 yd) for a player
without a socket, the map's range as today for everybody else, and the map's range also for a
bot within that range of a client's character or in a group with one. Measured stand-in: §7 E1
(the whole map at 60 yd).

To check: the risk model's assumptions (patrols outside the range stand still until a bot comes
near: `Escape::RoamerNear` and the pull check see them where they stand), respawns, escort
quests (the escorted NPC is an active object), deaths per bot hour.

### PF-8 Route searches off the world thread (parked)

The world thread waits for the maps for most of every tick; route searches are pure functions
of static data plus the navmesh. A worker with its own `dtNavMeshQuery` objects could answer
queued searches meanwhile. It needs a lock around navmesh tile loading in the core (tiles are
added by map tasks when grids load) and a planner that can wait a tick for an answer. Only if
PF-2 leaves the world thread as the limit.

### After the plan: what stays

One task per continent stays the structural limit; no cheap way around it exists in this core
(splitting a map's update across threads touches every interaction between cells; several
processes share a realm by map, not within one). What the second stress test has to show is
whether the estimates above hold.

## 7. Config experiments

Each after a restart of the instance (the 1600 bots come back by themselves in under a minute),
five minutes to settle, two minutes of `.server info` samples (eight reads of the last 500
ticks) and two profiles. Other GA servers were running, so only the comparison between the rows
counts.

`.server info`, means of the eight reads (ms):

| Run | Change | mean | median | p95 | p99 | max |
|---|---|---|---|---|---|---|
| E0 | none (baseline after a restart) | 36.0 | 23.4 | 100 | 328 | 510 |
| E1 | `Visibility.Distance.Continents = 60` (was 100) | 29.8 | 19.8 | 81 | 322 | 427 |
| E2 | `Playerbots.ReactDelay = 200` (was 100), visibility back to 100 | 27.4 | 19.4 | 66 | 323 | 428 |

Ticks of the 30 s trace and CPU per tick of the work the change aims at (ms, all threads):

| Run | ticks | mean | median | p90 | p95 | p99 | creatures + sight checks | visibility | AI reaction pass | AI main engine |
|---|---|---|---|---|---|---|---|---|---|---|
| E0 | 753 | 40.7 | 25.1 | 54.9 | 85.9 | 446 | 19.0 | 5.0 | 7.7 | 8.5 |
| E1 | 1034 | 29.6 | 19.8 | 38.8 | 55.4 | 351 | 13.7 | 2.5 | 5.9 | 6.5 |
| E2 | 1016 | 30.1 | 19.8 | 43.2 | 59.6 | 386 | 17.7 | 3.7 | 3.3 | 4.2 |

- **E1:** creature updates -26 %, their sight checks -36 %, visibility -51 % per tick, together
  a third less; the mean tick -27 %, p95 -35 %. The hitches stay (p99): they are the planner's.
  The setting itself is no option for the live server (it also cuts what a player sees to 60
  yd); PF-7 does the same for bots nobody watches only.
- **E2:** the reaction pass costs 43 % less per second (189 -> 108 ms) and the main engine a
  third less (208 -> 139 ms): `Playerbots.ReactDelay` is also the unit of the main engine's
  delays (`PlayerbotAI::GetReactDelay`). The mean tick -26 %, p95 -31 %; the hitches stay. As a
  setting it is no option either: every bot then thinks half as often, in fights too. It shows
  what the AI's clock-driven share is worth: PF-6 takes more off the scan (the checks that
  cannot fire, not the rate) and changes nothing a bot does.
- Both runs fit the model of §5 (E1 `14.7 / 0.48 = 30.5 ms`, E2 `16.8 / 0.55 = 30.7 ms`), which
  is what the estimates of §6 rest on.

## 8. Decisions for the owner

the owner gave the go for the plan on 2026-11-21 ([quote left out]); the defaults below were taken, with the
implementation's rulings in the design log §7 PD1 (CP-11 as a phased interval, CP-12 off by default) and §10 here.

- **PD1 - Core and mod-playerbots patches for performance (PF-5, PF-6, PF-7).** They are small,
  switchable by config and documented like CP-1..CP-10. The mod-playerbots patch (PF-6) changes
  no behaviour. The two core patches change how bots nobody watches are simulated: such a bot is
  updated once per map rotation instead of on every world tick (under load about three times
  less often per second), and it wakes creatures within 60 yd instead of 100.
  *Proposal: yes to all three, each behind its config key, each measured on its own.*
- **PD2 - How many bots the server should hold.** The plan stops when the second stress test
  shows 1600 bots at p95 below 50 ms and p99 below 100 ms with four map threads. *Proposal: that
  target (40 full guilds); PF-8 only if a later target needs it.*
- **PD3 - `MinWorldUpdateTime = 10` on the live server (PF-4).** No effect on play; the server
  thread sets it with the next restart. *Proposal: yes.*

## 9. Repeat the measurements

[`tools/loadtest/README.md`](../tools/loadtest/README.md): set up the instance, log the bots in,
`sample`, `profile.ps1`, `analyze_profile.py`, tear down. One profile window per stage is enough
to see whether a step did what §6 expects: the "CPU by kind of work" table and the tick table of
a `-Waits` report are the two to compare. Sample a steady state (restart, five minutes), not
the minutes after a boost.

## 10. Implementation and measurements (2026-11-21)

Built the same day by the implementation thread (branch `claude/project-thread-v818j3`: module, tools, the
mod-playerbots patch file; core branch `claude/perf-cp11-cp12-v818j3` of `Shoro2/azerothcore-ga`: CP-11, CP-12).
Measured on a throwaway instance `pf` (ports 3756/8117/7910, `GA_pf_*` cloned from the integration server, binaries
built from the thread's own trees), with the 40 test guilds of the capacity test: 1600 guild bots on `AUTO`, four map
threads. Every run: a restart with its config (`tools/loadtest/restart-with.ps1`), the 1600 bots back in the world,
five minutes to settle, three minutes of `.server info` (12 reads of the last 500 world ticks), then `.ga perf`. The
integration server and dclear ran on the same PC, so the rows compare with each other; PF-9 gives the capacity table.

### 10.1 What is built

| Step | What | Switch (default) | Where |
|---|---|---|---|
| PF-0 | `Perf.h` counters (route searches and their µs, walks asked of the path finder and from the cache, flights, errands, zone picks), the planner's work per world tick kept for a minute; `.ga perf [clear]` (GM, console, SOAP): ticks, planner ms per second and per tick (mean, p95, max), steps, budget cuts, lateness, route searches, cache hits, the slowest step; an activity line `the planner took N ms in one world tick` naming the three most expensive steps | `GuildAdventure.Perf.SlowTickMs` (50; 0 = never) | module `7a5a3f6` |
| PF-1 | `OrderService::UpdateSpread`: the 4 Hz loop per character at a phase of its guid (modulo 250 ms), the 1 Hz steps of guilds, bots and leaders each at a phase (modulo 1000 ms; a squad member 1 ms after its leader), due list sorted by due time, a budget per world tick; the next due time is one period after the phase, not `now + 1000`; the old "one second" clocks (`+= TickMs`, `PauseAI(2 * TickMs)`) use the real gap since the bot's last step (`BotRuntime::stepGapMs`) | `GuildAdventure.Planner.Spread` (1; 0 = all in one tick as before), `.Planner.BudgetMs` (4; at least a quarter of the last tick, and the whole last tick once the oldest due step is 500 ms late), `.Planner.SearchesPerTick` (0 = no cap) | module `7a5a3f6` |
| PF-2 | A walk cache in front of `WalkReaches` (`PathProbe.cpp`): answers and lengths by map, both ends rounded to 8 yd, phase, a headless session (the path filter of bots) and water; a yes lives ten minutes, a no three, capped at 400,000 (then cleared); `.ga perf clear` empties it. The Dijkstra arrays of `RoadMap::FindRoute` are kept per thread | `GuildAdventure.Planner.RouteCache` (1) | module `7a5a3f6` |
| PF-3 | Auto loot armed by the kill (`OnPlayerCreatureKillCredit`: the killer and its group within 100 yd for 30 s); the corpse scan runs only while armed, gathering as before; the scan's 250 ms throttle is a phase test without a per-character lookup | `GuildAdventure.AutoLoot.ArmByKill` (1) | module `7a5a3f6` |
| PF-4 | `MinWorldUpdateTime = 10` written by `tools\configure-server.ps1` | config | tools `dee530a` |
| PF-6 | `patches/mod-playerbots/0001-skip-idle-triggers.patch` (idle chat command triggers skipped, no monitor names while the monitor is off, a small vector of fired triggers), applied by `tools\apply-playerbots-patches.ps1` (called by `setup-server.ps1`) | `Playerbots.SkipIdleTriggers` (1) | tools `dee530a` |
| PF-5 | Core patch CP-11: a bot is updated once every `Bots.UpdateInterval` ms at a phase of its own (docs/core-patches.md CP-11) | `Bots.UpdateInterval` (75; 0 = every tick) | core `e9b1cb5dd`, `8575b417a` |
| PF-7 | Core patch CP-12: a smaller grid activation range for bots on continents (docs/core-patches.md CP-12) | `Bots.GridActivationRange` (0 = off; the plan proposed 60) | core `bdb6f8e19` |

Not built of the plan: the landing route cache, one scope per `FindFlight` and a grid over the road nodes (PF-2):
the walk cache answers 77 % of the walks and route search stays below the acceptance line of 70 ms per second
without them. PF-8 stays parked.

### 10.2 Ticks

`.server info`, means of the 12 reads (ms); CPU and the other GA servers as in the introduction:

| Run | Module (PF-1..3) | PF-6 | CP-11 | CP-12 | mean | median | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|---|
| e0, series 1 | off | off | off | off | 31.5 | 16.8 | 84 | 403 | 618 |
| e1 | on | off | off | off | 27.3 | 26.3 | 49 | 56 | 63 |
| e2 | on | on | off | off | 24.9 | 22.8 | 45 | 51 | 59 |
| e3, CP-11 first version | on | on | full step | off | 34.6 | 11.6 | 90 | 100 | 126 |
| e4, CP-11 first version | on | on | full step | 60 yd | 25.7 | 11.0 | 64 | 70 | 77 |
| e3, series 2 | on | on | 75 ms | off | 21.1 | 18.4 | 40 | 48 | 60 |
| e4, series 2 | on | on | 75 ms | 60 yd | 30.3 | 29.0 | 57 | 67 | 88 |
| e5 | on | on | off | 60 yd | 28.0 | 26.0 | 50 | 56 | 63 |
| e0, final | off | off | off | off | 36.9 | 21.1 | 108 | 405 | 543 |
| **defaults, final** | on | on | 75 ms | off | **19.8** | **16.8** | **37** | **42** | **49** |

- **PF-1 to PF-3** took the planner's hitch away: p99 403 -> 56 ms, max 618 -> 63 ms (the mean of the reads' maxima);
  the mean tick fell 13 %. The hitches of §4 F1 are gone at 1600 bots. The median rose (16.8 -> 26.3 ms): every tick
  now carries its share of the planner (the budget of 4 ms and more under load) instead of one tick a second carrying all.
- **PF-6** 9 % off the mean, as expected from §7 E2's direction (no behaviour change).
- **CP-11, first version** (every bot on its map's full step) made it worse: the full step then carried every bot's
  update and AI together with the creatures (p95 45 -> 90 ms) while the other ticks fell to 12 ms (median). The
  phased interval of `8575b417a` spreads the bots over the ticks.
- **CP-12** made it slower in both comparisons (e4 against e3 of series 2, e5 against e2): a creature a bot sees stays in the map's update list anyway (`Creature::IsUpdateNeeded`: a visible player keeps it), and the creatures the smaller cell marking lets fall out of the list are put back by the next visibility pass of a bot (`Player::UpdateVisibilityOf` adds every target it looks at), so, as far as the code reads, the list churns (not profiled on its own). Off by default (core `0fd0b6a82`); the stress test may try 60 again, it is one config line.
- **The defaults** (e3 of series 2, repeated as the final run): mean 19.8 ms, p95 37, p99 42, max 49: PD2's target (p95 below 50, p99 below 100 ms at 1600 bots) holds on this run, which had the integration server and dclear beside it. The `-Waits` trace of the final runs (30 s each): everything off 791 ticks, mean 38.5 ms, p99 495, max 669, the world thread's own work 14.3 ms per tick; the defaults 1498 ticks, mean 20.5, p99 41, max 55.5, the world thread 4.9 ms. Of the slowest 5 % of the ticks the world thread made the larger part in 30 of 40 before and in 2 of 75 after.

### 10.3 The planner (`.ga perf`, last minute of the run)

| | all off | final defaults |
|---|---|---|
| world ticks a second | 26 | 49 |
| planner ms of every second | 362.6 | 207.0 |
| planner ms per tick: mean / p95 / max | 13.9 / 9.2 / 579.8 | 4.2 / 10.4 / 24.4 |
| route searches a second, ms each, ms of every second | 122.2, 2.00, 244.9 | 112.1, 0.56, 62.6 |
| walks asked of the path finder a second | 698.4 | 159.1 |
| walks from the cache | 0 % | 75 % |
| flights, errands, zone picks a second | 16.3, 49.3, 11.6 | 15.2, 47.7, 10.5 |

PF-2's acceptance (route search at or below 70 ms per second, more than 70 % from the cache) is met. Steps are not
counted in the all-in-one mode.

### 10.4 Behaviour

Activity lines per minute in the sampling windows (the same three minutes as the ticks; the activity log):

| Run | quests turned in | deaths | fights | fights left | rescues | "is stuck" | rests | slow planner tick lines |
|---|---|---|---|---|---|---|---|---|
| everything off (final) | 83.4 | 6.6 | 1042 | 48.5 | 6.3 | 3.3 | 266 | 5.9 |
| the defaults (final) | 77.2 | 7.0 | 1060 | 47.7 | 4.0 | 1.3 | 263 | 0 |
| e3, series 2 (the defaults) | 70.0 | 5.0 | 1099 | 38.7 | 2.0 | 2.0 | 278 | 0 |
| e4, series 2 (with CP-12) | 79.6 | 3.6 | 1056 | 49.1 | 7.3 | 2.7 | 266 | 0 |

Three minutes per run are short windows; between the two final runs (everything off against the defaults) quests,
deaths, fights and rests stay within the spread of the other runs, so no change of behaviour is seen. The slow tick
line (a planner tick of 50 ms or more, at most one per 10 s) fires every ten seconds with the planner in one tick and
never with the spread.

CPU per second of wall time by kind of work in the two final traces (ms of all threads, the analyser's table):

| Work | everything off | the defaults |
|---|---|---|
| planner: route search | 250 | 78 |
| planner: the rest | 97 | 129 |
| bot AI: reaction pass | 116 | 61 |
| bot AI: main engine | 202 | 222 |
| players: auras, movement, the rest of `Player::Update` | 291 | 203 |
| our per-tick player hook (auto loot) | 27 | 17 |
| creatures, pets, game objects; their sight checks; visibility after moves | 582 | 981 |
| process CPU | 1.65 cores | 1.82 cores |

The work that follows the clock fell (planner, AI reaction pass, the bots' own updates); the work that follows the
tick grew because there are twice as many ticks a second (49 against 26): a continent's creatures are updated on its
full step, every third tick. That is the next limit: one task per continent, its creatures. At this load
`MinWorldUpdateTime = 10` (on in every run) does not bite; a server with fewer bots stops spinning at 10 ms ticks.
PF-6 took less off the reaction pass than estimated (116 -> 61 ms, the plan said about 35): the main engine's own
trigger checks were not touched by it.

### 10.5 What is left for PF-9

Take control with CP-11 on (ga-probe against `pf` with the 1600 bots): `connect` 5/5 and `swap-relog` 20/20.
`watch-mode` stopped at the same two checks with `Bots.UpdateInterval = 0` as with 75 (the forward key moved the leader
0 yd, and the relog of the probe guild's account timed out while it prepared the character list), so it says nothing about
CP-11; watch mode with CP-11 is left for a run on a lighter instance.

the integration server installs the branches (core: a full rebuild, `Player.h` and `WorldConfig.h` change; module:
`build-server.ps1 -Reconfigure` for `Perf.cpp`; `tools\apply-playerbots-patches.ps1` on the integration checkout's
mod-playerbots; `configure-server.ps1 -Force` for `MinWorldUpdateTime`; the new keys of the three `.conf.dist`
files are optional, their defaults are on). Then the stress test thread runs the capacity table again with every
other server stopped.

## 11. PF-9: the second stress test (2026-11-21)

Every other GA server stopped (integration server from 09:18, dclear from 09:40 local), throwaway instance `load` made
with `tools\loadtest` from the integration server's round-15 binaries and configs (core `0fd0b6a82`, module `04d051b`,
the playerbots patch, `MinWorldUpdateTime = 10`, four map threads, `Bots.UpdateInterval = 75`,
`Bots.GridActivationRange = 0`); 40 test guilds of 40 bots, boosted to levels 10-60 and left 12 minutes to spread
(47 zones: Eastern Kingdoms 809, Kalimdor 690, Outland 101), then restarted and settled 5 minutes before each sample
(`.server info` every 15 s for 3-4 minutes; `.ga perf` after each). Fewer bots came from ending the offline windows of
test guilds before a restart (see the crash below).

| Step | Bots | Tick mean / median / p95 / p99 / max (ms) | CPU (cores) | RAM | Planner (`.ga perf`) |
|---|---|---|---|---|---|
| live config | 800 | 12 / 11 / 21 / 24 / 28 | 1.27 | 5.3 GB | 98 ms of every second |
| live config | 1200 | 14 / 11 / 27 / 30 / 35 | 1.54 | 5.9 GB | 131 ms |
| live config | 1600 | 16 / 12 / 30 / 34 / 38 | 1.65 | 5.9 GB | 172 ms |
| `Bots.GridActivationRange = 60` | 1600 | 21 / 19 / 37 / 42 / 48 | 1.60 | 5.9 GB | 166 ms |
| `MapUpdate.Threads = 1` | 800 | 26 / 19 / 61 / 71 / 84 | 0.89 | 5.3 GB | 95 ms |

Against the first capacity test (the design log §0 "Server capacity"): 1600 bots with four map threads went from
30 / 20 / 81 / 352 / 488 ms to 16 / 12 / 30 / 34 / 38 ms, 800 bots with one map thread from p99 146 to 71 ms. PD2's
target (1600 bots at p95 < 50, p99 < 100 ms) holds with room. The median of 11-12 ms is the floor of
`MinWorldUpdateTime = 10`. CP-12 at 60 yd is slower again (as in §10): keep it at 0. MySQL stayed below 0.1 cores.
More than 1600 bots were not tested (the test made 40 guilds).

**A crash found on the way.** `.ga admin logout` of 400 bots at once (ten test guilds, Alliance and Horde mixed) crashed
the worldserver (ACCESS_VIOLATION): `Object::HasFlag` <- `OrderService::StopLeftoverAutoShot` (`OrderServiceRest.cpp`,
`spell->m_targets.GetUnitTarget()` then `target->IsInWorld()`) <- `UpdateBot` <- `Step` <- `UpdateSpread` <- `Update` on
the world thread. A hunter bot's Auto Shot target pointer dangles once its target has left the world (most likely a bot
of the other side it was shooting); `Spell::m_targets` refreshes the pointer only inside `Spell::update` on the map
thread. The fix belongs to the module: resolve the target by its GUID (`GetUnitTargetGUID()` and
`ObjectAccessor::GetUnit`). Reported to the maintainers with the dump.

## 12. Beyond 1600 bots: an estimate (2026-11-24)

the owner on PD2 (2026-11-24): [quote left out]. **Nothing above 1600 bots has been measured.** This section extrapolates §10 and §11
(T0, an estimate of a model) and names the test that replaces it (PF-10).

### 12.1 What a tick is made of after the optimisations

Since PF-1 to PF-6 and CP-11 almost everything a bot costs follows the clock instead of the tick:

| Work | Runs on | At 1600 bots | Per bot |
|---|---|---|---|
| The planner | world thread | 172 ms of every second (§11; 98 / 131 / 172 ms at 800 / 1200 / 1600 bots: linear) | 0.11 ms a second |
| Bot AI passes, a bot's `Player::Update` every 75 ms (CP-11), our per-update hook | the map task of the bot's continent | 503 ms of every second over all maps (§10.4: 61 + 222 + 203 + 17) | 0.31 ms a second; the busiest continent held half the bots (809 of 1600 in §11) |
| Creatures, their sight checks, visibility | the map task, on its full step only (every third tick) | 981 ms a second at 49 ticks a second: about 60 ms per full step over all maps, about 30 ms on the busiest continent | follows the creatures the bots keep awake |

On the critical path (world thread plus the busiest continent) the clock-driven share of every second is
`F` = 0.17 + 0.21, about 0.4 at 1600 bots with every other server stopped (0.46 in §10's runs beside other servers).
The per-cycle work `c` is the busiest continent's creature update, about 21 ms. That reproduces §11's 1600-bot row:
two ticks at the 10 ms floor of `MinWorldUpdateTime` (median 12 ms) and one of about 30 ms (p95), mean 16 ms. About
7 ms of every 48 ms cycle is sleep: at 1600 bots the server uses about 85 % of what its critical path can carry.

### 12.2 Extrapolation

Assumptions: `F` and `c` grow linearly with the bots (the planner did, and the p95 tick went 21 / 27 / 30 ms), the bots
spread as in the tests (half on the busiest continent, levels 10-60, order `AUTO`), the live config, this PC. Once the
short ticks leave the 10 ms floor (about 1900 bots) a cycle of three ticks lasts `c / (1 - F)` and its longest tick
`c / (1 - F^3)`.

| Bots (guilds of 40) | `F` | Mean tick | Longest tick of three (about the p95) | Reading |
|---|---|---|---|---|
| 1600 (40), measured | 0.40 | 16 ms | 30 ms | smooth |
| 2000 (50) | 0.50 | about 19 ms | about 31 ms | smooth |
| 2400 (60) | 0.60 | about 26 ms | about 40 ms | smooth, near PD2's p95 line of 50 ms |
| 2800 (70) | 0.70 | about 41 ms | about 56 ms | over the p95 line |
| 3200 (80) | 0.80 | about 70 ms | about 86 ms | lag that players notice |
| 3600 (90) | 0.90 | about 160 ms | about 175 ms | heavy lag |
| 4000 (100) | 1.0 | | | the clock-driven work no longer fits into a second |

The server does not fall over at `F` = 1, it slows its bots: with ticks above 75 ms a bot is updated once per tick
(CP-11's interval is a minimum), its AI passes once per update, and the planner's budget lets steps run late. From
about 100 guilds on a tick is then the whole per-tick cost: about 0.024 ms per bot of the busiest continent for its
update and AI pass, the creatures on every third tick, and the planner's share of the world thread. That gives ticks
of roughly 100 to 200 ms at 4000 bots. The planner alone needs 0.9 of the one world thread at 8000 bots (200 guilds)
and 2.2 at 20000 (500 guilds): there ticks last several hundred milliseconds to seconds and bots are planned every few
seconds, which is no longer playable for a human on the same server.

How far off this can be: about 30 % either way. For more bots: a continent has a fixed number of creatures, so their
cost stops growing once the levelling zones are awake everywhere. For fewer: more bots per zone mean more sight checks
per creature and more fights over the same creatures, and the shares of §10.4 were taken beside other servers.

Memory is not the limit before the CPU: 5.9 GB at 1600 bots (§11), at most 1.6 MB a bot above the 3.6 GB base, so
about 10 GB at 4000, 16 GB at 8000 and 35 GB at 20000 bots (the PC has 61 GB). MySQL stayed below 0.1 cores.

**So, on this PC with today's build (estimate): smooth up to about 60 guilds in the world at once, playable with lag
up to about 80, not beyond 100.** "Guilds" means guilds whose bots are in the world at the same time (the owner online
or inside the offline window of 1 to 8 hours), not registered guilds.

### 12.3 What sets the limit, and what would move it

The limit is single-threaded: the world thread (sessions, the planner) and one task per continent. Of the PC's 32
logical cores the worldserver used 1.65 at 1600 bots. More map threads, more cores or a faster database do not move it.

| Lever | What | Expected | Cost and risk |
|---|---|---|---|
| PF-10, measure first | a third stress test with every other server stopped: 60 / 80 / 100 / 120 test guilds (2400 to 4800 bots), `.server info`, `.ga perf` and a `-Waits` trace per stage; two to three hours | replaces the table of §12.2 by numbers, shows whether the creatures saturate and which thread gives out first | the harness exists (`tools/loadtest`); the live server is down meanwhile |
| L1, bots nobody watches think less often | for bots with no client in sight: `Bots.UpdateInterval` 150 instead of 75, a longer react delay, a planner step every 2 s | `F` about halves: about twice the bots (100 to 120 guilds smooth) | a behaviour change: such bots react later in fights (§7 E2 halved the AI's rate for all bots and was not judged for behaviour). The owner's decision |
| L2, PF-8: route searches off the world thread | the planner's searches on worker threads | the world thread's own ceiling moves from about 9000 to about 14000 bots; pays only together with L3 | large, parked since the plan |
| L3, a continent in several tasks | regions of a continent updated in parallel (a bot deals with its own zone) | the only lever that uses the idle cores: three to six times the bots | a deep core patch, thread safety at region borders, high risk, hard to keep through upstream merges |
| L4, several realms | one worldserver process per 60 to 80 guilds, each with its own world and character database, one account database; a guild lives on one realm | linear: about four to five processes on this PC (three busy threads and 8 to 10 GB each, untested) = 250 to 350 guilds, more machines beyond that | no engine risk; players of different realms do not meet; operations work (realm list, databases and configs per realm, the tools' instance handling) |
| L5, a CPU with faster single cores | | linear in single-thread speed | hardware |

For the owner's numbers: 100 guilds at once need L1 or two realms; 200 need three realms (or L1 with L3); 500 at once need
about eight realms (some 24 busy threads and 60 GB of RAM: more than this PC) or L1, L2 and L3 together. How
many of the registered guilds are in the world at once is a game rule (the offline window, M9's server-wide cap on
offline bots), not a technical lever.
