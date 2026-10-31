#!/usr/bin/env python3
# fasi2.py <run folder> [initial_skip_s=4]
# Pairwise comparison for still-scene runs with variants alternated hot: for each non-base phase it computes the
# % difference vs the average of the two adjacent base phases; then median and range per variant.
# The "c-base2" phases are an A/A control: their difference from the nearby bases is the measurement noise.
import sys, re, glob, json, statistics as st
run = sys.argv[1]; skip = float(sys.argv[2]) if len(sys.argv) > 2 else 4
d = glob.glob(run + '/*/')[0]
# the runner's "nominal" seconds are longer than the real ones: the true end is derived from the CPU samples
cs = [json.loads(l) for l in open(d + 'cpu-samples.jsonl')]
total = cs[-1]['wall_time'] - (cs[0]['wall_time'] - cs[0]['nominal_s'])
phases = [(float(l.split()[0]), l.split()[1].replace('.toml', '')) for l in open(d + 'live_phases.txt') if len(l.split()) == 2 and not l.startswith('#')]
phases = [p for p in phases if p[0] < total - 6]
ctr = [dict((k, int(v)) for k, v in re.findall(r'(\w+)=(-?\d+)', l)) for l in open(d + 'gpuctr.txt', errors='replace') if l.startswith('GPUCTR')]
ctr_t = [total - (len(ctr) - 1 - i) for i in range(len(ctr))]
rep = [(int(m.group(1)), float(m.group(2))) for m in re.finditer(r'f:(\d+) \S+ GuestFps: ([\d.]+) fps', open(d + 'session.log', errors='replace').read())]
fps = []; t = total
for i in range(len(rep) - 1, 0, -1):
    fps.append((t, rep[i][1])); t -= (rep[i][0] - rep[i - 1][0]) / max(rep[i][1], 1.0)
cpu = []
try:
    for l in open(d + 'cpu-samples.jsonl'):
        j = json.loads(l); cpu.append((j.get('t', 0), j.get('cores', j.get('cpu_cores', 0))))
except Exception: pass
K = [('FPS', None), ('GPU Mc/f', 'gpu'), ('GPU Mc/s', 'gpu'), ('quad M/f', 'shadedQd'), ('lzsK M/f', 'lzsKill'), ('rd MB/f', 'extRd'), ('wr MB/f', 'extWr')]
rows = []
for i, (t0, name) in enumerate(phases):
    t1 = phases[i + 1][0] if i + 1 < len(phases) else total
    a, b = t0 + skip, t1 - 0.5
    f = [x for tt, x in fps if a <= tt <= b]; c = [x for tt, x in zip(ctr_t, ctr) if a <= tt <= b]
    if not f or not c: continue
    fp = st.median(f); m = lambda k: st.median([x.get(k, 0) for x in c]) / 1e6
    rows.append((name, [fp, m('gpu') / fp, m('gpu'), m('shadedQd') / fp, m('lzsKill') / fp, m('extRd') / fp, m('extWr') / fp]))
print('%-12s' % 'phase' +''.join('%10s' % k for k, _ in K))
for name, v in rows: print('%-12s' % name + ''.join('%10.1f' % x for x in v))
print('\nDifference %% vs the adjacent base phases (median [min..max], n):')
print('%-12s' % 'variant' +''.join('%22s' % k for k, _ in K[:5]))
var = {}
for i, (name, v) in enumerate(rows):
    if name == 'c-base': continue
    nb = [rows[j][1] for j in (i - 1, i + 1) if 0 <= j < len(rows) and rows[j][0] == 'c-base']
    if not nb: continue
    base = [sum(x[k] for x in nb) / len(nb) for k in range(len(v))]
    var.setdefault(name, []).append([100 * (v[k] / base[k] - 1) if base[k] else 0 for k in range(len(v))])
for name, ds in var.items():
    print('%-12s' % name + ''.join('%22s' % ('%+.1f [%+.0f..%+.0f] n=%d' % (st.median([x[k] for x in ds]), min(x[k] for x in ds), max(x[k] for x in ds), len(ds))) for k in range(5)))
