#!/usr/bin/env python3
"""tempi_carica.py <run folder> ...  - per run: sequence of phases seen from the GuestFps lines
(about one per second): seconds spent in 'light screen' (< 300 draws: menu, loading, black) and in
'scene' (>= 1000 draws), and the second where the scene that lasts until the end starts (entering the match).
Used to see whether a change lengthens the loading times."""
import sys, re, glob
for run in sys.argv[1:]:
    f = glob.glob(run.rstrip('/') + '/*/session.log')
    if not f: print(run, 'no session.log'); continue
    v = []
    for l in open(f[0], errors='replace'):
        m = re.search(r'GuestFps: ([0-9.]+) fps, (\d+) draws', l)
        if m: v.append((float(m[1]), int(m[2])))
    if not v: print(run, 'no GuestFps line'); continue
    # run-length encode by class
    def cls(d): return 'L' if d < 300 else ('S' if d >= 1000 else 'm')
    runs = []
    for fps, d in v:
        c = cls(d)
        if runs and runs[-1][0] == c: runs[-1][1] += 1
        else: runs.append([c, 1])
    # last sustained scene start
    t = 0; last_scene_start = None
    for c, n in runs:
        if c == 'S' and n >= 10: last_scene_start = t
        t += n
    seq = ' '.join(f'{c}{n}' for c, n in runs if n >= 3)
    print(f"{run.rstrip('/').split('/')[-1]:30s} lines {len(v):3d} | last long scene from second ~{last_scene_start} | phases: {seq[:150]}")
