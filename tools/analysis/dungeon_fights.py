"""Per-run metrics of a dungeon group from the activity log (logs/GuildAdventure.log).

Input: one file per run with the lines of the group's characters, date and "activity:" stripped
("HH:MM:SS <text>"), for example:

    sed -n '<first>,<last>p' GuildAdventure.log | grep -E "Wartank|Duduheal|..." | grep -v "guild bank of"         | sed -e 's/^[0-9-]* //' -e 's/ activity: / /' > run.txt
    python dungeon_fights.py run.txt <tank> <healer>

Output: who is in a fight first, each member's fight start against the tank's (a fight line is written at the end
of a fight with its length), the bosses with each member's arrival, rests, the leader's waits, deaths and the
"goes to help" distances. Read-only; it knows nothing of the server.
"""
import re
import sys
from collections import defaultdict

FIGHT = re.compile(
    r"^(\d\d):(\d\d):(\d\d) (\w+) fight (\d+) s(?: [\w ]+)?: (\d+) foes? at once(?: \((.+?) level (\d+)\))?, "
    r"health (\d+)% -> (\d+)% \(lowest (\d+)%\), mana (\d+)% -> (\d+)%, (\w+)(.*)$")
REST = re.compile(r"^(\d\d):(\d\d):(\d\d) (\w+) rested (\d+) s: health (\d+)% -> (\d+)%, mana (\d+)% -> (\d+)%.*ended by: (.*)$")
WAIT = re.compile(r"^(\d\d):(\d\d):(\d\d) (\w+) waits for its squad: (.*)$")
WAITED = re.compile(r"^(\d\d):(\d\d):(\d\d) (\w+) waited (\d+) s for its squad \((.*)\): goes on")
DONE = re.compile(r"^(\d\d):(\d\d):(\d\d) (\w+)'s group is done with (.+?) in ")
DIED = re.compile(r"^(\d\d):(\d\d):(\d\d) (\w+) died at")
HELP = re.compile(r"^(\d\d):(\d\d):(\d\d) (\w+)'s group goes to help (\w+).*?\((\d+)% health\) against (.+?) \(level \d+, (\d+) yd from")


def sec(m):
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))


