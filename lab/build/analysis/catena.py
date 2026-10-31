#!/usr/bin/env python3
"""catena.py perf-report.txt.gz - latency of the chain 'pulse -> worker wake-up' and the job rhythm:
for each wake-up of a worker thread (back on CPU after a sleep in RunLoop) it finds the last PulseEvent
(start of a sleep inside NtPulseEvent, or an on-CPU sample inside NtPulseEvent) of another thread and measures the delay."""
import gzip, sys, collections, bisect, statistics
f = gzip.open(sys.argv[1], 'rt', errors='replace')
names = {}; ons = collections.defaultdict(list); offs = collections.defaultdict(list)
pulse_sleep = collections.defaultdict(list)   # tid -> times of switch-out inside NtPulseEvent nanosleep
runloop_sleep = collections.defaultdict(list) # tid -> times of switch-out in RunLoop (idle dispatch thread)
wait_sleep = collections.defaultdict(list)
cur = None; cs = None
def flush():
    if not cur or 'tid' not in cur: return
    names[cur['tid']] = cur.get('name', '?')
    if cur.get('event') != 'sched:sched_switch': return
    ch = cur['syms']
    if not ch: return
    if any('NtPulseEvent_entry' in s for s in ch) and ch[0].startswith('nanosleep'): pulse_sleep[cur['tid']].append(cur['time'])
    elif any('GuestScheduler::RunLoop' in s for s in ch) and not any('RegisterExport<' in s for s in ch) and not any(s.startswith('sub_') for s in ch): runloop_sleep[cur['tid']].append(cur['time'])
    elif any('NtWaitForSingleObjectEx_entry' in s for s in ch): wait_sleep[cur['tid']].append(cur['time'])
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
for d in (ons, offs, pulse_sleep, runloop_sleep, wait_sleep):
    for k in d: d[k].sort()
byname = {v: k for k, v in names.items()}
def q(v, p): 
    v = sorted(v); return v[min(len(v) - 1, int(len(v) * p))] if v else 0
allp = sorted(t for tid, lst in pulse_sleep.items() for t in lst)
tmin = min(min(v) for v in ons.values()); tmax = max(max(v) for v in ons.values()); dur = (tmax - tmin) / 1e9
print(f'duration {dur:.1f} s; sleeps inside PulseEvent: {len(allp)/dur:.0f}/s')
for nm in ('Guest CPU 0', 'Guest CPU 1', 'Guest CPU 2', 'Guest CPU 3', 'Guest CPU 4', 'Guest CPU 5'):
    tid = byname.get(nm)
    if tid is None: continue
    # RunLoop sleep durations (threads with no work) and delay from the nearest pulse before the wake-up
    o = ons[tid]; sl = []; lat = []; lat_pulser_wake = []
    for t in runloop_sleep[tid]:
        i = bisect.bisect_right(o, t)
        if i >= len(o): continue
        tw = o[i]; sl.append(tw - t)
        j = bisect.bisect_left(allp, tw) - 1
        if j >= 0 and tw - allp[j] < 2_000_000 and allp[j] > t: lat.append(tw - allp[j])
    ps = []
    for t in pulse_sleep[tid]:
        i = bisect.bisect_right(o, t)
        if i < len(o): ps.append(o[i] - t)
    ws = []
    for t in wait_sleep[tid]:
        i = bisect.bisect_right(o, t)
        if i < len(o): ws.append(o[i] - t)
    print(f'{nm}: idle sleeps (RunLoop) {len(sl)/dur:5.0f}/s p50 {q(sl,.5)/1000:4.0f} p90 {q(sl,.9)/1000:4.0f} us | wake-up after a pulse {len(lat)/dur:5.0f}/s delay p50 {q(lat,.5)/1000:4.0f} p90 {q(lat,.9)/1000:4.0f} us'
          f' | sleep in pulse {len(ps)/dur:5.0f}/s p50 {q(ps,.5)/1000:3.0f} p90 {q(ps,.9)/1000:3.0f} us | sleep in NtWait{len(ws)/dur:4.0f}/s p50 {q(ws,.5)/1000:3.0f} p90 {q(ws,.9)/1000:4.0f} us')
