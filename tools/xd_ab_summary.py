#!/usr/bin/env python3
"""Paired A/B summary of AbWin lines (debug_ab_frames) in an xe log.

Pairs each variant-1 window with the adjacent variant-0 windows (mean of the
one before and after) so slow thermal drift cancels out.
usage: xd_ab_summary.py <xe-after.log> [skip_windows=4]
"""
import math
import re
import sys

path = sys.argv[1]
skip = int(sys.argv[2]) if len(sys.argv) > 2 else 4
wins = []
for line in open(path, errors='replace'):
    m = re.search(r'AbWin: v=(\d) frames=\d+ interval=([\d.]+)ms cpu=([\d.]+)ms '
                  r'draw=([\d.]+)ms draws=(\d+)', line)
    if m:
        wins.append((int(m.group(1)), float(m.group(2)), float(m.group(3)),
                     float(m.group(4)), float(m.group(5))))
# Gameplay windows only (many draws), after warm-up.
wins = [w for w in wins if w[4] > 2000][skip:]
print(f'{len(wins)} gameplay windows')
names = ['interval', 'cpu', 'draw']
diffs = {n: [] for n in names}
for i in range(1, len(wins) - 1):
    v, *vals, _ = wins[i]
    if v != 1 or wins[i - 1][0] != 0 or wins[i + 1][0] != 0:
        continue
    for k, n in enumerate(names):
        ref = (wins[i - 1][1 + k] + wins[i + 1][1 + k]) / 2
        diffs[n].append((vals[k] - ref, ref))
for n in names:
    d = diffs[n]
    if not d:
        continue
    mean = sum(x for x, _ in d) / len(d)
    ref = sum(r for _, r in d) / len(d)
    sd = math.sqrt(sum((x - mean) ** 2 for x, _ in d) / max(1, len(d) - 1))
    se = sd / math.sqrt(len(d))
    if ref == 0:
        print(f'{n:8s} unavailable (per-draw timers disabled)')
        continue
    print(f'{n:8s} A={ref:6.2f}ms  B-A={mean:+6.2f}ms ({100*mean/ref:+5.1f}%)  '
          f'+-{se:.2f} (n={len(d)})')
a = [w[1] for w in wins if w[0] == 0]
b = [w[1] for w in wins if w[0] == 1]
if a and b:
    print(f'fps A={1000*len(a)/sum(a):.1f} B={1000*len(b)/sum(b):.1f}')
