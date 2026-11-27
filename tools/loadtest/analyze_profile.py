#!/usr/bin/env python3
"""Where the worldserver's CPU time goes: analyses an xperf dumper CSV made by profile.ps1.

    python analyze_profile.py <label>.dump.csv --pid <worldserver pid>
           [--playerbots-src <core>/modules/mod-playerbots/src] [--min 0.5] [--depth 40]
           [--seconds <recorded seconds>] [--out report.txt]

The first run parses the dump (hundreds of MB) and writes <label>.samples.pkl.gz beside it; later
runs read that file, so the dump can be deleted. Sections of the report:

  threads     CPU per thread with its role (world, map worker, database, network ...)
  owners      who owns the sampled function (core, mod-playerbots, mod-guild-adventure, Detour, system)
              and how much of the time runs under a frame of the bot AI or of our module
  domains     cross-cutting costs: share of samples with a pathfinding / line of sight / grid search /
              ... frame anywhere in the stack
  call trees  per role, pruned at --min percent of the process's samples
  functions   top inclusive and exclusive functions
  ticks       the world thread's ticks (from the order of World::Update's phases), their length,
              what the world thread did in them, and how slow ticks differ from the rest
  waits       with a -Waits trace: where threads were blocked (off-CPU time by the stack they woke up in)
"""
import argparse
import collections
import gzip
import os
import pickle
import re
import statistics
import sys

WORLD_PHASES = [  # callee of World::Update, in the order the core runs them
    ('sessions', 'WorldSessionMgr::UpdateSessions'),
    ('maps', 'MapMgr::Update'),
    ('battlegrounds', 'BattlegroundMgr::Update'),
    ('callbacks', 'World::ProcessQueryCallbacks'),
    ('cli', 'World::ProcessCliCommands'),
    ('scripts', 'ScriptMgr::OnWorldUpdate'),
]

DOMAINS = [
    ('pathfinding (PathGenerator, Detour)', r'^(PathGenerator::|dtNavMesh|dtNavMeshQuery|dtQueryFilter|MMAP::)'),
    ('line of sight / height (vmaps, dynamic tree)', r'^(VMAP::|BIH::|BIHWrap|DynamicMapTree|DynTreeImpl|GameObjectModel|RegularGrid2D)'),
    ('terrain height / liquid (GridTerrainData, Map::GetHeight ...)', r'^(GridTerrainData::|Map::GetHeight|Map::GetGridHeight|Map::GetFullTerrainStatus|Map::GetLiquid|Map::GetWaterOrGroundLevel|WorldObject::GetFloorZ|WorldObject::UpdatePositionData|Unit::ProcessTerrainStatusUpdate)'),
    ('grid searches (Cell::Visit, searchers, notifiers)', r'^(Cell::Visit|Map::Visit|TypeContainerVisitor|VisitorHelper|Acore::\w*(Searcher|Notifier|Check|Deliverer|Worker)|GridNotifiers|Map::VisitNearbyCellsOf)'),
    ('visibility / object updates to clients', r'^(Map::SendObjectUpdates|Object::BuildCreateUpdateBlockForPlayer|Object::BuildValuesUpdateBlockForPlayer|UpdateData::|WorldObject::UpdateObjectVisibility|Player::UpdateVisibilityOf|Player::UpdateObjectVisibility|Acore::VisibleNotifier|Acore::VisibleChangesNotifier|Map::ProcessRelocationNotifies|WorldObject::BuildUpdate|Unit::BuildValuesUpdate)'),
    ('packets (build, send, bot packet handling)', r'^(WorldPacket::|ByteBuffer::|WorldSession::SendPacket|WorldSession::Update|WorldSession::Handle|WorldSocket::|PlayerbotAI::HandleBotOutgoingPacket|PlayerbotAI::HandleMasterIncomingPacket|ExternalEventHelper::|PacketHandlingHelper::)'),
    ('spells and auras', r'^(Spell::|SpellInfo::|Aura::|AuraEffect::|Unit::CalculateSpell|SpellMgr::|Unit::_UpdateSpells|Unit::RemoveAura|Unit::HasAura|Unit::GetAura)'),
    ('creature AI and movement (Creature::Update, SmartAI, MotionMaster)', r'^(Creature::Update|CreatureAI::|SmartAI::|SmartScript::|MotionMaster::|\w*MovementGenerator|Movement::MoveSpline|Unit::UpdateSplineMovement)'),
    ('threat / combat references', r'^(ThreatMgr::|ThreatManager::|HostileRefMgr::|CombatManager::|Unit::IsValidAttackTarget|Unit::IsHostileTo|Unit::GetReactionTo|Unit::IsFriendlyTo)'),
    ('memory (new, delete, malloc, free)', r'^(operator new|operator delete|malloc|free|_malloc_base|_free_base|calloc|realloc|RtlAllocateHeap|RtlFreeHeap|std::_Allocate|std::allocator)'),
    ('locks (mutex, shared_mutex, SRW)', r'^(std::_Mutex_base|std::shared_mutex|std::unique_lock|std::lock_guard|std::shared_lock|_Mtx_lock|_Mtx_unlock|_Smtx_|RtlAcquireSRW|RtlReleaseSRW|AcquireSRWLock|ReleaseSRWLock|std::recursive_mutex)'),
    ('database (statements, queue, MySQL client)', r'^(MySQLConnection::|DatabaseWorkerPool|PreparedStatement|Transaction|mysql_|SQLQueryHolder|QueryCallback|Field::|ResultSet::|PreparedResultSet::)'),
    ('strings and formatting', r'^(std::basic_string|fmt::|std::_String|Acore::StringFormat|std::basic_ostream|std::basic_stringstream|std::to_string|Utf8|strlen|memcmp|std::char_traits)'),
    ('logging', r'^(Log::|Appender|Logger::|LogMessage::)'),
    ('player save', r'^(Player::SaveToDB|Player::_Save|ObjectAccessor::SaveAllPlayers)'),
    ('scripts hook dispatch (ScriptMgr::On..., ExecuteScript)', r'^(ScriptMgr::|ScriptRegistry|ExecuteScript|CALL_ENABLED_HOOKS|IsValidBoolScript)'),
]

