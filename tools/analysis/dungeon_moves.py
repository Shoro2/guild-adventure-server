#!/usr/bin/env python3
"""Counts how often a dungeon group is moved by the module (a teleport) instead of walking, per dungeon and kind.

Reads the activity log (logs/GuildAdventure.log) and counts the lines the dungeon runs write when they put a group or a
member somewhere: a boss or a creature with no way on foot, a group put on a boss's floor or at the place it clears a
room from, a member put beside its tank or caught up with its squad leader, a group put beside an event's NPC or an
object, a group that stands apart as its run begins. The entry teleport of a run (into the dungeon from wherever its
bots are) is by design and not counted. The owner, 2026-11-24: the bots move through a dungeon like real players and do
not port; this is the measure of that.

Usage: python dungeon_moves.py <GuildAdventure.log> [--since "YYYY-MM-DD HH:MM:SS"] [--who <name>,<name>] [--lines]
"""
import argparse
import collections
import re
import sys

KINDS = [
    ("boss hop", re.compile(r"'s group finds no way on foot to (?P<what>.+?) in (?P<dungeon>.+?) \(\d+ yd away; a way players swim or jump\)")),
    ("clear hop", re.compile(r"'s group finds no way on foot to (?P<what>.+?) \(level \d+, \d+ yd away\) in (?P<dungeon>.+?) while")),
    ("floor hop", re.compile(r"'s group stands \d+ yd (?:above|below) (?P<what>.+?) in (?P<dungeon>.+?) with no way on foot to it: it is put on its floor")),
    ("room place hop", re.compile(r"'s group stands where no way on foot leads to the place it clears the room of (?P<what>.+?) from in (?P<dungeon>.+?) \(")),
    ("member to tank", re.compile(r"activity: (?P<what>\S+) did not come to its tank \S+ (?:before .+? )?in (?P<dungeon>.+?) (?:for|in) \d+ s")),
    ("member catch-up", re.compile(r"activity: (?P<what>\S+) caught up with its squad leader \S+ in (?P<dungeon>.+?) \(.*with no way on foot")),
    ("event NPC", re.compile(r"'s group fell \d+ yd behind (?P<what>.+?) in (?P<dungeon>.+?): it is put beside it")),
    ("object", re.compile(r"'s group gets no nearer to (?:object )?(?P<what>.+?) in (?P<dungeon>.+?) in a minute")),
    ("apart at start", re.compile(r"'s group stands apart inside (?P<dungeon>.+?) as its run begins: (?P<what>\d+) put")),
]
STAMP = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})")
RUN_END = re.compile(r"activity: (\S+) ends its dungeon run in (.+?) after")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--since", default="")
    parser.add_argument("--who", default="", help="only lines that name one of these characters")
    parser.add_argument("--lines", action="store_true", help="print every counted line")
    args = parser.parse_args()
    who = [w for w in args.who.split(",") if w]
    counts = collections.Counter()
    per_dungeon = collections.defaultdict(collections.Counter)
    runs = collections.Counter()
    with open(args.log, encoding="utf-8", errors="replace") as log:
        for line in log:
            stamp = STAMP.match(line)
            if not stamp or stamp.group(1) < args.since:
                continue
            if who and not any(w in line for w in who):
                continue
            end = RUN_END.search(line)
            if end:
                runs[end.group(2)] += 1
                continue
            for kind, pattern in KINDS:
                m = pattern.search(line)
                if m:
                    counts[kind] += 1
                    per_dungeon[m.group("dungeon")][kind] += 1
                    if args.lines:
                        print(line.rstrip()[:260])
                    break
    names = [k for k, _ in KINDS]
    print(f"{'dungeon':34} {'runs':>4} " + " ".join(f"{n[:10]:>10}" for n in names) + "  all")
    for dungeon in sorted(set(per_dungeon) | set(runs)):
        row = per_dungeon[dungeon]
        print(f"{dungeon[:34]:34} {runs[dungeon]:>4} " + " ".join(f"{row[n]:>10}" for n in names) + f"  {sum(row.values())}")
    print(f"{'all':34} {sum(runs.values()):>4} " + " ".join(f"{counts[n]:>10}" for n in names) + f"  {sum(counts.values())}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
