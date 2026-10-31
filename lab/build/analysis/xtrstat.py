#!/usr/bin/env python3
"""xtrstat.py file.xtr - riepilogo di una traccia GPU: comandi, opcode PM4 type-3, memoria."""
import struct, sys, collections
d = open(sys.argv[1], 'rb').read(); p = 48
cmds = collections.Counter(); ops = collections.Counter(); mem = 0; memn = 0; first_pkts = []
N = ['PrimStart','PrimEnd','IndStart','IndEnd','PktStart','PktEnd','MemRead','MemWrite','Edram','Event','Regs','Gamma']
while p < len(d):
    t = struct.unpack_from('<I', d, p)[0]; cmds[N[t]] += 1
    if t in (0, 2): p += 12 + struct.unpack_from('<I', d, p + 8)[0] * 4
    elif t in (1, 3, 5): p += 4
    elif t == 4:
        base, cnt = struct.unpack_from('<II', d, p + 4); p += 12
        if cnt:
            w = struct.unpack_from('>I', d, p)[0]
            typ = w >> 30
            ops[(typ, (w >> 8) & 0x7F if typ == 3 else None)] += 1
            if len(first_pkts) < 12: first_pkts.append((typ, hex(w), cnt))
        p += cnt * 4
    elif t in (6, 7):
        base, enc, el, dl = struct.unpack_from('<IIII', d, p + 4); p += 20 + el
        if t == 6: mem += dl; memn += 1
    elif t == 8: p += 12 + struct.unpack_from('<I', d, p + 8)[0]
    elif t == 9: p += 8
    elif t == 10: p += 24 + struct.unpack_from('<I', d, p + 20)[0]
    elif t == 11: p += 16 + struct.unpack_from('<I', d, p + 12)[0]
    else: raise SystemExit(f'bad cmd {t} at {p}')
print(dict(cmds)); print('mem reads', memn, mem // 2**20, 'MiB decoded')
for (typ, op), n in sorted(ops.items(), key=lambda x: -x[1]): print(f'type{typ} op={"" if op is None else hex(op)} x{n}')
print(first_pkts)