WORK_GROUPS = [  # (label, role, frames that must all be in the stack); the first match wins
    ('planner: route search (FindRoute, WalkReaches)', 'world', ['OrderService::Update', 'PathGenerator::CalculatePath']),
    ('planner: route search (FindRoute, WalkReaches)', 'world', ['OrderService::Update', 'RoadMap::FindRoute']),
    ('planner: the rest', 'world', ['OrderService::Update']),
    ('world thread: the rest', 'world', []),
    ('bot AI: reaction pass (10 Hz trigger scan)', 'map worker', ['PlayerbotAI::UpdateAIReaction']),
    ('bot AI: main engine (triggers, actions)', 'map worker', ['PlayerbotAI::UpdateAIInternal']),
    ('bot AI: the rest of UpdateAI', 'map worker', ['PlayerbotAI::UpdateAI']),
    ('our per-tick player hooks (auto loot scan)', 'map worker', ['ScriptMgr::OnPlayerUpdate']),
    ('players: auras (Unit::_UpdateSpells)', 'map worker', ['Player::Update', 'Unit::_UpdateSpells']),
    ('players: movement and position', 'map worker', ['Player::Update', 'Unit::UpdateSplineMovement']),
    ('players: the rest of Player::Update', 'map worker', ['Player::Update']),
    ('creatures: sight checks after moves (AIRelocationNotifier)', 'map worker', ['Map::UpdateNonPlayerObjects', 'Unit::ExecuteDelayedUnitAINotifyEvent']),
    ('creatures, pets, game objects: the rest', 'map worker', ['Map::UpdateNonPlayerObjects']),
    ('visibility after moves (HandleDelayedVisibility)', 'map worker', ['Map::HandleDelayedVisibility']),
    ('sessions (bot packets)', 'map worker', ['WorldSession::Update']),
    ('map workers: the rest', 'map worker', []),
]

SYSTEM_PREFIX = re.compile(r'^([\w.\-]+\.(dll|sys|exe))!', re.I)
IDENT = re.compile(r'[A-Za-z_]\w*')


def clean(name):
    """worldserver.exe!A::b -> A::b ; ntdll.dll!0x7ff.. -> ntdll.dll!?"""
    name = name.strip()
    if name.startswith('worldserver.exe!'):
        name = name[len('worldserver.exe!'):]
        if name.startswith('0x'):
            return 'worldserver.exe!?'
        return name
    m = SYSTEM_PREFIX.match(name)
    if m:
        rest = name[m.end():]
        return m.group(1).lower() + '!' + ('?' if rest.startswith('0x') else rest)
    return name


