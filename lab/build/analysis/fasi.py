#!/usr/bin/env python3
# fasi.py <run folder> <total seconds> <live_from> <live_step>
# Medians per live phase of: guest FPS, GPU cycles/s, faded quads, quads killed by the late test, external bandwidth.
import sys, re, glob, statistics as st
run, total, start, step = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
d = glob.glob(run + '/*/')[0]
import json, os
if os.path.exists(d + 'result.json'):
    try: total = float(json.load(open(d + 'result.json')).get('seconds', total))   # run stopped early (thermal limit)
    except Exception: pass
phases = []
for l in open(d + 'live_phases.txt'):
    p = l.split()
    if len(p) == 2 and not l.startswith('#'): phases.append((float(p[0]), p[1]))
ctr = [dict((k, int(v)) for k, v in re.findall(r'(\w+)=(-?\d+)', l)) for l in open(d + 'gpuctr.txt', errors='replace') if l.startswith('GPUCTR')]
# the last sample roughly coincides with the end of the run; one sample per second
ctr_t = [total - (len(ctr) - 1 - i) for i in range(len(ctr))]
fps = []  # (estimated time, fps)
rep = [(int(m.group(1)), float(m.group(2))) for m in re.finditer(r'f:(\d+) \S+ GuestFps: ([\d.]+) fps', open(d + 'session.log', errors='replace').read())]
t = total
for i in range(len(rep) - 1, 0, -1):
    fps.append((t, rep[i][1])); df = rep[i][0] - rep[i - 1][0]
    t -= df / max(rep[i][1], 1.0)
def med(v): return st.median(v) if v else float('nan')
print('%-22s %7s %9s %9s %9s %9s %9s' % ('phase','FPS', 'GPU Mc/f', 'quad M/f', 'lzsK M/f', 'rd MB/f', 'wr MB/f'))
phases = [p for p in phases if p[0] < total - 6]
for i, (t0, name) in enumerate(phases):
    t1 = phases[i + 1][0] if i + 1 < len(phases) else total
    a, b = t0 + 5, t1 - 1          # discard the seconds around the switch
    f = [x for tt, x in fps if a <= tt <= b]
    c = [x for tt, x in zip(ctr_t, ctr) if a <= tt <= b]
    fp = med(f)
    per = lambda k: med([x.get(k, 0) for x in c]) / 1e6 / fp if c and fp == fp and fp > 0 else float('nan')
    print('%-22s %7.1f %9.1f %9.2f %9.2f %9.0f %9.0f' % (name, fp, per('gpu'), per('shadedQd'), per('lzsKill'), per('extRd'), per('extWr')))
