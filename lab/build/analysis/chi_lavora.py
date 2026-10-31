#!/usr/bin/env python3
"""chi_lavora.py perf-report.txt.gz [waiting-thread ...] - while a thread WAITS (samples inside the game's wait
functions or the emulated kernel's wait calls, or asleep), who is still working?
For each wait instant it checks which other threads are running (from the exact context switches) and whether they
run real code or are themselves waiting (from the nearest sample). It says how much of the wait is 'dead time'
(nobody works for the waiter) and how much is other threads' work in progress."""
import gzip, sys, collections, bisect, re
WAIT_FN = ('sub_82545448','sub_82544ED8','sub_82545138','sub_825462A0','sub_825AF8F0','sub_82544D18','sub_82545BC8',
           'sub_8238FAC8','sub_82546458','sub_82546210','sub_82544220')   # Black Ops job system / wait loops
WAIT_EXPORT = ('NtWaitForSingleObjectEx','NtYieldExecution','NtPulseEvent','KeWaitForSingleObject','NtSetEvent',
               'NtReleaseSemaphore','KeDelayExecutionThread','NtWaitForMultipleObjectsEx','KeSetEvent')
path = sys.argv[1]; targets = sys.argv[2:] or ['Guest CPU 5', 'Guest CPU 1']
names = {}; ons = collections.defaultdict(list); offs = collections.defaultdict(list)
samples = collections.defaultdict(list)   # tid -> [(time, kind)] kind: 'work' | 'wait'
cur = None; cs = None
def classify(syms, files):
    for s in syms:
        m = re.search(r'RegisterExport<&xe::kernel::[a-z_]+::(\w+?)_entry', s)
        if m: return 'wait' if m.group(1) in WAIT_EXPORT else 'work'
    if syms and syms[0].startswith('sub_'): return 'wait' if syms[0] in WAIT_FN else 'work'
    if any('GuestScheduler' in s or 'xe::threading' in s for s in syms[:6]): return 'wait'
    return 'work'
def flush():
    if not cur or 'tid' not in cur: return
    names[cur['tid']] = cur.get('name', '?')
    if cur.get('event') == 'cpu-clock' and cur['syms']:
        samples[cur['tid']].append((cur['time'], classify(cur['syms'], None)))
with gzip.open(path, 'rt', errors='replace') as f:
    for line in f:
        s = line.strip()
        if line.startswith('sample:'): flush(); cur = {'syms': []}; cs = None
        elif line.startswith('context_switch:'): flush(); cur = None; cs = {}
        elif cur is not None:
            if s.startswith('event_type:'): cur['event'] = s.split(': ')[1]
            elif s.startswith('time:'): cur['time'] = int(s.split(': ')[1])
            elif s.startswith('thread_id:'): cur['tid'] = int(s.split(': ')[1])
            elif s.startswith('thread_name:'): cur['name'] = s.split(': ', 1)[1]
            elif s.startswith('symbol:'): cur['syms'].append(s.split(': ', 1)[1])
        elif cs is not None:
            if s.startswith('switch_on:'): cs['on'] = s.endswith('true')
            elif s.startswith('time:'): cs['time'] = int(s.split(': ')[1])
            elif s.startswith('thread_id:'):
                (ons if cs.get('on') else offs)[int(s.split(': ')[1])].append(cs['time']); cs = None
flush()
for d in (ons, offs):
    for k in d: d[k].sort()
for k in samples: samples[k].sort()
def running(tid, t):
    """True if tid is on a CPU at time t."""
    i = bisect.bisect_right(ons[tid], t) - 1
    if i < 0: return False
    j = bisect.bisect_right(offs[tid], t) - 1
    return j < 0 or offs[tid][j] < ons[tid][i]
def doing(tid, t):
    """'work' / 'wait' from the nearest sample within 1 ms while running, '?' if none."""
    lst = samples.get(tid)
    if not lst: return '?'
    i = bisect.bisect_left(lst, (t, ''))
    best = None
    for j in (i - 1, i):
        if 0 <= j < len(lst) and abs(lst[j][0] - t) <= 1_000_000:
            if best is None or abs(lst[j][0] - t) < abs(best[0] - t): best = lst[j]
    return best[1] if best else '?'
