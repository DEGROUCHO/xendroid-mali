#!/usr/bin/env python3
"""cambi_contesto.py perf-report.txt.gz - risvegli al secondo e durata media di ogni tratto in esecuzione, per thread,
dagli eventi context_switch di un profilo simpleperf con --trace-offcpu. Molti risvegli brevi = thread svegliato a vuoto."""
import gzip, sys, collections
names = {}; on = collections.Counter(); run_ns = collections.Counter(); last_on = {}
tmin = None; tmax = None; cur = None; kind = None
def flush():
    global tmin, tmax
    if not cur: return
    if kind == 'sample':
        if 'tid' in cur and 'name' in cur: names[cur['tid']] = cur['name']
    elif kind == 'cs' and 'tid' in cur and 'time' in cur:
        t = cur['time']; tid = cur['tid']
        tmin = t if tmin is None else min(tmin, t); tmax = t if tmax is None else max(tmax, t)
        if cur.get('on'):
            on[tid] += 1; last_on[tid] = t
        elif tid in last_on:
            run_ns[tid] += t - last_on.pop(tid)
with gzip.open(sys.argv[1], 'rt', errors='replace') as f:
    for line in f:
        s = line.strip()
        if line.startswith('sample:'): flush(); cur = {}; kind = 'sample'
        elif line.startswith('context_switch:'): flush(); cur = {}; kind = 'cs'
        elif cur is not None:
            if s.startswith('thread_id:'): cur['tid'] = int(s.split(': ')[1])
            elif s.startswith('thread_name:'): cur['name'] = s.split(': ', 1)[1]
            elif s.startswith('time:'): cur['time'] = int(s.split(': ')[1])
            elif s.startswith('switch_on:'): cur['on'] = s.endswith('true')
flush()
dur = (tmax - tmin) / 1e9
print(f'durata {dur:.1f} s')
print(f'{"thread":26s} {"risvegli/s":>10s} {"in esecuzione":>14s} {"tratto medio":>13s}')
for tid, n in on.most_common(16):
    r = run_ns[tid] / 1e9
    print(f'{names.get(tid, str(tid))[:26]:26s} {n/dur:10.0f} {100*r/dur:13.1f}% {1e6*r/max(n,1):10.0f} us')
