#!/usr/bin/env python3
"""abab.py PREFIX_A PREFIX_B [NIGHT.tsv] : verdict of an ABAB from NIGHT.tsv rows
'TAG fps F cores C gpu G bw B' -> Mcycles/frame (G*1000/F) and MB/frame (B*1000/F), means and delta."""
import sys, re, statistics
a, b = sys.argv[1], sys.argv[2]
f = sys.argv[3] if len(sys.argv) > 3 else '/path/to/xendroid-ultimate-mali/lab/test/NIGHT.tsv'
rows = {a: [], b: []}
for l in open(f):
    m = re.match(r'(\S+) fps ([0-9.]+) cores ([0-9.]+) gpu ([0-9.]+) bw ([0-9.]+)', l)
    if not m: continue
    tag = m.group(1)
    for p in (b, a):  # longer prefix first
        if tag.startswith(p + '-') or tag == p:
            fps, gpu, bw = float(m.group(2)), float(m.group(4)), float(m.group(5))
            rows[p].append((gpu * 1000 / fps, bw * 1000 / fps, fps)); break
for p in (a, b):
    r = rows[p]
    print(f'{p:12} n={len(r)} Mcyc/frame ' + ' '.join(f'{x[0]:.2f}' for x in r) + f'  MB/frame ' + ' '.join(f'{x[1]:.0f}' for x in r))
if rows[a] and rows[b]:
    ca, cb = statistics.mean(x[0] for x in rows[a]), statistics.mean(x[0] for x in rows[b])
    ba, bb = statistics.mean(x[1] for x in rows[a]), statistics.mean(x[1] for x in rows[b])
    print(f'cycles {ca:.2f} -> {cb:.2f} ({(cb/ca-1)*100:+.1f}%)   bandwidth {ba:.0f} -> {bb:.0f} MB/frame ({(bb/ba-1)*100:+.1f}%)')