def parse_dump(path, pid):
    header = {}
    pid_re = re.compile(r'\(\s*%d\)$' % pid)
    names = {}       # function name -> id
    name_list = []
    samples = []     # [ts_us, tid, [frame ids leaf first]]
    pending = {}
    switches = []    # (ts, tid, kind 0 in / 1 out, cpu)
    wait_stacks = {}  # (ts, tid) -> [frame ids]  stack recorded with a switch-in of our thread
    ready = []       # (ts, readied tid) not used yet
    last_ts = 0

    def nid(n):
        i = names.get(n)
        if i is None:
            i = names[n] = len(name_list)
            name_list.append(n)
        return i

    cs = None
    in_header = True
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if in_header:
                if line.startswith('EndHeader'):
                    in_header = False
                    h = header.get('CSwitch')
                    if h:
                        cs = {k: h.index(k) + 1 for k in h}
                    continue
                parts = [p.strip() for p in line.split(',')]
                if parts and parts[0] != 'BeginHeader':
                    header[parts[0]] = parts[1:]
                continue
            s = line.lstrip()
            c = s[:3]
            if c == 'Sam':
                if not s.startswith('SampledProfile,'):
                    continue
                parts = s.split(',')
                if not pid_re.search(parts[2].strip()):
                    continue
                ts = int(parts[1])
                tid = int(parts[3])
                smp = [ts, tid, [], nid(clean(parts[7]))]
                samples.append(smp)
                pending[(ts, tid)] = smp[2]
                last_ts = ts
            elif c == 'Sta':
                if not s.startswith('Stack,'):
                    continue
                parts = s.split(',', 5)
                key = (int(parts[1]), int(parts[2]))
                st = pending.get(key)
                if st is not None:
                    st.append(nid(clean(parts[5])))
            elif c == 'CSw' and cs is not None:
                if not s.startswith('CSwitch,'):
                    continue
                parts = [p.strip() for p in s.split(',')]
                try:
                    ts = int(parts[cs['TimeStamp']])
                    newp = parts[cs['New Process Name ( PID)']]
                    oldp = parts[cs['Old Process Name ( PID)']]
                except (KeyError, ValueError, IndexError):
                    continue
                if pid_re.search(newp):
                    tid = int(parts[cs['New TID']])
                    switches.append((ts, tid, 0))
                    st = []
                    wait_stacks[(ts, tid)] = st
                    pending[(ts, tid)] = st
                if pid_re.search(oldp):
                    switches.append((ts, int(parts[cs['Old TID']]), 1))
            if len(pending) > 200000:
                cut = last_ts - 2000000
                for k in [k for k in pending if k[0] < cut]:
                    del pending[k]
    wait_stacks = {k: v for k, v in wait_stacks.items() if v}
    return {'names': name_list, 'samples': samples, 'switches': switches, 'wait_stacks': wait_stacks, 'pid': pid}


def load(path, pid):
    cache = re.sub(r'\.dump\.csv$', '', path) + '.samples.pkl.gz'
    if path.endswith('.pkl.gz'):
        cache = path
    if os.path.exists(cache) and (path == cache or os.path.getmtime(cache) >= os.path.getmtime(path)):
        with gzip.open(cache, 'rb') as f:
            return pickle.load(f)
    if pid is None:
        sys.exit('--pid is needed to parse a dump')
    data = parse_dump(path, pid)
    with gzip.open(cache, 'wb', compresslevel=3) as f:
        pickle.dump(data, f, protocol=pickle.HIGHEST_PROTOCOL)
    return data


def class_definitions(src):
    """names of the classes and structs defined (not just declared) under a source folder"""
    out = set()
    if not src or not os.path.isdir(src):
        return out
    rx = re.compile(r'^[ 	]*(?:class|struct)[ 	]+(?:[A-Z_]+_API[ 	]+)?([A-Za-z_]\w*)[^;{]*\{', re.M)
    for root, _, files in os.walk(src):
        for fn in files:
            if fn.endswith(('.h', '.hpp', '.cpp')):
                try:
                    with open(os.path.join(root, fn), encoding='utf-8', errors='replace') as f:
                        out.update(rx.findall(f.read()))
                except OSError:
                    pass
    return out


def playerbots_classes(src):
    """classes mod-playerbots defines, without those the core defines too"""
    out = class_definitions(src)
    if out:
        core = os.path.normpath(os.path.join(src, '..', '..', '..', 'src'))
        out -= class_definitions(core)
    return out


