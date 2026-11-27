#!/usr/bin/env python3
"""Generates modules/mod-guild-adventure/data/sql/db-world/base/ga_road_link.sql: walking links the travel node graph of
mod-playerbots lacks, found on the server's navmesh (docs/orders.md "Road links from the navmesh").

mod-playerbots' graph (playerbots_travelnode, _link) has parts that no walking link joins to the rest of their map:
the start mesa of the Tauren (Camp Narache, Red Cloud Mesa) has links among its own seven nodes and none down to the
plains, so the road map has no route off it, and the worldserver's path finder cannot see the way down either (the
plains' grid is not loaded while a recruit stands on the mesa). For each such part this script asks the navmesh
(tools/navpath, the core's Detour with the filter the core gives bots) for walks between its nodes and the nodes of
the other parts of its map, and keeps the shortest walk that is not much longer than the straight line, one per pair
of parts, until no part can be joined any more.

    python tools/gen_road_links.py [--cnf _work\\mysql-client.cnf] [--db GA_playerbots] [--data _work\\server\\Data]
                                   [--navpath _work\\tools-bin\\navpath.exe]

Build tools/navpath first (tools\\navpath\\build.ps1). Ids are assigned anew on every run (the table is not referenced
by anything else). Reload: a restart of the worldserver (the road map is read at startup and with ".ga reload").
"""

import argparse
import math
import os
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'modules', 'mod-guild-adventure', 'data', 'sql', 'db-world', 'base', 'ga_road_link.sql')
MYSQL = r'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe'
OPEN_WORLD = (0, 1, 530, 571)

# Pairs of nodes asked per part (the nearest first), and how far apart they may be as the crow flies.
CANDIDATES = 16
MAX_STRAIGHT = 1800.0
# A walk is kept when it is at most this many times the straight line and not longer than MAX_WALK yards (a way over
# the mountains around a cliff is no road).
MAX_DETOUR = 3.0
MAX_WALK = 2600.0
# A walked path that rises or falls JUMP_YARDS or more within a short stretch, and the navmesh has no walk between the
# two points of at most JUMP_WALK times their distance (JUMP_WALK_MIN yards at least): the link is left out. The walk
# between its two nodes takes its place when it is at most REPAIR_DETOUR times their distance (400 yd more at least).
JUMP_YARDS = 10.0
JUMP_WALK = 4.0
JUMP_WALK_MIN = 200.0
REPAIR_DETOUR = 4.0


def query(args, sql):
    out = subprocess.run([MYSQL, f'--defaults-extra-file={args.cnf}', '-N', '-e', sql], capture_output=True, text=True,
                         check=True).stdout
    return [line.split('\t') for line in out.splitlines() if line]


def blocked(name):
    # RoadMap::Load: nodes at instance portals and teleporters are no places to walk over.
    return name.endswith('entrance') or name.endswith('exit') or 'portal' in name


