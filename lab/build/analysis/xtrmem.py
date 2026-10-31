#!/usr/bin/env python3
"""xtrmem.py file.xtr - confronta le letture di memoria durante il frame con lo snapshot iniziale
(stessi indirizzi): dice se la RAM guest registrata coincide con la copia GPU scaricata all'inizio."""
import struct, sys
def unsnappy(b):
    n = 0; s = 0; p = 0
    while True:
        c = b[p]; p += 1; n |= (c & 0x7F) << s; s += 7
        if c < 128: break
    out = bytearray()
    while p < len(b):
        t = b[p]; p += 1; k = t & 3
        if k == 0:
            l = t >> 2
            if l >= 60:
                nb = l - 59; l = int.from_bytes(b[p:p + nb], 'little'); p += nb
            l += 1; out += b[p:p + l]; p += l
        else:
            if k == 1: l = ((t >> 2) & 7) + 4; off = ((t >> 5) << 8) | b[p]; p += 1
            elif k == 2: l = (t >> 2) + 1; off = b[p] | (b[p + 1] << 8); p += 2
            else: l = (t >> 2) + 1; off = int.from_bytes(b[p:p + 4], 'little'); p += 4
            st = len(out) - off
            if off >= l: out += out[st:st + l]
            else:
                for i in range(l): out.append(out[st + i])
    assert len(out) == n, (len(out), n); return bytes(out)
d = open(sys.argv[1], 'rb').read(); p = 48; pk = 0; pre = []; res = {'uguali': 0, 'diverse': 0, 'fuori': 0}; ex = []
while p < len(d):
    t = struct.unpack_from('<I', d, p)[0]
    if t in (0, 2): p += 12 + struct.unpack_from('<I', d, p + 8)[0] * 4
    elif t in (1, 3, 5): p += 4
    elif t == 4: pk += 1; p += 12 + struct.unpack_from('<I', d, p + 8)[0] * 4
    elif t in (6, 7):
        base, enc, el, dl = struct.unpack_from('<IIII', d, p + 4); raw = d[p + 20:p + 20 + el]; p += 20 + el
        if t == 6:
            data = unsnappy(raw) if enc == 1 else raw
            if pk == 0: pre.append((base, data))
            elif dl >= 4096:
                hit = next(((b, x) for b, x in pre if b <= base and base + dl <= b + len(x)), None)
                if not hit: res['fuori'] += 1
                else:
                    same = hit[1][base - hit[0]:base - hit[0] + dl] == data
                    res['uguali' if same else 'diverse'] += 1
                    if not same and len(ex) < 6:
                        a = hit[1][base - hit[0]:base - hit[0] + 16].hex(); ex.append((hex(base), dl, a, data[:16].hex()))
    elif t == 8: p += 12 + struct.unpack_from('<I', d, p + 8)[0]
    elif t == 9: p += 8
    elif t == 10: p += 24 + struct.unpack_from('<I', d, p + 20)[0]
    elif t == 11: p += 16 + struct.unpack_from('<I', d, p + 12)[0]
print('letture >=4K durante il frame rispetto allo snapshot iniziale:', res)
for e in ex: print(e)