class Report:
    def __init__(self, data, args):
        self.names = data['names']
        self.samples = data['samples']
        self.switches = data['switches']
        self.wait_stacks = data['wait_stacks']
        self.args = args
        self.out = []
        self.pb = playerbots_classes(args.playerbots_src)
        self.ga = class_definitions(args.ga_src) if args.ga_src else set()
        self.owner_cache = {}
        self.total = len(self.samples)
        ts = [s[0] for s in self.samples]
        self.t0, self.t1 = (min(ts), max(ts)) if ts else (0, 1)
        self.seconds = args.seconds or max(1e-9, (self.t1 - self.t0) / 1e6)
        self.roles = {}
        self.find_roles()

    def p(self, s=''):
        self.out.append(s)

    def frames(self, smp):
        st = smp[2]
        return st if st else [smp[3]]

    def owner(self, fid):
        o = self.owner_cache.get(fid)
        if o is not None:
            return o
        n = self.names[fid]
        head = re.sub(r'<.*', '', n)
        idents = IDENT.findall(head)[:4]
        if 'GuildAdventure' in n or any(i in self.ga for i in idents):
            o = 'mod-guild-adventure'
        elif SYSTEM_PREFIX.match(n):
            o = 'system (' + n.split('!')[0] + ')'
            if n.startswith(('ntoskrnl', 'ntkrnl', 'hal.', 'win32k')) or n.endswith('.sys!?'):
                o = 'system (kernel)'
        elif n.startswith(('dtNavMesh', 'dtQuery', 'dtNode', 'dtAlloc', 'dtFree', 'dtClosest', 'dtDistance', 'dtIntersect', 'dtPoint', 'dtTri', 'dtOverlap', 'dtCalc', 'dtRand', 'dtMerge', 'dtProxim')) or n.startswith('dt'):
            o = 'Detour'
        else:
            o = 'core'
            for ident in idents:
                if ident in self.pb:
                    o = 'mod-playerbots'
                    break
            if o == 'core' and n.startswith(('std::', 'boost::', 'fmt::', '_', 'mem', 'str', 'operator ', 'G3D::')):
                o = 'runtime / std / libs'
        self.owner_cache[fid] = o
        return o

    def find_roles(self):
        votes = collections.defaultdict(collections.Counter)
        markers = [
            ('map worker', 'MapUpdater::WorkerThread'),
            ('world', 'WorldUpdateLoop'),
            ('world', 'World::Update'),
            ('database', 'DatabaseWorker::WorkerThread'),
            ('database', 'DatabaseWorker'),
            ('network / asio', 'boost::asio'),
            ('soap', 'process_message'),
            ('soap', 'ACSoapThread'),
            ('freeze detector', 'FreezeDetector'),
        ]
        ids = {}
        for role, marker in markers:
            for i, n in enumerate(self.names):
                if marker in n:
                    ids.setdefault(i, role)
        for smp in self.samples:
            role = 'other'
            for fid in reversed(smp[2]):
                r = ids.get(fid)
                if r:
                    role = r
                    break
            votes[smp[1]][role] += 1
        for tid, c in votes.items():
            best = [r for r, _ in c.most_common() if r != 'other']
            self.roles[tid] = best[0] if best else 'other'

    # ---- sections ---------------------------------------------------------------------------------
    def threads(self):
        per = collections.Counter(s[1] for s in self.samples)
        nostack = sum(1 for s in self.samples if not s[2])
        self.p('== threads ==')
        self.p('%d samples of the process in %.1f s = %.2f cores on average; %d samples (%.1f %%) came without a stack' % (
            self.total, self.seconds, self.total / 1000.0 / self.seconds, nostack, 100.0 * nostack / max(1, self.total)))
        self.p('%8s  %-16s %8s %7s' % ('thread', 'role', 'samples', 'cores'))
        for tid, n in per.most_common(16):
            self.p('%8d  %-16s %8d %7.2f' % (tid, self.roles.get(tid, 'other'), n, n / 1000.0 / self.seconds))
        by_role = collections.Counter()
        for tid, n in per.items():
            by_role[self.roles.get(tid, 'other')] += n
        self.p('by role: ' + ', '.join('%s %.2f cores (%.1f %%)' % (r, n / 1000.0 / self.seconds, 100.0 * n / self.total) for r, n in by_role.most_common()))
        self.p()

    def owners(self):
        self.p('== owners ==')
        ex = collections.Counter()
        under_pb = under_ga = under_both = 0
        ex_under_pb = collections.Counter()
        ex_under_ga = collections.Counter()
        for smp in self.samples:
            fr = self.frames(smp)
            leaf_owner = self.owner(fr[0])
            ex[leaf_owner] += 1
            has_pb = has_ga = False
            for fid in fr:
                o = self.owner(fid)
                if o == 'mod-playerbots':
                    has_pb = True
                elif o == 'mod-guild-adventure':
                    has_ga = True
            if has_pb:
                under_pb += 1
                ex_under_pb[leaf_owner] += 1
            if has_ga:
                under_ga += 1
                ex_under_ga[leaf_owner] += 1
            if has_pb and has_ga:
                under_both += 1
        self.p('exclusive (the sampled function itself):')
        for o, n in ex.most_common(14):
            self.p('  %5.1f %%  %s' % (100.0 * n / self.total, o))
        self.p('inclusive: %.1f %% of the samples have a mod-playerbots frame in their stack, %.1f %% a mod-guild-adventure frame (%.1f %% both)' % (
            100.0 * under_pb / self.total, 100.0 * under_ga / self.total, 100.0 * under_both / self.total))
        self.p('  under mod-playerbots, the sampled function is: ' + ', '.join('%s %.1f %%' % (o, 100.0 * n / self.total) for o, n in ex_under_pb.most_common(6)))
        self.p('  under mod-guild-adventure, the sampled function is: ' + ', '.join('%s %.1f %%' % (o, 100.0 * n / self.total) for o, n in ex_under_ga.most_common(6)))
        self.p()

    def domains(self):
        self.p('== domains (share of all samples with such a frame anywhere in the stack; they overlap) ==')
        compiled = [(label, re.compile(rx)) for label, rx in DOMAINS]
        match = {}
        for i, n in enumerate(self.names):
            hits = [k for k, (_, rx) in enumerate(compiled) if rx.search(n)]
            if hits:
                match[i] = hits
        cnt = collections.Counter()
        by_role = collections.defaultdict(collections.Counter)
        for smp in self.samples:
            seen = set()
            for fid in self.frames(smp):
                for k in match.get(fid, ()):
                    seen.add(k)
            role = self.roles.get(smp[1], 'other')
            for k in seen:
                cnt[k] += 1
                by_role[k][role] += 1
        for k, n in cnt.most_common():
            roles = ', '.join('%s %.1f' % (r, 100.0 * c / self.total) for r, c in by_role[k].most_common(3))
            self.p('  %5.1f %%  %-62s (%s)' % (100.0 * n / self.total, compiled[k][0], roles))
        self.p()

    def tree(self, role, start_markers, focus=None):
        if focus:
            sel = self.samples
        else:
            sel = [s for s in self.samples if self.roles.get(s[1]) == role]
        if not sel:
            return
        start_ids = set(i for i, n in enumerate(self.names) if any(n == m or n.startswith(m) for m in start_markers))
        root = [0, {}]
        for smp in sel:
            fr = list(reversed(self.frames(smp)))
            for k, fid in enumerate(fr):
                if fid in start_ids:
                    fr = fr[k:]
                    break
            else:
                if focus:
                    continue
                fr = [-1] + fr[-6:] if smp[2] else [-2, smp[3]]
            node = root
            node[0] += 1
            for fid in fr[:self.args.depth]:
                ch = node[1].get(fid)
                if ch is None:
                    ch = node[1][fid] = [0, {}]
                ch[0] += 1
                node = ch
        if focus:
            self.p('== call tree below %s (%d samples, %.1f %% of the process; pruned below %.2f %% of the process) ==' % (
                focus, root[0], 100.0 * root[0] / self.total, self.args.min))
        else:
            self.p('== call tree: %s threads (%d samples, %.1f %% of the process; pruned below %.2f %% of the process) ==' % (
                role, len(sel), 100.0 * len(sel) / self.total, self.args.min))
        thr = self.args.min / 100.0 * self.total

        def name(fid):
            if fid == -1:
                return '[stack without the thread root]'
            if fid == -2:
                return '[no stack]'
            n = self.names[fid]
            return n if len(n) <= 150 else n[:147] + '...'

        def walk(node, depth):
            kids = sorted(node[1].items(), key=lambda kv: -kv[1][0])
            shown = 0
            for fid, ch in kids:
                if ch[0] < thr:
                    continue
                shown += ch[0]
                label = name(fid)
                cur = ch
                # collapse chains: one child that carries nearly everything
                while True:
                    kk = [(f, c) for f, c in cur[1].items()]
                    if len(kk) >= 1:
                        f2, c2 = max(kk, key=lambda kv: kv[1][0])
                        if c2[0] >= 0.97 * cur[0] and c2[0] >= thr and len(label) < 240:
                            label += ' > ' + name(f2)
                            cur = c2
                            continue
                    break
                self_n = cur[0] - sum(c[0] for c in cur[1].values())
                self.p('%s%5.1f %%  %s%s' % ('  ' * depth, 100.0 * ch[0] / self.total, label,
                                              ('   [self %.1f %%]' % (100.0 * self_n / self.total)) if self_n >= thr else ''))
                walk(cur, depth + 1)
            rest = node[0] - shown - (node[0] - sum(c[0] for c in node[1].values()))
            if depth > 0 and rest >= thr:
                self.p('%s%5.1f %%  (smaller callees together)' % ('  ' * depth, 100.0 * rest / self.total))

        walk(root, 0)
        self.p()

    def functions(self):
        inc = collections.Counter()
        exc = collections.Counter()
        for smp in self.samples:
            fr = self.frames(smp)
            exc[fr[0]] += 1
            for fid in set(fr):
                inc[fid] += 1
        # the same with the runtime folded into its caller: who allocates, hashes, copies strings, locks
        folded = collections.Counter()
        folded_rt = collections.Counter()
        for smp in self.samples:
            fr = self.frames(smp)
            for k, fid in enumerate(fr):
                o = self.owner(fid)
                if not (o.startswith('system') or o == 'runtime / std / libs'):
                    folded[fid] += 1
                    if k > 0:
                        folded_rt[fid] += 1
                    break
        self.p('== top functions, exclusive with the runtime folded in (the function, or system / C++ runtime code it called directly) ==')
        self.p('   total   of it runtime')
        for fid, n in folded.most_common(self.args.top):
            self.p('  %5.2f %%  %5.2f %%  %-22s %s' % (100.0 * n / self.total, 100.0 * folded_rt[fid] / self.total, '[' + self.owner(fid)[:20] + ']', self.names[fid][:160]))
        self.p()
        self.p('== top functions, exclusive (the function itself was on the CPU) ==')
        for fid, n in exc.most_common(self.args.top):
            self.p('  %5.2f %%  %-22s %s' % (100.0 * n / self.total, '[' + self.owner(fid)[:20] + ']', self.names[fid][:170]))
        self.p()
        self.p('== top functions, inclusive (the function or anything it called) ==')
        skip = re.compile(r'^(ntdll|kernel32|ucrtbase|kernelbase)\.dll!|^std::thread::_Invoke|^main$|^__scrt|^invoke_main|^mainCRTStartup')
        shown = 0
        for fid, n in inc.most_common(self.args.top * 3):
            if skip.search(self.names[fid]):
                continue
            self.p('  %5.2f %%  %-22s %s' % (100.0 * n / self.total, '[' + self.owner(fid)[:20] + ']', self.names[fid][:170]))
            shown += 1
            if shown >= self.args.top * 2:
                break
        self.p()

    def on_cpu(self):
        """per thread: sorted list of (start, end) on-CPU intervals from the context switches"""
        iv = collections.defaultdict(list)
        open_at = {}
        for ts, tid, kind in sorted(self.switches):
            if kind == 0:
                open_at[tid] = ts
            else:
                st = open_at.pop(tid, None)
                if st is not None and ts > st:
                    iv[tid].append((st, ts))
        return iv

    def overlap(self, iv, a, b):
        """microseconds of the sorted intervals iv inside [a, b)"""
        import bisect
        if not iv:
            return 0
        k = bisect.bisect_right(self._iv_starts[id(iv)], a) - 1
        k = max(k, 0)
        tot = 0
        while k < len(iv) and iv[k][0] < b:
            lo = max(iv[k][0], a)
            hi = min(iv[k][1], b)
            if hi > lo:
                tot += hi - lo
            k += 1
        return tot

    def ticks(self):
        world = [t for t, r in self.roles.items() if r == 'world']
        if not world:
            return
        wtid = max(world, key=lambda t: sum(1 for s in self.samples if s[1] == t))
        wait_id = [i for i, n in enumerate(self.names) if n == 'MapUpdater::wait']
        self.p('== ticks of the world thread ==')
        if not self.wait_stacks or not wait_id:
            self.p('needs a trace made with -Waits (the stacks of the context switches name the wait for the maps)')
            self.p()
            return
        wait_id = wait_id[0]
        # the world thread's waits: (out, in, kind)
        waits = []
        last_out = None
        for ts, tid, kind in sorted(self.switches):
            if tid != wtid:
                continue
            if kind == 1:
                last_out = ts
            elif last_out is not None:
                st = self.wait_stacks.get((ts, tid), [])
                k = 'maps' if wait_id in st else ('other' if st else 'unknown')
                if k == 'other':
                    own = [self.names[f] for f in st if not SYSTEM_PREFIX.match(self.names[f])]
                    if own and (own[0] == 'WorldUpdateLoop' or 'sleep' in own[0]):
                        k = 'sleep'
                waits.append((last_out, ts, k))
                last_out = None
        marks = [w for w in waits if w[2] == 'maps']
        if len(marks) < 20:
            self.p('too few waits for the maps found (%d)' % len(marks))
            self.p()
            return
        iv = self.on_cpu()
        self._iv_starts = {id(v): [a for a, _ in v] for v in iv.values()}
        maps = [t for t, r in self.roles.items() if r == 'map worker']
        # rows: length, world cpu, wait for maps, sleep, other waits, busiest map worker, all map workers (ms), start, end
        rows = []
        wi = 0
        for i in range(len(marks) - 1):
            a, b = marks[i][1], marks[i + 1][1]
            mapwait = (marks[i + 1][1] - marks[i + 1][0]) / 1000.0
            sl = ot = 0.0
            while wi < len(waits) and waits[wi][1] <= a:
                wi += 1
            k = wi
            while k < len(waits) and waits[k][1] < b:
                d = (waits[k][1] - waits[k][0]) / 1000.0
                if waits[k][2] == 'sleep':
                    sl += d
                elif waits[k][2] != 'maps':
                    ot += d
                k += 1
            wc = self.overlap(iv.get(wtid, []), a, b) / 1000.0
            mc = [self.overlap(iv.get(t, []), a, b) / 1000.0 for t in maps]
            rows.append(((b - a) / 1000.0, wc, mapwait, sl, ot, max(mc) if mc else 0.0, sum(mc), a, b))
        dur_sorted = sorted(r[0] for r in rows)

        def pct(q):
            return dur_sorted[min(len(dur_sorted) - 1, int(q / 100.0 * len(dur_sorted)))]

        n = len(rows)
        total = sum(r[0] for r in rows)
        self.p('%d ticks in %.1f s (a tick: from the end of one wait for the maps to the end of the next): mean %.1f ms, median %.1f, p90 %.1f, p95 %.1f, p99 %.1f, max %.1f' % (
            n, total / 1000.0, total / n, pct(50), pct(90), pct(95), pct(99), dur_sorted[-1]))

        def line(label, sel):
            if not sel:
                return
            m = lambda k: statistics.mean(r[k] for r in sel)
            self.p('  %-26s %5d ticks, %5.1f %% of the time: mean %7.1f ms = world thread works %6.1f + waits for the maps %6.1f + sleeps %5.1f + other waits %5.1f | busiest map worker %6.1f, all map workers %6.1f' % (
                label, len(sel), 100.0 * sum(r[0] for r in sel) / total, m(0), m(1), m(2), m(3), m(4), m(5), m(6)))

        line('all ticks', rows)
        p50, p95, p99 = pct(50), pct(95), pct(99)
        line('fastest half', [r for r in rows if r[0] <= p50])
        line('p50 to p95', [r for r in rows if p50 < r[0] < p95])
        line('p95 to p99', [r for r in rows if p95 <= r[0] < p99])
        line('slowest 1 %', [r for r in rows if r[0] >= p99])
        for lim in (50, 100, 200):
            sel = [r for r in rows if r[0] >= lim]
            if sel:
                line('ticks of %d ms and more' % lim, sel)
        slow = [r for r in rows if r[0] >= p95]
        rest = [r for r in rows if r[0] < p95]
        if slow:
            by_world = sum(1 for r in slow if r[1] > r[2])
            self.p("  of the slowest 5 %% of the ticks (from %.0f ms), the world thread's own work is the larger part in %d, the wait for the maps in %d" % (p95, by_world, len(slow) - by_world))
        ph_cnt = collections.Counter()
        wu = [i for i, nm in enumerate(self.names) if nm == 'World::Update']
        if wu:
            wu = wu[0]
            tot = 0
            for smp in self.samples:
                if smp[1] != wtid or not smp[2]:
                    continue
                st = smp[2]
                tot += 1
                try:
                    k = st.index(wu)
                except ValueError:
                    ph_cnt['outside World::Update'] += 1
                    continue
                ph_cnt[self.names[st[k - 1]][:50] if k > 0 else 'World::Update itself'] += 1
            self.p("  the world thread's CPU by the part of World::Update: " + ', '.join('%s %.1f %%' % (k, 100.0 * c / max(1, tot)) for k, c in ph_cnt.most_common(7)))
        self.tick_classes(rows, p50, p95)
        if slow and rest:
            self.slow_profile(slow, rest, 'slowest 5 % of the ticks')
        vlim = max(100.0, p99)
        vslow = [r for r in rows if r[0] >= vlim]
        if vslow and len(vslow) < len(slow):
            self.slow_profile(vslow, [r for r in rows if r[0] < p95], 'ticks of %.0f ms and more' % vlim)
        self.p()

    def group_of(self, smp, cache={}):
        """index into WORK_GROUPS for a sample, or None"""
        role = self.roles.get(smp[1])
        if role not in ('world', 'map worker'):
            return None
        if 'ids' not in cache:
            ids = {}
            for i, n in enumerate(self.names):
                ids.setdefault(n, i)
            cache['ids'] = [(k, r, [ids.get(f, -9) for f in fr]) for k, (_, r, fr) in enumerate(WORK_GROUPS)]
        st = smp[2]
        have = set(st)
        for k, r, need in cache['ids']:
            if r == role and all(f in have for f in need):
                return k
        return None

    def tick_classes(self, rows, p50, p95):
        """CPU milliseconds per tick by kind of work, for fast, middle and slow ticks"""
        import bisect
        classes = [('fastest half', [r for r in rows if r[0] <= p50]),
                   ('p50 to p95', [r for r in rows if p50 < r[0] < p95]),
                   ('slowest 5 %', [r for r in rows if r[0] >= p95]),
                   ('all ticks', rows)]
        starts = [r[7] for r in rows]
        cls_of = []
        for r in rows:
            cls_of.append(0 if r[0] <= p50 else (1 if r[0] < p95 else 2))
        sums = [collections.Counter() for _ in range(4)]
        for smp in self.samples:
            g = self.group_of(smp)
            if g is None:
                continue
            k = bisect.bisect_right(starts, smp[0]) - 1
            if k < 0 or smp[0] >= rows[k][8]:
                continue
            label = WORK_GROUPS[g][0]
            sums[cls_of[k]][label] += 1
            sums[3][label] += 1
        self.p('  CPU per tick by kind of work (ms of all threads together; a sample is 1 ms):')
        self.p('    %-60s %s' % ('', '  '.join('%13s' % c[0] for c in classes)))
        self.p('    %-60s %s' % ('ticks (mean length ms)', '  '.join('%6d (%4.0f)' % (len(c[1]), statistics.mean(r[0] for r in c[1]) if c[1] else 0) for c in classes)))
        order = []
        for label, _, _ in WORK_GROUPS:
            if label not in order:
                order.append(label)
        for label in order:
            vals = []
            for i, c in enumerate(classes):
                n = len(c[1])
                vals.append('%13.2f' % (sums[i][label] / n if n else 0.0))
            self.p('    %-60s %s' % (label, '  '.join(vals)))
        self.p('    per second of wall time, all ticks: ' + ', '.join('%s %.0f ms' % (label.split(':')[0] + ':' + label.split(':')[1][:22] if ':' in label else label[:30],
            1000.0 * sums[3][label] / max(1.0, sum(r[0] for r in rows))) for label in order if sums[3][label]))

    def slow_profile(self, slow, rest, label):
        """what runs in the slow ticks: inclusive functions by role, against their share in the other ticks"""
        import bisect
        s_iv = sorted((r[7], r[8]) for r in slow)
        s_starts = [a for a, _ in s_iv]
        r_iv = sorted((r[7], r[8]) for r in rest)
        r_starts = [a for a, _ in r_iv]

        def inside(ts, starts, ivs):
            k = bisect.bisect_right(starts, ts) - 1
            return k >= 0 and ts < ivs[k][1]

        for role in ('world', 'map worker'):
            inc_s = collections.Counter()
            inc_r = collections.Counter()
            ns = nr = 0
            for smp in self.samples:
                if self.roles.get(smp[1]) != role:
                    continue
                if inside(smp[0], s_starts, s_iv):
                    ns += 1
                    for fid in set(self.frames(smp)):
                        inc_s[fid] += 1
                elif inside(smp[0], r_starts, r_iv):
                    nr += 1
                    for fid in set(self.frames(smp)):
                        inc_r[fid] += 1
            if ns < 100 or nr < 100:
                continue
            self.p('  %s, %s threads: functions by their share of the samples there (%d) against the other ticks (%d)' % (label, role, ns, nr))
            scored = []
            for fid, c in inc_s.items():
                a = c / ns
                b = inc_r.get(fid, 0) / nr
                if a >= 0.04 and (a - b >= 0.03 or a >= 2 * b):
                    scored.append((a - b, a, b, fid))
            scored.sort(reverse=True)
            for d, a, b, fid in scored[:28]:
                self.p('    %5.1f %% / %5.1f %%   %-20s %s' % (100 * a, 100 * b, '[' + self.owner(fid)[:18] + ']', self.names[fid][:150]))

    def waits(self):
        if not self.wait_stacks:
            return
        last_out = {}
        blocked = collections.Counter()
        blocked_role = collections.Counter()
        skip = re.compile(r'^(ntoskrnl|ntkrnl|hal|win32k)\w*\.(exe|dll|sys)!|\.sys!')
        for ts, tid, kind in sorted(self.switches):
            if kind == 1:
                last_out[tid] = ts
                continue
            st = self.wait_stacks.get((ts, tid))
            out = last_out.get(tid)
            if st is None or out is None:
                continue
            d = ts - out
            role = self.roles.get(tid, 'other')
            if role not in ('world', 'map worker'):
                continue
            # the first frames of our own code, innermost first
            own = [self.names[f] for f in st if not SYSTEM_PREFIX.match(self.names[f])][:5]
            key = (role, ' < '.join(n[:70] for n in own) if own else '[system only]')
            blocked[key] += d
            blocked_role[role] += d
        self.p('== waits: off-CPU time of world and map threads by the code they were blocked in (innermost own frames) ==')
        for role, d in blocked_role.most_common():
            self.p('%s threads were off the CPU for %.1f s in all (%.1f s recorded)' % (role, d / 1e6, self.seconds))
        for (role, key), d in blocked.most_common(30):
            self.p('  %7.2f s  %-11s %s' % (d / 1e6, role, key))
        self.p()

    def run(self):
        if self.args.focus:
            for f in self.args.focus:
                self.tree(None, [f], focus=f)
            return '\n'.join(self.out)
        self.threads()
        self.owners()
        self.domains()
        self.ticks()
        self.waits()
        self.tree('map worker', ['MapUpdater::WorkerThread'])
        self.tree('world', ['WorldUpdateLoop', 'World::Update'])
        self.tree('database', ['DatabaseWorker::WorkerThread'])
        self.tree('other', ['#none#'])
        self.functions()
        return '\n'.join(self.out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump')
    ap.add_argument('--pid', type=int)
    ap.add_argument('--playerbots-src')
    ap.add_argument('--ga-src', help='modules/mod-guild-adventure/src: its classes count as our module')
    ap.add_argument('--min', type=float, default=0.5, help='prune call tree nodes below this percent of the process')
    ap.add_argument('--depth', type=int, default=60)
    ap.add_argument('--top', type=int, default=60)
    ap.add_argument('--seconds', type=float)
    ap.add_argument('--focus', action='append', help='print only the call tree below this function (may be given more than once)')
    ap.add_argument('--out')
    args = ap.parse_args()
    data = load(args.dump, args.pid)
    text = Report(data, args).run()
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(text + '\n')
        print('wrote', args.out)
    else:
        sys.stdout.reconfigure(encoding='utf-8')
        print(text)


if __name__ == '__main__':
    main()
