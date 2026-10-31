#!/usr/bin/env python3
"""Aggregates per-thread CPU usage from `top -H -b` samples (TOPSAMPLE blocks).

Only samples where the busiest thread is above min_cpu (gameplay) are used.
usage: xd_top_threads.py <top.txt> [min_cpu=50]
"""
import collections
import sys

path = sys.argv[1]
min_cpu = float(sys.argv[2]) if len(sys.argv) > 2 else 50.0
samples = []
cur = None
for line in open(path, errors='replace'):
    if line.startswith('TOPSAMPLE'):
        cur = {}
        samples.append(cur)
        continue
    p = line.split()
    if cur is None or len(p) < 12 or not p[0].isdigit():
        continue
    try:
        cpu = float(p[8])
    except ValueError:
        continue
    name = ' '.join(p[11:-1]) if len(p) > 12 else p[11]
    cur[f'{name}'] = cur.get(name, 0.0) + cpu
game = [s for s in samples if s and max(s.values()) >= min_cpu]
print(f'{len(samples)} samples, {len(game)} with a thread >= {min_cpu}%')
agg = collections.defaultdict(list)
for s in game:
    for k, v in s.items():
        agg[k].append(v)
rows = []
for k, v in agg.items():
    v = sorted(v + [0.0] * (len(game) - len(v)))
    rows.append((v[len(v) // 2], v[int(len(v) * 0.9)], k))
for med, p90, k in sorted(rows, reverse=True)[:16]:
    print(f'{k[:28]:28s} median {med:6.1f}%  p90 {p90:6.1f}%')
tot = [sum(s.values()) for s in game]
if tot:
    tot.sort()
    print(f'total process CPU median {tot[len(tot) // 2]:.0f}%')
