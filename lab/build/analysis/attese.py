#!/usr/bin/env python3
"""attese.py perf-report.txt.gz [thread...] - per thread: time running (cpu-clock samples) split by category, and
time OFF the CPU (sched_switch samples with --trace-offcpu: the period up to the next sample gives the sleep length)
split by who called the sleep. Shows where a thread loses wall-clock time."""
import gzip, sys, collections, re
want = sys.argv[2:]
def short(sym):
    m = re.search(r'RegisterExport<&xe::kernel::[a-z_]+::(\w+?)_entry', sym)
    if m: return 'export:' + m.group(1)
    m = re.match(r'(xe::[\w:~<>]+?)\(', sym)
    return m.group(1) if m else sym[:60]
on = collections.defaultdict(collections.Counter)      # thread -> category -> ns
off = collections.defaultdict(collections.Counter)     # thread -> reason -> ns
offn = collections.defaultdict(collections.Counter)
outs = collections.defaultdict(list); ons = collections.defaultdict(list); cs = None
names = {}
tmin = tmax = None
def classify_on(frames):
    # frames: list of (file, symbol), leaf first
    for f, s in frames:
        if 'RegisterExport<' in s: return short(s)
    f0, s0 = frames[0]
    if 'perf-' in f0 and '.map' in f0: return 'jit'
    for f, s in frames[:6]:
        if 'GuestScheduler' in s or 'XThread' in s or 'threading' in s: return 'scheduler/threading'
    if 'libe.so' in f0: return 'libe:' + short(s0)[:50]
    if 'libc.so' in f0: return 'libc:' + s0[:30]
    return f0.split('/')[-1][:30]
def classify_off(frames):
    chain = [s for f, s in frames]
    for f, s in frames:
        if 'RegisterExport<' in s: 
            leaf = chain[0][:14]
            return short(s) + ' [' + leaf + ']'
    for s in chain:
        for key in ('PreemptCurrentFiber', 'TimebaseSpinPark', 'RunLoop', 'IoWorkerLoop', 'WatchdogLoop', 'WaitGuestWakeGeneration', 'ThrottlePresentation', 'CommandProcessor', 'AudioSystem', 'XmaDecoder'):
            if key in s: return key + ' [' + chain[0][:14] + ']'
    return 'altro: ' + ' < '.join(short(s)[:28] for s in chain[:3])
cur = None
def flush():
    global tmin, tmax
    if not cur or 'tid' not in cur: return
    tid = cur['tid']; names[tid] = cur.get('name', str(tid)); t = cur['time']
    tmin = t if tmin is None else min(tmin, t); tmax = t if tmax is None else max(tmax, t)
    frames = cur['frames']
    if not frames: return
    if cur['event'] == 'cpu-clock':
        on[tid][classify_on(frames)] += cur['count']
    else:
        # switch-out sample: the thread left the CPU here; it came back at its next sample.
        outs[tid].append((t, classify_off(frames)))
with gzip.open(sys.argv[1], 'rt', errors='replace') as f:
    for line in f:
        s = line.strip()
        if line.startswith('sample:'):
            flush(); cur = {'frames': []}; pend = [None, None]
        elif line.startswith('context_switch:'):
            flush(); cur = None; cs = {}
        elif cur is None and cs is not None:
            if s.startswith('switch_on:'): cs['on'] = s.endswith('true')
            elif s.startswith('time:'): cs['time'] = int(s.split(': ')[1])
            elif s.startswith('thread_id:'):
                if cs.get('on'): ons[int(s.split(': ')[1])].append(cs['time'])
                cs = None
        elif cur is not None:
            if s.startswith('event_type:'): cur['event'] = s.split(': ')[1]
            elif s.startswith('time:'):
                cur['time'] = int(s.split(': ')[1])
            elif s.startswith('event_count:'): cur['count'] = int(s.split(': ')[1])
            elif s.startswith('thread_id:'):
                cur['tid'] = int(s.split(': ')[1])
            elif s.startswith('thread_name:'): cur['name'] = s.split(': ', 1)[1]
            elif s.startswith('file:'): pend[0] = s.split(': ', 1)[1]
            elif s.startswith('symbol:'):
                pend[1] = s.split(': ', 1)[1]; cur['frames'].append((pend[0] or '', pend[1])); pend = [None, None]
flush()
import bisect
for tid, lst in outs.items():
    o = sorted(ons[tid])
    for t, reason in lst:
        i = bisect.bisect_right(o, t)
        if i < len(o):
            d = o[i] - t
            if 0 < d < 5e9: off[tid][reason] += d; offn[tid][reason] += 1
dur = (tmax - tmin) / 1e9
print(f'durata {dur:.1f} s')
tot = {tid: sum(c.values()) for tid, c in on.items()}
for tid in sorted(tot, key=tot.get, reverse=True)[:10]:
    nm = names[tid]
    if want and not any(w in nm for w in want): continue
    print(f'\n=== {nm}: in esecuzione {tot[tid]/1e9/dur*100:.1f}% del tempo, fuori CPU {sum(off[tid].values())/1e9/dur*100:.1f}%')
    print('  in esecuzione:')
    for k, v in on[tid].most_common(12): print(f'    {v/1e9/dur*100:5.1f}%  {k}')
    print('  off the CPU (sleeps):')
    for k, v in off[tid].most_common(10): print(f'    {v/1e9/dur*100:5.1f}%  {offn[tid][k]/dur:7.0f}/s  media {v/offn[tid][k]/1000:6.0f} us  {k}')
