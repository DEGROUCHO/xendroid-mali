#!/usr/bin/env python3
"""xdsdiff.py A.xds B.xds [TOP=30]: page-by-page diff of the guest memory of two XenDroid savestates (format 2),
grouped by 1 MiB: pages whose state (free/reserved/committed) or contents differ. Written 25/10 to check that a
restore reproduces the guest: save right after a restore (run-restore-only XD_SAVE_AFTER) and diff with the slot."""
import struct, sys, runpy, io, contextlib, os, hashlib, collections
def load(path):
    sys.argv = ['x', path]
    with contextlib.redirect_stdout(io.StringIO()):
        g = runpy.run_path(os.path.join(os.path.dirname(__file__), 'xdsheap.py'))
    m = g['sections'][2]; o = 8; heaps = []
    for heap in range(5):
        base, ps, count = struct.unpack_from('<III', m, o); o += 12
        pages = {}
        for i in range(count):
            w, = struct.unpack_from('<Q', m, o); o += 8
            st = (w >> 48) & 3; h = None
            if (st & 2) or heap == 4:
                h = hashlib.blake2b(m[o:o + ps], digest_size=8).digest(); o += ps
            pages[i] = (st, h)
        heaps.append((base, ps, pages))
    return heaps
ARGS = sys.argv[1:]
a, b = load(ARGS[0]), load(ARGS[1]); top = int(ARGS[2]) if len(ARGS) > 2 else 30
for (base, ps, pa), (_, _, pb) in zip(a, b):
    chunks = collections.Counter(); kinds = collections.Counter()
    for i in set(pa) | set(pb):
        x, y = pa.get(i), pb.get(i)
        if x == y: continue
        kind = 'state' if (x or (0, 0))[0] != (y or (0, 0))[0] else 'contents'
        kinds[kind] += 1; chunks[(base + i * ps) >> 20] += 1
    print(f'heap {base:08X} page {ps:X}: {kinds["contents"]} pages differ in contents, {kinds["state"]} in state')
    for mb, n in sorted(chunks.items(), key=lambda kv: -kv[1])[:top]:
        print(f'   {mb << 20:08X}  {n} pages')
