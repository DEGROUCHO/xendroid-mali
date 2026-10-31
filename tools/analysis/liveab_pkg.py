#!/usr/bin/env python3
"""Per-arm GPU work per frame of a pkg_run live A/B (XD_LIVE_AB) game directory.

usage: liveab_pkg.py <game dir> [settle seconds, default 2]

Alignment by frame numbers, not by the fps timeline (which starts when the game
starts logging, seconds after the launch): live_phases.txt has the push time of
each switch (seconds since launch) and the session log has the guest frame at
which the emulator applied it ("LiveCvar: frame=N"). A phase spans from one
applied switch to the next; its frames and seconds give its fps, and the
xd_gpuctr samples (one per second from 60 s after launch) inside it, minus
`settle` seconds after the switch, give its counters per frame.
"""
import re
import statistics
import sys

d = sys.argv[1].rstrip('/')
settle = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
pushes = []
for line in open(d + '/live_phases.txt'):
    if line.strip() and not line.startswith('#'):
        t, name = line.split()
        pushes.append((float(t), name))
applied = []
for line in open(d + '/session.log', errors='replace'):
    m = re.search(r'LiveCvar: frame=(\d+) applied:(.*)', line)
    if m:
        applied.append((int(m[1]), m[2]))
# Each push is applied once (a duplicate re-apply of the same content follows
# the touch after the move - keep the first of identical consecutive ones).
dedup = []
for f, a in applied:
    if not dedup or dedup[-1][1] != a:
        dedup.append((f, a))
n = min(len(pushes), len(dedup))
if len(pushes) != len(dedup):
    print(f"note: {len(pushes)} pushes, {len(dedup)} applied changes - using the first {n}")
APPLY_LATENCY = 0.7  # seconds from the push to the frame the change is applied at
ctr = [dict(m.groups() for m in re.finditer(r'(\w+)=(-?\d+)', l))
       for l in open(d + '/gpuctr.txt', errors='replace') if l.startswith('GPUCTR')]


def ctr_sum(t0, t1):
    # Samples k cover [60 + k, 61 + k) seconds after the launch.
    tot = {}
    secs = 0
    for k, c in enumerate(ctr):
        s = 60 + k
        if s >= t0 and s + 1 <= t1:
            secs += 1
            for key in ('gpu', 'fThr', 'shadedQd', 'tiles', 'extRd', 'extWr'):
                tot[key] = tot.get(key, 0) + int(c.get(key, 0))
    return tot, secs


phases = []
for i in range(n - 1):
    t0 = pushes[i][0] + APPLY_LATENCY
    t1 = pushes[i + 1][0] + APPLY_LATENCY
    f0, f1 = dedup[i][0], dedup[i + 1][0]
    if f1 <= f0 or t1 <= t0:
        continue
    fps = (f1 - f0) / (t1 - t0)
    tot, secs = ctr_sum(t0 + settle, t1)
    if secs < 3 or not tot.get('gpu'):
        continue
    frames = fps * secs
    phases.append((pushes[i][1], {
        'fps': fps, 'gpu': tot['gpu'] / frames / 1e6, 'fThr': tot['fThr'] / frames / 1e6,
        'shadedQd': tot['shadedQd'] / frames / 1e6, 'tiles': tot['tiles'] / frames / 1e3,
        'ext': (tot['extRd'] + tot['extWr']) / frames / 1e6}, t0))
KEYS = ('fps', 'gpu', 'fThr', 'shadedQd', 'tiles', 'ext')
for name, m, t0 in phases:
    print(f"  t={t0:6.1f} {name:22s} " + ' '.join(f"{k} {m[k]:7.1f}" for k in KEYS))
arms = sorted({p[0] for p in phases})
print(f"{'arm':22s} phases  fps   GPU Mcyc/fr  fThr M/fr  shadedQd M/fr  tiles k/fr  ext MB/fr")
for a in arms:
    rows = [m for nm, m, _ in phases if nm == a]
    med = {k: statistics.median(r[k] for r in rows) for k in KEYS}
    print(f"{a:22s} {len(rows):5d}  {med['fps']:5.1f}  {med['gpu']:10.1f}  {med['fThr']:9.1f}  {med['shadedQd']:13.2f}  {med['tiles']:10.1f}  {med['ext']:9.0f}")
if len(arms) == 2:
    a, b = arms
    diffs = {k: [] for k in KEYS}
    for (n1, m1, _), (n2, m2, _) in zip(phases, phases[1:]):
        if n1 != n2:
            x, y = (m1, m2) if n1 == a else (m2, m1)
            for k in KEYS:
                diffs[k].append((y[k] - x[k]) / x[k] * 100 if x[k] else 0)
    print(f"paired change {b} vs {a} (median of {len(diffs['fps'])} neighbour pairs): " +
          ", ".join(f"{k} {statistics.median(v):+.1f}%" for k, v in diffs.items() if v))
