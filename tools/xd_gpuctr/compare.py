#!/usr/bin/env python3
"""Compares xd_gpuctr captures (1 s samples) side by side, medians per second.

usage: compare.py <label=gpuctr.txt[@fps]> [...]
Skips the first 2 samples of each capture (startup). With @fps on every
capture, a second table prints the same medians per frame.
"""
import statistics
import sys

KEYS = [
    ('gpu', 'GPU active Mcyc/s', 1e6), ('core', 'core active sum Mcyc/s', 1e6),
    ('fThr', 'frag threads M/s', 1e6), ('nfThr', 'non-frag threads M/s', 1e6),
    ('rastQd', 'quads rasterized M/s', 1e6), ('shadedQd', 'quads shaded M/s', 1e6),
    ('opaqueQd', 'opaque quads M/s', 1e6), ('transpQd', 'transparent quads M/s', 1e6),
    ('fpkKill', 'FPK killed quads M/s', 1e6),
    ('ezsTest', 'early ZS test M/s', 1e6), ('ezsKill', 'early ZS kill M/s', 1e6),
    ('lzsTest', 'late ZS test M/s', 1e6), ('lzsKill', 'late ZS kill M/s', 1e6),
    ('extRd', 'DRAM read GB/s', 1e9), ('extWr', 'DRAM write GB/s', 1e9),
    ('tiles', 'tiles k/s', 1e3), ('tileKill', 'tiles killed k/s', 1e3),
    ('prim', 'input prims k/s', 1e3),
    ('aluIss', 'ALU issue Mcyc/s', 1e6), ('fmaI', 'FMA instr M/s', 1e6),
    ('cvtI', 'CVT instr M/s', 1e6), ('sfuI', 'SFU instr M/s', 1e6),
    ('arithI', 'arith instr M/s', 1e6), ('narrowI', 'FP16 instr M/s', 1e6),
    ('texIss', 'tex issue Mcyc/s', 1e6), ('texFilt', 'tex filter Mcyc/s', 1e6),
    ('lsIss', 'load/store Mcyc/s', 1e6), ('varIss', 'varying Mcyc/s', 1e6),
    ('starve', 'issue starve Mcyc/s', 1e6),
    ('texExtRd', 'tex DRAM read GB/s', 1e9), ('lsExtRd', 'LS DRAM read GB/s', 1e9),
    ('lsWr', 'LS write GB/s', 1e9),
]
caps = []
for arg in sys.argv[1:]:
    label, path = arg.split('=', 1)
    fps = None
    if '@' in path:
        path, fps = path.rsplit('@', 1)
        fps = float(fps)
    rows = []
    for line in open(path, errors='replace'):
        if line.startswith('GPUCTR'):
            rows.append({k: float(v) for k, v in
                         (kv.split('=') for kv in line.split()[1:])})
    caps.append((label, rows[2:], fps))
def table(per_frame):
    print(f"{'counter' + (' (per frame)' if per_frame else ''):26}" +
          ''.join(f'{l:>14}' for l, _, _ in caps))
    for key, name, div in KEYS:
        if not any(rows and key in rows[0] for _, rows, _ in caps):
            continue
        vals = []
        for _, rows, fps in caps:
            v = (statistics.median(r.get(key, 0.0) for r in rows) / div
                 if rows else float('nan'))
            vals.append(v / fps if per_frame else v)
        if per_frame:
            name = name.replace('/s', '/fr')
        print(f'{name:26}' + ''.join(f'{v:14.3f}' for v in vals))


table(False)
if caps and all(fps for _, _, fps in caps):
    print()
    table(True)
