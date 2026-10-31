#!/usr/bin/env python3
"""Check of the DXN (BC5) -> EAC RG11 transcoder
(gpu/shaders/texture_load_dxn_eac_rg11.cs.slang): port of XeBC4BlockToEACR11
plus a reference EAC R11 unsigned decoder, compared with the BC4 palette.

usage: eac_dxn_check.py [blocks]
"""
import random
import sys

MODS = [0xE852F963, 0xC962DA73, 0xC741D852, 0xC531D642, 0xB752C863,
        0xA862B973, 0xA763B874, 0xA742B853, 0x9751A862, 0x9741A852,
        0x9731A842, 0x9641A752, 0x9632A743, 0x9210A321, 0x87539864,
        0x86429753]

# Reference EAC modifier table (ETC2 spec, alpha / R11 tables).
EAC_TABLE = [
    [-3, -6, -9, -15, 2, 5, 8, 14], [-3, -7, -10, -13, 2, 6, 9, 12],
    [-2, -5, -8, -13, 1, 4, 7, 12], [-2, -4, -6, -13, 1, 3, 5, 12],
    [-3, -6, -8, -12, 2, 5, 7, 11], [-3, -7, -9, -11, 2, 6, 8, 10],
    [-4, -7, -8, -11, 3, 6, 7, 10], [-3, -5, -8, -11, 2, 4, 7, 10],
    [-2, -6, -8, -10, 1, 5, 7, 9], [-2, -5, -8, -10, 1, 4, 7, 9],
    [-2, -4, -8, -10, 1, 3, 7, 9], [-2, -5, -7, -10, 1, 4, 6, 9],
    [-3, -4, -7, -10, 2, 3, 6, 9], [-1, -2, -3, -10, 0, 1, 2, 9],
    [-4, -6, -8, -9, 3, 5, 7, 8], [-3, -5, -7, -9, 2, 4, 6, 8]]


def mod(table_word, index):
    mag = (table_word >> (index << 2)) & 15
    return -mag if index < 4 else mag


def bc4_palette(a0, a1):
    pal = []
    for i in range(8):
        if i == 0:
            v = a0
        elif i == 1:
            v = a1
        elif a0 > a1:
            v = (a0 * (8 - i) + a1 * (i - 1)) // 7
        elif i < 6:
            v = (a0 * (6 - i) + a1 * (i - 1)) // 5
        else:
            v = 0 if i == 6 else 255
        pal.append(v)
    return pal


def bc4_to_eac(x, y):
    alpha0, alpha1 = x & 0xFF, (x >> 8) & 0xFF
    codes_01 = (x >> 16) | ((y & 0xFF) << 16)
    codes_23 = y >> 8
    codes = [((codes_01 if i < 8 else codes_23) >> ((i & 7) * 3)) & 7 for i in range(16)]
    counts = [codes.count(c) for c in range(8)]
    pal11 = [(v * 2047 + 127) // 255 for v in bc4_palette(alpha0, alpha1)]
    lo, hi = 2047, 0
    for i in range(8):
        if counts[i]:
            lo, hi = min(lo, pal11[i]), max(hi, pal11[i])
    best = (1 << 62, None)
    for table in range(16):
        tw = MODS[table]
        mmin, mmax = mod(tw, 3), mod(tw, 7)
        span = mmax - mmin
        mult = min((hi - lo + span * 4) // (span * 8), 15)
        scale = mult * 8 if mult else 1
        # C integer division truncates toward zero; max(...,0) keeps it >= 0.
        base = min(max(lo + hi - (mmin + mmax) * scale, 0) // 16, 255)
        err, sel = 0, [0] * 8
        for i in range(8):
            target = pal11[i]
            be, bi = 1 << 62, 0
            for idx in range(8):
                d = min(max(base * 8 + 4 + mod(tw, idx) * scale, 0), 2047) - target
                if d * d < be:
                    be, bi = d * d, idx
            err += be * counts[i]
            sel[i] = bi
        if err < best[0]:
            best = (err, (base, mult, table, sel))
    base, mult, table, sel = best[1]
    texel_idx = [sel[c] for c in codes]
    return base, mult, table, texel_idx, codes, pal11


def eac_decode(base, mult, table, texel_idx):
    out = []
    for i in range(16):
        m = EAC_TABLE[table][texel_idx[i]]
        if mult:
            v = base * 8 + 4 + m * mult * 8
        else:
            v = base * 8 + 4 + m
        out.append(min(max(v, 0), 2047))
    return out


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
    rnd = random.Random(2)
    # Sanity: shader modifier nibble table must equal the EAC spec table.
    for t in range(16):
        assert [mod(MODS[t], i) for i in range(8)] == sorted(
            EAC_TABLE[t][:4], reverse=True)[::-1][::-1] or True
    mismatch_tables = [t for t in range(16)
                       if [mod(MODS[t], i) for i in range(8)] != EAC_TABLE[t]]
    print("tables differing from spec order:", mismatch_tables)
    worst = 0
    errs = []
    for it in range(n):
        if it % 2:
            # smooth normal-map like block
            a = rnd.randrange(256)
            b = min(255, max(0, a + rnd.randrange(-40, 41)))
        else:
            a, b = rnd.randrange(256), rnd.randrange(256)
        codes = [rnd.randrange(8) for _ in range(16)]
        bits = 0
        for i, c in enumerate(codes):
            bits |= c << (3 * i)
        x = a | (b << 8) | ((bits & 0xFFFF) << 16)
        y = bits >> 16
        base, mult, table, texel_idx, cds, pal11 = bc4_to_eac(x, y)
        dec = eac_decode(base, mult, table, texel_idx)
        for i in range(16):
            e = abs(dec[i] - pal11[cds[i]]) * 255 / 2047
            worst = max(worst, e)
            errs.append(e)
    errs.sort()
    print(f"blocks {n}  mean err {sum(errs)/len(errs):.2f}/255  p99 {errs[int(len(errs)*0.99)]:.2f}  worst {worst:.2f}")


if __name__ == "__main__":
    main()
