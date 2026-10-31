#!/usr/bin/env python3
"""xtrtex.py file.xtr BASE_HEX SIZE W H FMT out.png - estrae dalla traccia la lettura di memoria (BASE,SIZE) e la decodifica
come texture Xbox 360 (FMT 18 = DXT1, 20 = DXT5; scambio a 16 bit, untiling) in un PNG. Serve a vedere a occhio se i byte
registrati sono davvero una texture."""
import struct, sys, zlib
exec(open(__file__.rsplit('/', 1)[0] + '/xtrmem.py').read().split("d = open(sys.argv[1]")[0])
path, base, size, w, h, fmt, out = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]), sys.argv[7]
d = open(path, 'rb').read(); p = 48; data = None
while p < len(d) and data is None:
    t = struct.unpack_from('<I', d, p)[0]
    if t in (0, 2): p += 12 + struct.unpack_from('<I', d, p + 8)[0] * 4
    elif t in (1, 3, 5): p += 4
    elif t == 4: p += 12 + struct.unpack_from('<I', d, p + 8)[0] * 4
    elif t in (6, 7):
        b, enc, el, dl = struct.unpack_from('<IIII', d, p + 4); raw = d[p + 20:p + 20 + el]; p += 20 + el
        if t == 6 and b == base and dl == size: data = unsnappy(raw) if enc == 1 else raw
    elif t == 8: p += 12 + struct.unpack_from('<I', d, p + 8)[0]
    elif t == 9: p += 8
    elif t == 10: p += 24 + struct.unpack_from('<I', d, p + 20)[0]
    elif t == 11: p += 16 + struct.unpack_from('<I', d, p + 12)[0]
assert data is not None, 'lettura non trovata'
def tiled(x, y, pitch, bpb_log2):
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return ((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F)
bs = 8 if fmt == 18 else 16; lg = 3 if fmt == 18 else 4
bw, bh = w // 4, h // 4
img = [bytearray(w * 3) for _ in range(h)]
def c565(v): return ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
for by in range(bh):
    for bx in range(bw):
        o = tiled(bx, by, bw, lg)
        blk = bytearray(data[o:o + bs])
        if len(blk) < bs: continue
        for i in range(0, bs, 2): blk[i], blk[i + 1] = blk[i + 1], blk[i]   # k8in16
        cb = blk[bs - 8:]
        c0, c1, bits = struct.unpack('<HHI', cb)
        a, b_ = c565(c0), c565(c1)
        if c0 > c1 or fmt != 18: pal = [a, b_, tuple((2 * a[i] + b_[i]) // 3 for i in range(3)), tuple((a[i] + 2 * b_[i]) // 3 for i in range(3))]
        else: pal = [a, b_, tuple((a[i] + b_[i]) // 2 for i in range(3)), (0, 0, 0)]
        for py in range(4):
            row = img[by * 4 + py]
            for px in range(4):
                c = pal[(bits >> (2 * (py * 4 + px))) & 3]
                k = (bx * 4 + px) * 3; row[k:k + 3] = bytes(c)
raw = b''.join(b'\x00' + bytes(r) for r in img)
def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
print('scritto', out)
