#!/usr/bin/env python3
"""xtrsnap.py file.xtr BASE_HEX SIZE out.bin - ricostruisce [BASE, BASE+SIZE) usando SOLO le letture che precedono il
primo pacchetto (lo snapshot iniziale: copia GPU delle pagine 'scritte dalla GPU'), escluse quelle di lunghezza esatta SIZE
a BASE (le letture RAM delle texture legate all'inizio). Stampa quanta parte dell'intervallo e' coperta."""
import struct, sys
exec(open(__file__.rsplit('/', 1)[0] + '/xtrmem.py').read().split("d = open(sys.argv[1]")[0])
path, base, size, out = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3]), sys.argv[4]
d = open(path, 'rb').read(); p = 48; pk = 0; buf = bytearray(size); cov = bytearray(size)
while p < len(d) and pk == 0:
    t = struct.unpack_from('<I', d, p)[0]
    if t in (0, 2): p += 12 + struct.unpack_from('<I', d, p + 8)[0] * 4
    elif t in (1, 3, 5): p += 4
    elif t == 4: pk += 1
    elif t in (6, 7):
        b, enc, el, dl = struct.unpack_from('<IIII', d, p + 4); raw = d[p + 20:p + 20 + el]; p += 20 + el
        if t == 6 and b < base + size and b + dl > base and not (b == base and dl == size):
            data = unsnappy(raw) if enc == 1 else raw
            lo = max(b, base); hi = min(b + dl, base + size)
            buf[lo - base:hi - base] = data[lo - b:hi - b]; cov[lo - base:hi - base] = b'\x01' * (hi - lo)
    elif t == 8: p += 12 + struct.unpack_from('<I', d, p + 8)[0]
    elif t == 9: p += 8
    elif t == 10: p += 24 + struct.unpack_from('<I', d, p + 20)[0]
    elif t == 11: p += 16 + struct.unpack_from('<I', d, p + 12)[0]
print('coperto dallo snapshot: %.0f%%' % (100 * sum(cov) / size))
open(out, 'wb').write(buf)
