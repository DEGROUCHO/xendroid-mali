#!/usr/bin/env python3
"""Clock-independent GPU work per frame from a pkg_run game directory.

usage: perframe.py <game dir> [...]

gpuctr.txt starts 60 s into the run with one sample per second; the GuestFps
timeline in result.json has about one value per second from the start. Each
counter per second divided by the fps of the same second gives work per frame
(fragment threads, shaded quads, tiles, external bytes) - comparable across runs
even when the Mali thermal cap changes the clock. Medians over seconds with
10-50 fps (gameplay; menus at 60 or loading excluded). FROM=<s> TO=<s> in the
environment restrict the window to seconds of the run (screenshots show when the
gameplay starts; in-engine cutscenes also run at 10-50 fps).
"""
import json
import os
import re
import statistics
import sys

for d in sys.argv[1:]:
    r = json.load(open(d + '/result.json'))
    fps = r['fps_timeline']
    rows = []
    for k, line in enumerate(l for l in open(d + '/gpuctr.txt', errors='replace') if l.startswith('GPUCTR')):
        f = dict(m.groups() for m in re.finditer(r'(\w+)=(-?\d+)', line))
        t = 60 + k
        if t >= len(fps) or not (10 <= fps[t] <= 50) or int(f.get('gpu', 0)) <= 0:
            continue
        if not (int(os.environ.get('FROM', 0)) <= t <= int(os.environ.get('TO', 10**9))):
            continue
        p = fps[t]
        rows.append({
            'gcyc': int(f['gpu']) / p / 1e6,
            'fThr': int(f.get('fThr', 0)) / p / 1e6,
            'shadedQd': int(f.get('shadedQd', 0)) / p / 1e6,
            'tiles': int(f.get('tiles', 0)) / p / 1e3,
            'extMB': (int(f.get('extRd', 0)) + int(f.get('extWr', 0))) / p / 1e6,
            'fps': p,
        })
    if not rows:
        print(d, 'no gameplay samples')
        continue
    med = {k: statistics.median(x[k] for x in rows) for k in rows[0]}
    print(f"{d.split('/')[-2][:14]:14s} n={len(rows):3d} fps {med['fps']:5.1f}  GPU Mcyc/fr {med['gcyc']:5.1f}  "
          f"frag thr M/fr {med['fThr']:5.1f}  shaded quads M/fr {med['shadedQd']:5.2f}  tiles k/fr {med['tiles']:5.1f}  ext MB/fr {med['extMB']:5.0f}")
