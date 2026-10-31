#!/usr/bin/env python3
# fasce.py TAG [TAG...] - CPU work per frame AT EQUAL SCENE: each 10 s interval
# (cpu-samples.jsonl samples) is put in the band of its draws per frame, so two
# runs that ended in different scenes are compared only on equivalent intervals.
import json, glob, re, sys
T = '/path/to/xendroid-ultimate-mali/lab/build/multigame-core-audit'
BANDS = [(0, 300), (300, 1000), (1000, 2000), (2000, 3000), (3000, 4500), (4500, 99999)]
rx = re.compile(rb'GuestFps: ([0-9.]+) fps, (\d+) draws/frame')
def intervals(tag):
    d = glob.glob(f'{T}/{tag}/*/')[0]
    s = [json.loads(l) for l in open(d + 'cpu-samples.jsonl') if l.strip()]
    log = open(d + 'session.log', 'rb').read()
    # session.log is the tail of xe.log since startup; gameplay-window.log the tail from the measurement start sample:
    # from there one gets the byte of xe.log where session.log starts.
    import os
    r = json.load(open(d + 'result.json')); r = r.get('games', [r])[0] if 'games' in r else r
    mf = r.get('measure_from_nominal_s', 150)
    at = [x for x in s if x['nominal_s'] == mf]
    base = (at[0]['log_bytes'] - (len(log) - os.path.getsize(d + 'gameplay-window.log'))) if at else s[-1]['log_bytes'] - len(log)
    for x in s: x['log_bytes'] -= base
    out = []
    for a, b in zip(s, s[1:]):
        m = rx.findall(log[a['log_bytes']:b['log_bytes']])
        if not m: continue
        fps = [float(x[0]) for x in m]; dr = [int(x[1]) for x in m]
        wall = b['wall_time'] - a['wall_time']; mf = sum(fps) / len(fps)
        frames = mf * wall
        out.append((b['nominal_s'], sum(dr) / len(dr), mf, (b['cpu_seconds'] - a['cpu_seconds']) * 1000 / frames, (b['cpu_seconds'] - a['cpu_seconds']) / wall, b['temperature'] / 10))
    return out
verbose = '-v' in sys.argv
tags = [t for t in sys.argv[1:] if t != '-v']
res = {}
for t in tags:
    iv = intervals(t); res[t] = iv
    if verbose:
        print(t)
        for n, dr, fps, ms, cores, temp in iv: print('  %4ds draw %5.0f  fps %5.1f  CPU %6.1f ms/frm  %4.2f core  %.1f C' % (n, dr, fps, ms, cores, temp))
print('%-18s' % 'draw band' + ''.join('%-34s' % t[-32:] for t in tags))
for lo, hi in BANDS:
    row = '%5d-%-12d' % (lo, hi)
    for t in tags:
        v = [x for x in res[t] if lo <= x[1] < hi]
        if v:
            row += '%2d int. fps %4.1f CPU %5.1f ms/frm   ' % (len(v), sum(x[2] for x in v) / len(v), sum(x[3] for x in v) / len(v))
        else: row += '%-34s' % '-'
    print(row)