class Parts:
    def __init__(self):
        self.parent = {}

    def find(self, a):
        self.parent.setdefault(a, a)
        while self.parent[a] != a:
            self.parent[a] = self.parent[self.parent[a]]
            a = self.parent[a]
        return a

    def join(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.parent[ra] = rb


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cnf', default=os.path.join(ROOT, '_work', 'mysql-client.cnf'))
    parser.add_argument('--db', default='GA_playerbots')
    parser.add_argument('--data', default=os.path.join(ROOT, '_work', 'server', 'Data'))
    parser.add_argument('--navpath', default=os.path.join(ROOT, '_work', 'tools-bin', 'navpath.exe'))
    args = parser.parse_args()

    nodes = {}
    for nid, name, map_id, x, y, z in query(args, f'SELECT id, name, map_id, x, y, z FROM {args.db}.playerbots_travelnode'):
        if int(map_id) in OPEN_WORLD and not blocked(name):
            nodes[int(nid)] = (name, int(map_id), float(x), float(y), float(z))

    walkers = {}

    def walk_points(map_id, a, b):
        if map_id not in walkers:
            walkers[map_id] = subprocess.Popen([args.navpath, args.data, str(map_id)], stdin=subprocess.PIPE,
                                               stdout=subprocess.PIPE, text=True)
        proc = walkers[map_id]
        proc.stdin.write(f'{a[0]} {a[1]} {a[2]} {b[0]} {b[1]} {b[2]}\n')
        proc.stdin.flush()
        answer = proc.stdout.readline().strip()
        if not answer.startswith('OK '):
            return None
        _, length, _, points = answer.split(' ', 3)
        return float(length), points

    def walk(map_id, a, b):
        return walk_points(map_id, nodes[a][2:5], nodes[b][2:5])

    # Walking links only, as RoadMap::Load reads them (its routes ride no lifts and use no other links; a part joined
    # to the rest by a lift alone, Thunder Bluff on its mesa, gets no walk from the navmesh either).
    links = set()
    for a, b in query(args, f'SELECT node_id, to_node_id FROM {args.db}.playerbots_travelnode_link WHERE type = 1'):
        a, b = int(a), int(b)
        if a in nodes and b in nodes and nodes[a][1] == nodes[b][1]:
            links.add((a, b))
    # Links whose walked path jumps up or down (a height step of JUMP_YARDS or more within a short stretch) where the
    # navmesh has no walk of reasonable length: mod-playerbots recorded the walk from Cold Hearth Manor in Tirisfal to
    # Lady Sylvanas in the Undercity's Royal Quarter on the surface above it, its last point 90 yd below the one before;
    # the road map routed over it, and a recruit's hand-in at the Undercity's warlock trainer ended "stuck" at once.
    # Such a link is dropped (ga_road_unlink); the walk between its two nodes, when the navmesh has one, is a link of
    # its own (ga_road_link). Not links to a ship, zeppelin or portal (their nodes stand on decks and platforms).
    transportish = set()
    for a, b in query(args, f'SELECT node_id, to_node_id FROM {args.db}.playerbots_travelnode_link WHERE type IN (2, 3)'):
        transportish.update((int(a), int(b)))
    paths = defaultdict(list)
    for a, b, x, y, z in query(args, f'SELECT node_id, to_node_id, x, y, z FROM {args.db}.playerbots_travelnode_path '
                                     'ORDER BY node_id, to_node_id, nr'):
        key = (int(a), int(b))
        if key in links:
            paths[key].append((float(x), float(y), float(z)))
    unlinked = []  # (from node, to node)
    found = []  # (map, from node, to node, walk yards, straight yards, points)
    repaired = set()
    for key in sorted(links):
        a, b = key
        if a in transportish or b in transportish:
            continue
        points = paths.get(key, [])
        for i in range(1, len(points)):
            p, q = points[i - 1], points[i]
            step = math.hypot(q[0] - p[0], q[1] - p[1])
            rise = abs(q[2] - p[2])
            if rise < JUMP_YARDS or rise <= step * 1.5:
                continue
            gap = math.dist(p, q)
            result = walk_points(nodes[a][1], p, q)
            if result and result[0] <= max(gap * JUMP_WALK, JUMP_WALK_MIN):
                break
            unlinked.append(key)
            pair = (min(a, b), max(a, b))
            if pair not in repaired:
                repaired.add(pair)
                straight = math.dist(nodes[a][2:5], nodes[b][2:5])
                whole = walk(nodes[a][1], a, b)
                if whole and whole[0] <= min(MAX_WALK, max(straight * REPAIR_DETOUR, straight + 400.0)):
                    found.append((nodes[a][1], a, b, whole[0], straight, whole[1]))
                    print(f'map {nodes[a][1]}: repaired {nodes[a][0]} -> {nodes[b][0]}: {whole[0]:.0f} yd walked', file=sys.stderr)
            break
    links -= set(unlinked)

    parts = Parts()
    linked = set()
    for a, b in links:
        parts.join(a, b)
        linked.update((a, b))
    for map_id, a, b, *_ in found:
        parts.join(a, b)
        linked.update((a, b))
    repairs = len(found)
    for map_id in OPEN_WORLD:
        while True:
            groups = defaultdict(list)
            for n in linked:
                if nodes[n][1] == map_id:
                    groups[parts.find(n)].append(n)
            if len(groups) < 2:
                break
            joined = False
            # The smallest parts first: they are the ones cut off.
            for root, members in sorted(groups.items(), key=lambda g: len(g[1])):
                pairs = []
                for a in members:
                    _, _, ax, ay, _ = nodes[a]
                    for other, others in groups.items():
                        if other == root:
                            continue
                        for b in others:
                            d = math.hypot(nodes[b][2] - ax, nodes[b][3] - ay)
                            if d <= MAX_STRAIGHT:
                                pairs.append((d, a, b))
                pairs.sort()
                best = None
                tried_parts = defaultdict(int)
                asked = 0
                for d, a, b in pairs:
                    target = parts.find(b)
                    if asked >= CANDIDATES or tried_parts[target] >= 6:
                        continue
                    tried_parts[target] += 1
                    asked += 1
                    result = walk(map_id, a, b)
                    if not result:
                        continue
                    length, points = result
                    if length > MAX_WALK or length > max(d * MAX_DETOUR, d + 150.0):
                        continue
                    if not best or length < best[3]:
                        best = (map_id, a, b, length, d, points)
                if best:
                    found.append(best)
                    parts.join(best[1], best[2])
                    print(f'map {map_id}: {nodes[best[1]][0]} -> {nodes[best[2]][0]}: {best[3]:.0f} yd walked, {best[4]:.0f} '
                          f'straight ({len(members)} nodes joined)', file=sys.stderr)
                    joined = True
                    break
            if not joined:
                break
    for proc in walkers.values():
        proc.stdin.close()
        proc.wait()

    def esc(text):
        return text.replace('\\', '\\\\').replace("'", "\\'")

    with open(OUT, 'w', encoding='utf-8', newline='\n') as out:
        out.write('-- mod-guild-adventure: walking links the travel node graph of mod-playerbots lacks (GA_world). GENERATED by\n'
                  '-- tools/gen_road_links.py from the server\'s navmesh (tools/navpath); run the script again, do not edit.\n'
                  '-- Applied by the worldserver\'s DB updater (re-applied when the file changes); idempotent. RoadMap::Load\n'
                  '-- reads it at startup (".ga reload" too) as two walking links each. Docs: docs/orders.md\n'
                  '-- "Road links from the navmesh".\n'
                  '-- points: "x y z" a few yards apart, from `from_node` to `to_node` (playerbots_travelnode ids).\n')
        out.write('CREATE TABLE IF NOT EXISTS `ga_road_link` (\n'
                  '    `id`        INT UNSIGNED      NOT NULL,\n'
                  '    `map_id`    SMALLINT UNSIGNED NOT NULL,\n'
                  '    `from_node` INT UNSIGNED      NOT NULL COMMENT \'playerbots_travelnode.id\',\n'
                  '    `to_node`   INT UNSIGNED      NOT NULL COMMENT \'playerbots_travelnode.id\',\n'
                  '    `distance`  FLOAT             NOT NULL COMMENT \'yards walked\',\n'
                  '    `points`    MEDIUMTEXT        NOT NULL,\n'
                  '    `comment`   VARCHAR(255)      NOT NULL DEFAULT \'\',\n'
                  '    PRIMARY KEY (`id`)\n'
                  ') ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT=\'Guild Adventure: road links from the navmesh\';\n\n'
                  'DELETE FROM `ga_road_link`;\n')
        if found:
            out.write('INSERT INTO `ga_road_link` (`id`, `map_id`, `from_node`, `to_node`, `distance`, `points`, `comment`) VALUES\n')
            rows = []
            for i, (map_id, a, b, length, d, points) in enumerate(found, 1):
                comment = f'{nodes[a][0]} -> {nodes[b][0]} ({d:.0f} yd straight)'
                rows.append(f"({i}, {map_id}, {a}, {b}, {length:.1f}, '{points}', '{esc(comment)}')")
            out.write(',\n'.join(rows) + ';\n')
        out.write('\n-- Links of the travel node graph RoadMap::Load leaves out: their walked path jumps up or down where the\n'
                  '-- navmesh has no walk of reasonable length (the walk between their nodes, when there is one, is above).\n'
                  'CREATE TABLE IF NOT EXISTS `ga_road_unlink` (\n'
                  '    `from_node` INT UNSIGNED      NOT NULL COMMENT \'playerbots_travelnode.id\',\n'
                  '    `to_node`   INT UNSIGNED      NOT NULL COMMENT \'playerbots_travelnode.id\',\n'
                  '    `comment`   VARCHAR(255)      NOT NULL DEFAULT \'\',\n'
                  '    PRIMARY KEY (`from_node`, `to_node`)\n'
                  ') ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT=\'Guild Adventure: travel node links left out\';\n\n'
                  'DELETE FROM `ga_road_unlink`;\n')
        if unlinked:
            out.write('INSERT INTO `ga_road_unlink` (`from_node`, `to_node`, `comment`) VALUES\n')
            out.write(',\n'.join(f"({a}, {b}, '{esc(nodes[a][0] + ' -> ' + nodes[b][0])}')" for a, b in unlinked) + ';\n')
    print(f'{repairs} repaired walks, {len(found) - repairs} bridges between parts, {len(unlinked)} links left out; written to {OUT}',
          file=sys.stderr)


if __name__ == '__main__':
    main()
