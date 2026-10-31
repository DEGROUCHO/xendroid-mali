#!/usr/bin/env python3
"""Summarizes xd_gpuctr samples (1 s each) for gameplay seconds.

Gameplay seconds are those with many input primitives (> prim_min per second).
usage: analyze.py <gpuctr.txt> [prim_min=2000000]
"""
import statistics
import sys

path = sys.argv[1]
prim_min = int(sys.argv[2]) if len(sys.argv) > 2 else 2000000
rows = []
for line in open(path, errors='replace'):
    if not line.startswith('GPUCTR'):
        continue
    d = {}
    for kv in line.split()[1:]:
        k, v = kv.split('=')
        d[k] = float(v) if v != '?' else float('nan')
    rows.append(d)
game = [r for r in rows if r.get('prim', 0) > prim_min]
print(f'{len(rows)} samples, {len(game)} gameplay (prim > {prim_min})')
if not game:
    sys.exit()
def m(k):
    return statistics.median(r.get(k, 0.0) for r in game)
gpu = m('gpu')
print(f'GPU active cycles/s: {gpu/1e6:.0f} M (clock while active)')
for k in ['binQ', 'mainQ', 'compQ', 'tiler', 'coreBin', 'coreMain']:
    print(f'  {k:9s} {m(k)/gpu*100:5.1f}% of GPU active')
print(f'  core (sum over 7 cores) {m("core")/gpu/7*100:5.1f}% per core')
print(f'Primitives/s: in {m("prim")/1e6:.2f} M, visible {m("vis")/1e6:.2f} M, '
      f'culled {m("cull")/1e6:.2f} M, DVS {m("dvs")/1e6:.2f} M, rasterized {m("rastPrim")/1e6:.2f} M')
print(f'Position shader threads/s {m("posThr")/1e6:.2f} M, varying {m("varThr")/1e6:.2f} M, '
      f'non-frag threads {m("nfThr")/1e6:.2f} M, frag threads {m("fThr")/1e6:.1f} M')
print(f'Tiles/s {m("tiles")/1e3:.0f} k (killed unchanged {m("tileKill")/1e3:.0f} k), '
      f'rasterized quads/s {m("rastQd")/1e6:.1f} M')
print(f'External bus: read {m("extRd")/1e9:.2f} GB/s, write {m("extWr")/1e9:.2f} GB/s; '
      f'tiler read {m("tilerRd")/1e6:.0f} MB/s, write {m("tilerWr")/1e6:.0f} MB/s')
print(f'Stalls: position shading {m("posStall")/gpu*100:.1f}%, prim assembly waiting on pos {m("primAsmStall")/gpu*100:.1f}%')
print(f'Quads/s: rasterized {m("rastQd")/1e6:.0f} M, shaded {m("shadedQd")/1e6:.0f} M, early-ZS tested {m("ezsTest")/1e6:.0f} M '
      f'killed {m("ezsKill")/1e6:.0f} M, late-ZS tested {m("lzsTest")/1e6:.0f} M killed {m("lzsKill")/1e6:.0f} M, '
      f'FPK killed {m("fpkKill")/1e6:.0f} M, opaque {m("opaqueQd")/1e6:.0f} M, transparent {m("transpQd")/1e6:.0f} M')
