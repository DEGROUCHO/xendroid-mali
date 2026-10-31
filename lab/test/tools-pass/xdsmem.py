#!/usr/bin/env python3
"""xdsmem.py SLOT.xds ADDR_HEX LEN : dump guest bytes of a XenDroid savestate (format 2, committed pages of the
virtual heaps; 0xA/0xC/0xE addresses read the physical heap) as hex, big-endian guest order. Uses xdsheap.py's
section parser. Written 25/10 (Reach post-restore hang); 25/10 10:30 reads page by page (a read across a page
boundary used to include the next page's 8-byte state word)."""
import struct, sys, runpy, io, contextlib, os
path, addr, ln = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3])
sys.argv = ['x', path]
with contextlib.redirect_stdout(io.StringIO()):
    g = runpy.run_path(os.path.join(os.path.dirname(__file__), 'xdsheap.py'))
m = g['sections'][2]; o = 8
pages = {}  # (heap, page address) -> offset of its data in the memory section
for heap in range(5):
    base, ps, count = struct.unpack_from('<III', m, o); o += 12
    for i in range(count):
        w, = struct.unpack_from('<Q', m, o); o += 8
        st = (w >> 48) & 3
        if (st & 2) or heap == 4:
            pages[(heap, base + i * ps)] = (o, ps)
            o += ps
phys = addr >= 0xA0000000
out = b''
a = (addr & 0x1FFFFFFF) if phys else addr
while len(out) < ln:
    hit = None
    for (heap, pa), (po, ps) in pages.items():
        if (heap == 4) == phys and pa <= a < pa + ps:
            hit = (po + a - pa, pa + ps - a); break
    if not hit:
        sys.exit(f'address {a:08X} not in a committed page')
    n = min(hit[1], ln - len(out))
    out += m[hit[0]:hit[0] + n]; a += n
print(out.hex())
