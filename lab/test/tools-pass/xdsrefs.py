#!/usr/bin/env python3
"""xdsrefs.py SLOT.xds TABLE_ADDR(hex, data array header) : scans every committed page of a savestate for 32-bit
words that look like datum handles (salt<<16 | index) of that Halo data array, and compares their salt with the
table entry. Written 25/10 (Reach restore: is the saved emitter table older than the memory that references it?)."""
import struct, sys, runpy, io, contextlib, os, collections
path, tab = sys.argv[1], int(sys.argv[2], 16)
sys.argv = ['x', path]
with contextlib.redirect_stdout(io.StringIO()):
    g = runpy.run_path(os.path.join(os.path.dirname(__file__), 'xdsheap.py'))
m = g['sections'][2]; o = 8; pages = []
for heap in range(5):
    base, ps, count = struct.unpack_from('<III', m, o); o += 12
    for i in range(count):
        w, = struct.unpack_from('<Q', m, o); o += 8
        if ((w >> 48) & 3) & 2 or heap == 4:
            pages.append((heap, base + i * ps, o, ps)); o += ps
def rd(addr, n):
    phys = addr >= 0xA0000000; a = addr & 0x1FFFFFFF if phys else addr
    for heap, pa, po, ps in pages:
        if (heap == 4) == phys and pa <= a < pa + ps: return m[po + a - pa: po + a - pa + n]
hdr = rd(tab, 0x54)
size, = struct.unpack_from('>I', hdr, 0x20); maxc, = struct.unpack_from('>I', hdr, 0x28); data, = struct.unpack_from('>I', hdr, 0x44)
salt = {}
for i in range(maxc):
    e = rd(data + size * i, 4)
    if e: salt[i] = struct.unpack('>I', e)[0] >> 16
live = {i: s for i, s in salt.items() if s}
lo, hi = min(live.values()), max(live.values())
print(f'table {tab:08X} size {size} max {maxc} live {len(live)} salt range {lo:04X}-{hi:04X}')
ok = collections.Counter(); bad = []
tab_phys = (data & 0x1FFFFFFF, (data & 0x1FFFFFFF) + size * maxc)
for heap, pa, po, ps in pages:
    blk = m[po:po + ps]
    for off in range(0, ps, 4):
        w, = struct.unpack_from('>I', blk, off)
        s, i = w >> 16, w & 0xFFFF
        if not (lo - 0x40 <= s <= hi + 0x40) or i >= maxc: continue
        if heap == 4 and tab_phys[0] <= pa + off < tab_phys[1]: continue  # inside the table itself
        where = f'{"P" if heap == 4 else "V"}{pa + off:08X}'
        if salt.get(i) == s: ok['match'] += 1
        elif salt.get(i) == 0: bad.append((where, w, 'slot FREE in table'))
        elif s > salt.get(i, 0): bad.append((where, w, f'NEWER than table salt {salt[i]:04X}'))
        else: bad.append((where, w, f'older than table salt {salt[i]:04X}'))
print('matching refs', ok['match'], 'non-matching', len(bad))
c = collections.Counter(b[2].split(' ')[0] for b in bad); print(dict(c))
for b in bad[:40]: print(*b)
