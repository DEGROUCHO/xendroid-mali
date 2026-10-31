#!/usr/bin/env python3
"""xtrpass.py file.xtr - per-draw EDRAM state from a GPU trace: resolves (with clear bits), RT config changes."""
import struct, sys, collections
d = open(sys.argv[1], 'rb').read(); p = 48
R = [0] * 0x10000
def wr(i, v):
    if 0 <= i < 0x10000: R[i] = v
ev = []   # (kind, info)
def draw():
    mode = R[0x2208] & 7
    surf, ci, di = R[0x2000], R[0x2001], R[0x2002]
    if mode == 6:
        cc = R[0x2318]
        ev.append(('resolve', dict(src=cc & 7, cclr=(cc >> 8) & 1, dclr=(cc >> 9) & 1, cmd=(cc >> 20) & 3,
                  pitch=surf & 0x3FFF, msaa=(surf >> 16) & 3, cbase=ci & 0xFFF, cfmt=(ci >> 16) & 0xF, dbase=di & 0xFFF,
                  dest=R[0x2319], dinfo=R[0x231B], br=R[0x2082])))
    else:
        cm = R[0x2104]; dc = R[0x2200]
        dep = (dc & 1) or ((dc >> 1) & 1)
        cols = tuple((R[0x2001 + (0 if k == 0 else k + 1)] & 0xFFF, (R[0x2001 + (0 if k == 0 else k + 1)] >> 16) & 0xF) if (cm >> (4 * k)) & 0xF else None for k in range(4))
        ev.append(('draw', (mode, surf & 0x3FFF, (surf >> 16) & 3, cols, (di & 0xFFF) if dep else None, (dc >> 2) & 1)))
while p < len(d):
    t = struct.unpack_from('<I', d, p)[0]
    if t in (0, 2): p += 12 + struct.unpack_from('<I', d, p + 8)[0] * 4
    elif t in (1, 3, 5): p += 4
    elif t == 4:
        base, cnt = struct.unpack_from('<II', d, p + 4); p += 12
        w = [struct.unpack_from('>I', d, p + 4 * k)[0] for k in range(cnt)]; p += cnt * 4
        if not w: continue
        h = w[0]; typ = h >> 30
        if typ == 0:
            bi = h & 0x7FFF; n = ((h >> 16) & 0x3FFF) + 1; one = (h >> 15) & 1
            for k in range(min(n, len(w) - 1)): wr(bi if one else bi + k, w[1 + k])
        elif typ == 1:
            wr(h & 0x7FF, w[1]); wr((h >> 11) & 0x7FF, w[2])
        elif typ == 3:
            op = (h >> 8) & 0x7F
            if op == 0x2D and len(w) > 2:  # SET_CONSTANT
                ot = (w[1] >> 16) & 0xFF; idx = w[1] & 0x7FF
                off = {0: 0x4000, 1: 0x4800, 2: 0x4900, 3: 0x4908, 4: 0x2000}.get(ot)
                if off is not None:
                    for k, v in enumerate(w[2:]): wr(off + idx + k, v)
            elif op in (0x22, 0x36):
                draw()
    elif t in (6, 7):
        base, enc, el, dl = struct.unpack_from('<IIII', d, p + 4); p += 20 + el
    elif t == 8: p += 12 + struct.unpack_from('<I', d, p + 8)[0]
    elif t == 9: ev.append(('event', struct.unpack_from('<I', d, p + 4)[0])); p += 8
    elif t == 10:
        first, cnt = struct.unpack_from('<II', d, p + 4)[:2]
        comp = struct.unpack_from('<I', d, p + 20)[0]
        p += 24 + comp
    elif t == 11: p += 16 + struct.unpack_from('<I', d, p + 12)[0]
    else: raise SystemExit(f'bad cmd {t} at {p}')
draws = [e for e in ev if e[0] == 'draw']; res = [e[1] for e in ev if e[0] == 'resolve']
print('draws', len(draws), 'resolves', len(res), 'events', sum(1 for e in ev if e[0] == 'event'))
c = collections.Counter((r['cclr'], r['dclr'], r['src']) for r in res)
print('resolves by (color_clear, depth_clear, src):', dict(c))
# segments: consecutive draws with same (pitch, msaa, active RTs, depth)
seg=[]; 
for e in ev:
    if e[0]=='draw':
        k=e[1][1:5]
        if seg and seg[-1][0]=='D' and seg[-1][1]==k: seg[-1][2]+=1; seg[-1][3]|=e[1][5]
        else: seg.append(['D',k,1,e[1][5]])
    elif e[0]=='resolve':
        r=e[1]; seg.append(['R',(r['src'],hex(r['cbase']),hex(r['dbase']),r['pitch'],r['msaa'],hex(r['dest']),r['cfmt'])])
print('segments',sum(1 for x in seg if x[0]=='D'))
for x in seg: print(x)
