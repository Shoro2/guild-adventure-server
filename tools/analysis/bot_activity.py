#!/usr/bin/env python3
"""How active the guild's bots are: standing around, walking to and fro, tasks begun again and again, targets switched.

Reads the activity log (logs/GuildAdventure.log) and sums per bot over a time window:

- the five-minute status lines: windows in which the bot got less than 30 yd from where it stood five minutes before
  with no fight and no rest in them ("idle windows"), its experience per hour; and, from the status lines that carry
  them (OrderService::TrackActivity, 2026-11-25), the seconds it stood still, the seconds of that with a walk to do,
  the yards it walked against the yards it got on (a ratio far above 1 is a walk to and fro), its turns back, the
  tasks it began and how many of them ended within 10 s, and its target switches in fights;
- the lines of walks that did not work: "is stuck", "does not get on with its walk", refused ways ("does not walk"),
  keep-aways, rescues, hearthstones, tasks that ended within 2 s;
- quests handed in and deaths.

the owner, 2026-11-25: [quote left out].

Usage: python bot_activity.py <GuildAdventure.log> [--since "YYYY-MM-DD HH:MM:SS"] [--until ...] [--who a,b]
       [--sort idle|stood|walkratio|quick|stuck|xph] [--lines <name>]
"""
import argparse
import collections
import re

STAMP = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}) activity: (\S+) (.*)$")
STATUS = re.compile(
    r"status: level (\d+) .*? at \((-?\d+), (-?\d+)\) map (\d+), moved (?P<moved>[^,]+?) in \d+ min, .*?xp/h (?P<xph>\d+), "
    r"(?P<fights>\d+) fights .*?(?P<rests>\d+) rests \((?P<restsec>\d+) s\), (?P<deaths>\d+) deaths")
ACTIVITY = re.compile(
    r"stood still (?P<stood>\d+) s \((?P<stoodwalk>\d+) s with a walk to do\), walked (?P<walked>\d+) yd, (?P<turns>\d+) turns back, "
    r"(?P<tasks>\d+) tasks begun \((?P<quick>\d+) ended within 10 s\), (?P<switches>\d+) target switches")
