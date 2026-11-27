# Load test and profiling harness

Measures how the worldserver behaves with hundreds to thousands of guild bots, and where its CPU
time goes. It is the harness of the capacity test of 2026-11-21 (the design log §0 "Server capacity")
and of the profiling behind the optimisation plan ([docs/performance.md](../../docs/performance.md)).

Everything runs on a **throwaway instance** beside the integration server: copied binaries (no
build), cloned databases, own ports. The owner's characters never log in there, and the integration
server is only read (one `mysqldump` per database).

| File | What it does |
|---|---|
| `setup-instance.ps1` | Copies the integration runtime to `_work\server-<instance>`, clones `GA_*` to `GA_<instance>_*`, re-points the configs, creates N test guilds (owner accounts 1001.., leaders 5001.., offline window running) |
| `load.ps1` | `recruit`, `login`, `boost`, `logout` for ranges of test guilds; `sample` (`.server info` and CPU per thread every 15 s into `samples.tsv`); `cmd`; `where` |
| `profile.ps1` | Records an ETW CPU sampling trace of the machine with `xperf` and dumps it with function names from `worldserver.pdb`; `-Waits` adds the stacks of every context switch |
| `analyze_profile.py` | Turns a dump into a report: threads, owners (core / mod-playerbots / our module), cross-cutting domains, call trees, top functions, the world thread's ticks and what makes the slow ones slow, where threads were blocked |
| `restart-with.ps1` | One step of an A/B comparison: sets config keys, restarts the instance, waits for the bots, lets it settle, samples and (with `-Profile`) records both kinds of trace |
| `teardown-instance.ps1` | Stops the instance, drops its databases, deletes its runtime |
| `loadtest-common.ps1` | Names of the test leaders and bots (letters only, no three equal letters in a row) |

## Recipe

Needs: the integration server built and installed (`_work\server` with `worldserver.pdb`), an
elevated PowerShell for `profile.ps1`, the Windows Performance Toolkit (`xperf.exe`, part of the
Windows SDK), Python 3. Register the instance in `tools\ga-env.ps1` (`$GaInstances`, own ports)
while it exists.

```powershell
$env:GA_INSTANCE = 'perf'
.\tools\loadtest\setup-instance.ps1 -Guilds 40 -MapThreads 4     # about 1 minute
.\tools\start-server.ps1 -WorldOnly                              # no authserver: nobody logs in with a client
.\tools\loadtest\load.ps1 recruit 1 20                           # 800 characters, about 1 minute
.\tools\loadtest\load.ps1 login 1 20                             # they enter the world at level 1
Start-Sleep 60
.\tools\loadtest\load.ps1 boost 1 20                             # random levels 10-60: the autopilot spreads them over the zones
# the quarter of an hour after a boost is travel-heavy; for a steady state restart and settle:
.\tools\loadtest\restart-with.ps1 -Label t4_0800 -Bots 800 -SettleSeconds 300 -SampleSeconds 240
.\tools\loadtest\profile.ps1 -Label t4_0800 -Seconds 60
.\tools\loadtest\profile.ps1 -Label t4_0800_waits -Seconds 30 -Waits
$pid_ = (Get-Process worldserver | Where-Object Path -like '*server-perf*').Id
python tools\loadtest\analyze_profile.py _work\server-perf\loadtest\profiles\t4_0800_waits.dump.csv --pid $pid_ `
    --playerbots-src _work\azerothcore-wotlk\modules\mod-playerbots\src --ga-src modules\mod-guild-adventure\src `
    --out _work\server-perf\loadtest\profiles\t4_0800_waits.report.txt
.\tools\loadtest\teardown-instance.ps1 -All
```

To measure a branch instead of the integration server's binaries: give the instance a build of its own
(`setup-server.ps1` and `build-server.ps1` with `GA_INSTANCE` set install into `_work\server-<instance>`), then
`setup-instance.ps1 -ConfigsOnly` copies only the live configs and keeps those binaries. The planner's own numbers
(time per world tick, route searches, cache hits) come from `load.ps1 cmd ".ga perf"` without a profiler.

A second batch (`recruit 21 40`, `login 21 40`, `boost 21 40`) makes it 1600 bots. After a
restart of the instance the bots come back by themselves (their guilds' offline windows run,
eight hours from `setup-instance.ps1`).

An A/B step: `restart-with.ps1 -Label vis60 -World @{ 'Visibility.Distance.Continents' = '60' }
-Bots 1600 -Profile`; give the old value in the next call to go back.

## Reading the numbers

- **`.server info`** gives the mean, median, p95, p99 and maximum of the last 500 world update
  diffs. With `MinWorldUpdateTime = 1` the world loop never sleeps under load, so a lightly loaded
  server runs hundreds of short ticks a second and burns CPU on them: compare tick times, not CPU.
- **A tick** is the world thread's own work plus the wait for the slowest map
  (`MapMgr::Update` schedules one task per map and waits for all of them). The `ticks` section
  of a `-Waits` report splits every tick that way and shows what ran in the slow ones.
- **A sampling profile shows shares**, not capacity: other servers on the box do not distort
  where the time goes, but tick times measured while they run are not capacity numbers. For
  those, stop every other GA server first (the capacity test's rule).
- The kernel logger samples the whole machine while a window records (a few percent overhead,
  more with `-Waits`). Keep windows at 30-60 s. A dump is 1-4 GB of text; `analyze_profile.py`
  writes a small `.samples.pkl.gz` beside it on its first run, after which the dump can go.
- System DLLs stay unresolved (`ntdll.dll!?`): no symbol server is used. The report's "runtime
  folded in" table charges such samples to the function that called them.
- **Steady state or not:** right after a boost (or a restart of a live server) every bot looks
  for its zone and the planner's route searches are well above the steady state. Say which
  state a number is from.
- mod-playerbots' own monitor gives the AI's time by trigger, action and value:
  `.playerbots pmon toggle`, `.playerbots pmon reset`, wait, `.playerbots pmon stack` (writes
  `pmon_total.json` into the logs folder and prints into `Playerbots.log`), `toggle` again.
  The monitor itself is expensive (the mean tick nearly doubled at 1600 bots): one minute, then
  off.