def clock(t):
    return "%02d:%02d:%02d" % (t // 3600, t // 60 % 60, t % 60)


def main(path, tank, healer):
    fights = []
    rests = []
    waits = defaultdict(int)
    waited = []
    done = []
    died = []
    helps = []
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if m := FIGHT.match(line):
            end = sec(m)
            dur = int(m.group(5))
            fights.append(dict(who=m.group(4), start=end - dur, end=end, foes=int(m.group(6)), foe=m.group(7) or "-",
                               h0=int(m.group(9)), h1=int(m.group(10)), hmin=int(m.group(11)), m0=int(m.group(12)),
                               m1=int(m.group(13)), result=m.group(14), tail=m.group(15)))
        elif m := REST.match(line):
            end = sec(m)
            rests.append(dict(who=m.group(4), start=end - int(m.group(5)), end=end, dur=int(m.group(5)), m0=int(m.group(8)),
                              m1=int(m.group(9)), by=m.group(10)))
        elif m := WAITED.match(line):
            waited.append((clock(sec(m)), m.group(5), m.group(6)))
        elif m := WAIT.match(line):
            reason = re.sub(r"\(.*\)", "", m.group(5))
            reason = re.sub(r"^\w+ ", "", reason).strip()
            waits[reason] += 1
        elif m := DONE.match(line):
            done.append((sec(m), m.group(5)))
        elif m := DIED.match(line):
            died.append((clock(sec(m)), m.group(4)))
        elif m := HELP.match(line):
            helps.append((m.group(5), int(m.group(8))))

    fights.sort(key=lambda f: f["start"])
    # encounters: fights whose intervals overlap (2 s slack)
    encounters = []
    for f in fights:
        if encounters and f["start"] <= encounters[-1]["end"] + 2:
            encounters[-1]["fights"].append(f)
            encounters[-1]["end"] = max(encounters[-1]["end"], f["end"])
        else:
            encounters.append(dict(start=f["start"], end=f["end"], fights=[f]))

    print(f"== {path}: {len(fights)} fight lines, {len(encounters)} encounters, tank {tank}, healer {healer}")
    members = sorted({f["who"] for f in fights})
    first = defaultdict(int)
    lag = defaultdict(list)
    tank_absent = 0
    tank_late = []
    for e in encounters:
        starts = {}
        for f in e["fights"]:
            starts.setdefault(f["who"], f["start"])
        opener = min(starts, key=starts.get)
        first[opener] += 1
        if tank not in starts:
            tank_absent += 1
        else:
            for who, t in starts.items():
                lag[who].append(t - starts[tank])
            if starts[tank] - e["start"] >= 3:
                tank_late.append((clock(e["start"]), opener, starts[tank] - e["start"],
                                  e["fights"][0]["foe"]))
    print("  who is in a fight first:", dict(first))
    print(f"  encounters without the tank in them: {tank_absent}")
    print(f"  encounters the tank joined 3+ s after a member ({len(tank_late)}):")
    for t in tank_late[:40]:
        print("     ", t)
    for who in members:
        if who == tank or not lag[who]:
            continue
        v = sorted(lag[who])
        late = [x for x in v if x >= 5]
        early = [x for x in v if x <= -3]
        print(f"  {who}: in {len(v)} of the tank's encounters; start vs tank: median {v[len(v) // 2]:+d} s, "
              f"{len(late)} times 5+ s later (max {v[-1]:+d}), {len(early)} times 3+ s earlier (min {v[0]:+d})")

    # mana at fight start
    for who in members:
        m0 = [f["m0"] for f in fights if f["who"] == who]
        if not m0 or max(m0) == min(m0) == 100:
            continue
        low = [x for x in m0 if x < 40]
        print(f"  {who}: mana at fight start: median {sorted(m0)[len(m0) // 2]}%, below 40% in {len(low)} of {len(m0)} fights, "
              f"below 20% in {len([x for x in m0 if x < 20])}")
    # tank's big fights
    big = [f for f in fights if f["who"] == tank and f["foes"] >= 4]
    print(f"  tank fights against 4+ foes: {len(big)}; lowest health in them: {[f['hmin'] for f in big]}")
    lows = [(clock(f["end"]), f["who"], f["hmin"], f["foes"], f["foe"]) for f in fights if f["hmin"] < 40]
    print(f"  fights with lowest health below 40%: {len(lows)}")
    for l in lows[:30]:
        print("     ", l)
    print(f"  rests: {len(rests)}; total {sum(r['dur'] for r in rests)} s;",
          {w: (len([r for r in rests if r['who'] == w]), sum(r['dur'] for r in rests if r['who'] == w)) for w in members})
    print("  rests ended by:", dict((k, len([r for r in rests if r['by'].startswith(k)])) for k in {r['by'][:12] for r in rests}))
    print("  leader waits:", dict(waits))
    print("  waited out:", waited)
    print("  deaths:", died)
    hd = sorted(d for _, d in helps)
    if hd:
        print(f"  rescues logged: {len(hd)}; distance from tank: median {hd[len(hd) // 2]} yd, 20+ yd: {len([d for d in hd if d >= 20])}, "
              f"40+ yd: {len([d for d in hd if d >= 40])}; victims: {dict((v, len([1 for w, _ in helps if w == v])) for v in {w for w, _ in helps})}")
    # bosses: who fought, mana at start
    print("  bosses:")
    for t, name in done:
        bf = [f for f in fights if f["foe"] == name]
        if not bf:
            print(f"     {clock(t)} {name}: no fight line names it")
            continue
        s = min(f["start"] for f in bf)
        row = []
        for who in members:
            mine = [f for f in bf if f["who"] == who]
            resting = [r for r in rests if r["who"] == who and r["start"] <= s <= r["end"]]
            if mine:
                row.append(f"{who} +{mine[0]['start'] - s}s mana {mine[0]['m0']}% hp {mine[0]['h0']}%")
            elif resting:
                row.append(f"{who} RESTING")
            else:
                row.append(f"{who} absent")
        print(f"     {clock(s)} {name}: " + "; ".join(row))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