guest = sorted((tid for tid, n in names.items() if n.startswith('Guest CPU')), key=lambda t: names[t])
cp = [tid for tid, n in names.items() if n.startswith('GPU Commands')]
tmin = min(v[0] for v in ons.values() if v); tmax = max(v[-1] for v in ons.values() if v); dur = (tmax - tmin) / 1e9
for target in targets:
    tids = [tid for tid in guest if names[tid] == target]
    if not tids: print('thread not found:', target); continue
    me = tids[0]
    others = [t for t in guest if t != me]
    # probe points, weighted by time (ns): every waiting sample of the target (0.5 ms each), and its sleeps
    # (each one its real length, looked at in its middle; long ones every 250 us)
    probes = [(t, 'attesa attiva', 500_000) for t, k in samples[me] if k == 'wait']
    for t_off in offs[me]:
        i = bisect.bisect_right(ons[me], t_off)
        if i < len(ons[me]):
            length = ons[me][i] - t_off
            if length <= 0 or length > 100_000_000: continue
            pieces = max(1, length // 250_000)
            for k in range(pieces):
                probes.append((t_off + (2 * k + 1) * length // (2 * pieces), 'addormentato', length // pieces))
    work_ns = sum(500_000 for t, k in samples[me] if k == 'work')
    tot = collections.Counter(); detail = collections.Counter(); bykind = collections.Counter()
    for t, kind, w in probes:
        workers = [o for o in others if running(o, t) and doing(o, t) == 'work']
        waiters = [o for o in others if running(o, t) and doing(o, t) != 'work']
        cp_on = any(running(c, t) for c in cp)
        if workers: key = 'altri thread del gioco lavorano (%d)' % min(len(workers), 3)
        elif cp_on: key = 'nessun thread del gioco lavora, il thread GPU si'
        elif waiters: key = 'TEMPO MORTO: gli altri svegli stanno solo aspettando'
        else: key = 'TEMPO MORTO: tutti gli altri dormono'
        tot[key] += w; bykind[kind] += w
        for o in workers: detail[names[o]] += w
    n = sum(tot.values()) or 1
    print(f'\n=== {target}: real work {work_ns/1e9/dur*100:.1f}% of the time, waiting {n/1e9/dur*100:.1f}% '
          f'({bykind["attesa attiva"]/1e9/dur*100:.1f}% awake + {bykind["addormentato"]/1e9/dur*100:.1f}% asleep)')
    for k, v in tot.most_common(): print(f'   {100*v/n:5.1f}% of the wait = {v/1e9/dur*100:5.1f}% of the time  {k}')
    print('   who works during the wait: ' + ', '.join(f'{k} {100*v/n:.0f}%' for k, v in detail.most_common()))

# ---- overlap of real work between threads: how many work at the same instant
grid = collections.Counter(); pair = collections.Counter(); alone = collections.Counter()
step = 250_000
t = tmin
allthreads = guest + cp
while t < tmax:
    w = [o for o in allthreads if running(o, t) and (o in cp or doing(o, t) == 'work')]
    gw = [o for o in w if o not in cp]
    grid[len(gw)] += 1
    if len(gw) == 1: alone[names[gw[0]]] += 1
    for i in range(len(gw)):
        for j in range(i + 1, len(gw)): pair[(names[gw[i]], names[gw[j]])] += 1
    t += step
n = sum(grid.values())
print('\n=== game threads doing real work at the same instant (sampled every 250 us)')
print('   ' + ', '.join(f'{k} thread: {100*v/n:.1f}%' for k, v in sorted(grid.items())))
print('   average: %.2f threads working' %(sum(k * v for k, v in grid.items()) / n))
print('   alone: ' +', '.join(f'{k} {100*v/n:.1f}%' for k, v in alone.most_common()))
print('   pairs together: ' +', '.join(f'{a[-1]}+{b[-1]} {100*v/n:.1f}%' for (a, b), v in pair.most_common(8)))
work_share = {names[o]: sum(1 for tt, k in samples[o] if k == 'work') * 0.0005 / dur for o in guest}
print('   real work per thread (share of time): ' +', '.join(f'{k[-5:]} {100*v:.0f}%' for k, v in sorted(work_share.items())))
m, r = [o for o in guest if names[o] == 'Guest CPU 5'][0], [o for o in guest if names[o] == 'Guest CPU 1'][0]
both = sum(v for (a, b), v in pair.items() if {a, b} == {'Guest CPU 5', 'Guest CPU 1'}) / n
pm, pr = work_share['Guest CPU 5'], work_share['Guest CPU 1']
print(f'   main (CPU 5) and render (CPU 1) working together: {100*both:.1f}% of the time; if independent: {100*pm*pr:.1f}%')