ENDS = re.compile(r"^ends \S+(?: quest \d+)? after (\d+) s")
COUNTED = [
    ("stuck", re.compile(r"^is stuck at ")),
    ("no headway", re.compile(r"^does not get on with its walk")),
    ("refused way", re.compile(r"^does not walk ")),
    ("keep away", re.compile(r"^keeps away from ")),
    ("rescue", re.compile(r"^(?:is (?:under the ground|cut off from every road|stranded)|has its movements cancelled).*(?:put back|rescued)|.*rescued")),
    ("hearthstone", re.compile(r"^uses its hearthstone")),
    ("turned in", re.compile(r"^ends turn-in .*: turned in")),
    ("death", re.compile(r"^died at level")),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--since", default="")
    parser.add_argument("--until", default="")
    parser.add_argument("--who", default="", help="only these characters (comma separated)")
    parser.add_argument("--sort", default="idle", choices=["idle", "stood", "walkratio", "quick", "stuck", "xph"])
    parser.add_argument("--lines", default="", help="print the lines that count against this character")
    args = parser.parse_args()
    who = {w for w in args.who.split(",") if w}

    bots = collections.defaultdict(lambda: collections.Counter())
    xph = collections.defaultdict(list)
    first = last = None
    with open(args.log, encoding="utf-8", errors="replace") as log:
        for line in log:
            m = STAMP.match(line)
            if not m:
                continue
            stamp, name, rest = m.group(1), m.group(2), m.group(3)
            if args.since and stamp < args.since or args.until and stamp > args.until:
                continue
            if who and name not in who:
                continue
            b = bots[name]
            first = first or stamp
            last = stamp
            s = STATUS.search(rest)
            if s:
                b["windows"] += 1
                xph[name].append(int(s.group("xph")))
                moved = s.group("moved")
                fights, rests = int(s.group("fights")), int(s.group("rests"))
                if moved.endswith(" yd") and float(moved[:-3]) < 30 and fights == 0 and rests == 0:
                    b["idle windows"] += 1
                    if args.lines == name:
                        print(line.rstrip()[:260])
                a = ACTIVITY.search(rest)
                if a:
                    b["tracked"] += 1
                    for key in ("stood", "stoodwalk", "walked", "turns", "tasks", "quick", "switches"):
                        b[key] += int(a.group(key))
                    if moved.endswith(" yd"):
                        b["net"] += float(moved[:-3])
                continue
            e = ENDS.match(rest)
            if e:
                b["task ends"] += 1
                if int(e.group(1)) <= 2:
                    b["ended at once"] += 1
            for key, pattern in COUNTED:
                if pattern.search(rest):
                    b[key] += 1
                    if args.lines == name and key in ("stuck", "no headway", "refused way", "rescue"):
                        print(line.rstrip()[:260])
                    break

    if not bots:
        print("no lines in the window")
        return
    print(f"window {first} .. {last}")
    rows = []
    for name, b in bots.items():
        if not b["windows"]:
            continue
        ratio = b["walked"] / b["net"] if b["tracked"] and b["net"] > 0 else 0.0
        rows.append((name, b, ratio, sum(xph[name]) / len(xph[name]) if xph[name] else 0))
    keyfn = {
        "idle": lambda r: r[1]["idle windows"] / r[1]["windows"],
        "stood": lambda r: r[1]["stoodwalk"],
        "walkratio": lambda r: r[2],
        "quick": lambda r: r[1]["ended at once"],
        "stuck": lambda r: r[1]["stuck"] + r[1]["no headway"],
        "xph": lambda r: -r[3],
    }[args.sort]
    rows.sort(key=keyfn, reverse=True)
    header = (f"{'bot':12} {'5-min':>5} {'idle':>4} {'xp/h':>6} {'stood s':>7} {'walk s':>6} {'walked':>7} {'ratio':>5} "
              f"{'turns':>5} {'tasks':>5} {'<10s':>4} {'switch':>6} {'ends':>4} {'<=2s':>4} {'stuck':>5} {'nohead':>6} "
              f"{'refuse':>6} {'keep':>4} {'resc':>4} {'hearth':>6} {'quests':>6} {'deaths':>6}")
    print(header)
    total = collections.Counter()
    for name, b, ratio, avgxph in rows:
        total.update(b)
        tracked = b["tracked"] > 0
        print(f"{name:12} {b['windows']:>5} {b['idle windows']:>4} {avgxph:>6.0f} "
              f"{(str(b['stood']) if tracked else '-'):>7} {(str(b['stoodwalk']) if tracked else '-'):>6} "
              f"{(str(b['walked']) if tracked else '-'):>7} {(f'{ratio:.1f}' if tracked else '-'):>5} "
              f"{(str(b['turns']) if tracked else '-'):>5} {(str(b['tasks']) if tracked else '-'):>5} "
              f"{(str(b['quick']) if tracked else '-'):>4} {(str(b['switches']) if tracked else '-'):>6} "
              f"{b['task ends']:>4} {b['ended at once']:>4} {b['stuck']:>5} {b['no headway']:>6} {b['refused way']:>6} "
              f"{b['keep away']:>4} {b['rescue']:>4} {b['hearthstone']:>6} {b['turned in']:>6} {b['death']:>6}")
    windows = total["windows"] or 1
    print(f"\nall: {total['windows']} five-minute windows, {total['idle windows']} idle ({100 * total['idle windows'] / windows:.0f} %), "
          f"{total['task ends']} task ends ({total['ended at once']} within 2 s), {total['stuck']} stuck, {total['no headway']} "
          f"walks without headway, {total['refused way']} refused ways, {total['keep away']} keep-aways, {total['rescue']} rescues, "
          f"{total['hearthstone']} hearthstones, {total['turned in']} quests handed in, {total['death']} deaths")
    if total["tracked"]:
        print(f"tracked windows: {total['tracked']}: stood still {total['stood']} s ({total['stoodwalk']} s with a walk to do), "
              f"walked {total['walked']} yd for a net {total['net']:.0f} yd, {total['turns']} turns back, {total['tasks']} tasks begun "
              f"({total['quick']} ended within 10 s), {total['switches']} target switches")


if __name__ == "__main__":
    main()
