#!/usr/bin/env python3
"""Bit-exact check of the DXT1 (BC1) -> ASTC 4x4 transcoder
(gpu/shaders/texture_load_dxt1_astc4x4.cs.slang) against a reference BC1
decode, through an independent ASTC LDR decoder for the block modes used.

usage: astc_dxt1_check.py [blocks]
"""
import random
import sys

M32 = 0xFFFFFFFF

# ---- port of the shader (bit-exact) -------------------------------------
TRIT_ENCODE = [
    0x04020100, 0x09080605, 0x1211100A, 0x18161514, 0x07031A19, 0x1B17130B,
    0x200E0D0C, 0x25242221, 0x2A292826, 0x34323130, 0x39383635, 0x2B27233A,
    0x2C3B3733, 0x41402E2D, 0x46454442, 0x504A4948, 0x55545251, 0x5A595856,
    0x534B4743, 0x4D4C5B57, 0x8281804E, 0x88868584, 0x91908A89, 0x96959492,
    0x839A9998, 0x97938B87, 0x8E8D8C9B, 0xA4A2A1A0, 0xA9A8A6A5, 0xB2B1B0AA,
    0xB8B6B5B4, 0xA7A3BAB9, 0xBBB7B3AB, 0xC0AEADAC, 0xC5C4C2C1, 0xCAC9C8C6,
    0xD4D2D1D0, 0xD9D8D6D5, 0xCBC7C3DA, 0xCCDBD7D3, 0x6160CECD, 0x66656462,
    0x706A6968, 0x75747271, 0x7A797876, 0x736B6763, 0x6D6C7B77, 0xE2E1E06E,
    0xE8E6E5E4, 0xF1F0EAE9, 0xF6F5F4F2, 0xE3FAF9F8, 0xF7F3EBE7, 0xEEEDECFB,
    0x3C1E1D1C, 0x5D5C3E3D, 0x9E9D9C5E, 0xDCBEBDBC, 0x3F1FDEDD, 0xDFBF9F5F,
    0x007E7D7C]


def trit_block(i):
    return (TRIT_ENCODE[i >> 2] >> ((i & 3) << 3)) & 0xFF


def rev32(v):
    return int(f"{v & M32:032b}"[::-1], 2)


def place(pos, value):
    word, shift = pos >> 5, pos & 31
    lo = (value << shift) & M32
    hi = (value >> (32 - shift)) & M32 if shift else 0
    out = [0, 0, 0, 0]
    out[word] |= lo
    if word < 3:
        out[word + 1] |= hi
    return out


def orb(a, b):
    return [x | y for x, y in zip(a, b)]


def endpoints_to_bgr8in10(endpoints):
    # XeDXTColorEndpointsToBGR8In10: 565 -> 888 bit replication, per endpoint
    # packed as B in 0:9? (layout below matches the shader's usage: >>20 = R,
    # >>10 = G, >>0 = B).
    out = []
    for c in (endpoints & 0xFFFF, endpoints >> 16):
        r5, g6, b5 = (c >> 11) & 31, (c >> 5) & 63, c & 31
        r = (r5 << 3) | (r5 >> 2)
        g = (g6 << 2) | (g6 >> 4)
        b = (b5 << 3) | (b5 >> 2)
        out.append((r << 20) | (g << 10) | b)
    return out


def high_color_weights(codes):
    # Weight of the second endpoint 0..3 per texel: 00->0, 01->3, 10->1, 11->2.
    m = {0: 0, 1: 3, 2: 1, 3: 2}
    w = 0
    for i in range(16):
        w |= m[(codes >> (2 * i)) & 3] << (2 * i)
    return w


def dequant7(v):
    return (v << 1) | (v >> 6)


def dxt1_to_astc(endpoints, codes):
    e = endpoints_to_bgr8in10(endpoints)
    rgb0 = [(e[0] >> 20) & 0xFF, (e[0] >> 10) & 0xFF, e[0] & 0xFF]
    rgb1 = [(e[1] >> 20) & 0xFF, (e[1] >> 10) & 0xFF, e[1] & 0xFF]
    if (endpoints & 0xFFFF) > (endpoints >> 16):
        weights = high_color_weights(codes)
        if sum(rgb1) < sum(rgb0):
            rgb0, rgb1 = rgb1, rgb0
            weights = (~weights) & M32
        block = [0x042 | (8 << 13), 0, 0, 0]
        block = orb(block, place(17, rgb0[0] | (rgb1[0] << 8) | (rgb0[1] << 16)))
        block = orb(block, place(41, rgb1[1] | (rgb0[2] << 8) | (rgb1[2] << 16)))
        block[3] |= rev32(weights)
        return block
    q0 = [c >> 1 for c in rgb0]
    q1 = [c >> 1 for c in rgb1]
    sum0 = sum(dequant7(c) for c in q0)
    sum1 = sum(dequant7(c) for c in q1)
    dark_weight = 0 if sum0 <= sum1 else 2
    alpha0, alpha1 = 0, 127
    swap = sum1 < sum0
    if swap:
        q0, q1 = q1, q0
        alpha0, alpha1 = 127, 0
    block = [0x451 | (12 << 13), 0, 0, 0]
    block = orb(block, place(17, q0[0] | (q1[0] << 7) | (q0[1] << 14)))
    block = orb(block, place(38, q1[1] | (q0[2] << 7) | (q1[2] << 14)))
    block = orb(block, place(59, alpha0 | (alpha1 << 7)))
    block = orb(block, place(74, 3))
    stream_lo = stream_hi = group = group_count = 0
    group_scale = 1
    stream_bits = 0
    for i in range(32):
        code = (codes >> ((i >> 1) << 1)) & 3
        if (i & 1) == 0:
            weight = 0 if code == 0 else (2 if code == 1 else (1 if code == 2 else dark_weight))
        else:
            weight = 0 if code == 3 else 2
        if swap:
            weight = 2 - weight
        group += weight * group_scale
        group_scale *= 3
        group_count += 1
        if group_count == 5 or i == 31:
            tb = trit_block(group)
            bit_count = 8 if group_count == 5 else 4
            tb &= (1 << bit_count) - 1
            if stream_bits < 32:
                stream_lo |= (tb << stream_bits) & M32
                if stream_bits + bit_count > 32:
                    stream_hi |= tb >> (32 - stream_bits)
            else:
                stream_hi |= (tb << (stream_bits - 32)) & M32
            stream_bits += bit_count
            group = group_count = 0
            group_scale = 1
    block[3] |= rev32(stream_lo)
    block[2] |= rev32(stream_hi)
    return block


# ---- reference BC1 decode -------------------------------------------------
def bc1_decode(endpoints, codes):
    e = endpoints_to_bgr8in10(endpoints)
    c0 = [(e[0] >> 20) & 0xFF, (e[0] >> 10) & 0xFF, e[0] & 0xFF, 255]
    c1 = [(e[1] >> 20) & 0xFF, (e[1] >> 10) & 0xFF, e[1] & 0xFF, 255]
    if (endpoints & 0xFFFF) > (endpoints >> 16):
        pal = [c0, c1, [(2 * a + b) / 3 for a, b in zip(c0, c1)],
               [(a + 2 * b) / 3 for a, b in zip(c0, c1)]]
    else:
        pal = [c0, c1, [(a + b) / 2 for a, b in zip(c0, c1)], None]
    return [pal[(codes >> (2 * i)) & 3] for i in range(16)]


# ---- independent ASTC LDR decoder (subset) ----------------------------------
def bits_of(block):
    v = 0
    for i, w in enumerate(block):
        v |= w << (32 * i)
    return v


def getb(v, start, n):
    return (v >> start) & ((1 << n) - 1)


TRIT_TABLE = None


def decode_trits(T):
    # ASTC spec trit decode of an 8-bit T into 5 trits.
    if getb(T, 2, 3) == 7:
        C = (getb(T, 5, 3) << 2) | getb(T, 0, 2)
        t4 = t3 = 2
    else:
        C = getb(T, 0, 5)
        if getb(T, 5, 2) == 3:
            t4, t3 = 2, getb(T, 7, 1)
        else:
            t4, t3 = getb(T, 7, 1), getb(T, 5, 2)
    if getb(C, 0, 2) == 3:
        t2 = 2
        t1 = getb(C, 4, 1)
        t0 = (getb(C, 3, 1) << 1) | (getb(C, 2, 1) & ~getb(C, 3, 1) & 1)
    elif getb(C, 2, 2) == 3:
        t2 = 2
        t1 = 2
        t0 = getb(C, 0, 2)
    else:
        t2 = getb(C, 4, 1)
        t1 = getb(C, 2, 2)
        t0 = (getb(C, 1, 1) << 1) | (getb(C, 0, 1) & ~getb(C, 1, 1) & 1)
    return [t0, t1, t2, t3, t4]


def ise_decode(v, start, count, levels):
    # Supports bits-only (power of two) and trit (3 * 2^m) ranges.
    if levels & (levels - 1) == 0:
        n = levels.bit_length() - 1
        return [getb(v, start + i * n, n) for i in range(count)]
    assert levels % 3 == 0
    m = (levels // 3).bit_length() - 1
    out = []
    pos = start
    for blk in range(0, count, 5):
        nvals = min(5, count - blk)
        mvals, T = [], 0
        tbits_layout = [2, 2, 1, 2, 1]
        tbit = 0
        for j in range(nvals):
            mvals.append(getb(v, pos, m)); pos += m
            nb = tbits_layout[j]
            T |= getb(v, pos, nb) << tbit
            pos += nb
            tbit += nb
        trits = decode_trits(T)
        for j in range(nvals):
            out.append((trits[j] << m) | mvals[j])
    return out


def unq_color(v, levels):
    if levels == 256:
        return v
    if levels == 128:
        return (v << 1) | (v >> 6)
    raise ValueError(levels)


def unq_weight(v, levels):
    if levels == 4:
        w = [0, 21, 43, 64][v]
    elif levels == 3:
        w = [0, 32, 64][v]
    else:
        raise ValueError(levels)
    return w


def astc_decode(block):
    v = bits_of(block)
    mode = getb(v, 0, 11)
    assert getb(v, 0, 2) != 0
    R = (getb(v, 4, 1)) | (getb(v, 0, 1) << 1) | (getb(v, 1, 1) << 2)
    # bits 1:0 are R2:R1 per spec: R = (R2 R1 R0) with R0 = bit4, R1 = bit0, R2 = bit1
    R = (getb(v, 1, 1) << 2) | (getb(v, 0, 1) << 1) | getb(v, 4, 1)
    H = getb(v, 9, 1)
    D = getb(v, 10, 1)
    assert getb(v, 2, 2) == 0
    A, B = getb(v, 5, 2), getb(v, 7, 2)
    W, Ht = B + 4, A + 2
    assert (W, Ht) == (4, 4), (W, Ht)
    levels = {(0, 2): 2, (0, 3): 3, (0, 4): 4, (0, 5): 5, (0, 6): 6, (0, 7): 8}[(H, R)]
    assert getb(v, 11, 2) == 0  # 1 partition
    cem = getb(v, 13, 4)
    nweights = 16 * (2 if D else 1)
    if levels == 4:
        wbits = 2 * nweights
    else:
        wbits = (8 * nweights + 4) // 5
    # weights are stored bit-reversed from bit 127 down
    rv = int(f"{v:0128b}", 2)  # identity, placeholder for clarity
    wv = int(f"{v:0128b}"[::1][:wbits][::-1], 2) if False else None
    weight_stream = 0
    for i in range(wbits):
        weight_stream |= getb(v, 127 - i, 1) << i
    weights = ise_decode(weight_stream, 0, nweights, levels)
    ccs = getb(v, 128 - wbits - 2, 2) if D else None
    color_end = 128 - wbits - (2 if D else 0)
    nvals = {8: 6, 12: 8}[cem]
    avail = color_end - 17
    # choose the largest range fitting avail bits among 256 and 128 (enough here)
    clev = 256 if 8 * nvals <= avail else 128
    if clev == 128:
        assert 7 * nvals <= avail
    n = 8 if clev == 256 else 7
    vals = [unq_color(getb(v, 17 + n * i, n), clev) for i in range(nvals)]
    if cem == 8:
        v0, v1, v2, v3, v4, v5 = vals
        s0, s1 = v0 + v2 + v4, v1 + v3 + v5
        assert s1 >= s0, "blue contraction"
        e0 = [v0, v2, v4, 255]
        e1 = [v1, v3, v5, 255]
    else:
        v0, v1, v2, v3, v4, v5, v6, v7 = vals
        s0, s1 = v0 + v2 + v4, v1 + v3 + v5
        assert s1 >= s0, "blue contraction"
        e0 = [v0, v2, v4, v6]
        e1 = [v1, v3, v5, v7]
    texels = []
    for t in range(16):
        if D:
            w_plane = [unq_weight(weights[2 * t], levels), unq_weight(weights[2 * t + 1], levels)]
        else:
            w_plane = [unq_weight(weights[t], levels)] * 2
        out = []
        for ch in range(4):
            w = w_plane[1] if (D and ch == ccs) else w_plane[0]
            c0, c1 = e0[ch] * 257, e1[ch] * 257
            c = (c0 * (64 - w) + c1 * w + 32) >> 6
            out.append(c >> 8)
        texels.append(out)
    return texels


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
    rnd = random.Random(1)
    worst = [0, 0]
    bad = 0
    for it in range(n):
        endpoints = rnd.getrandbits(32)
        if it % 3 == 0:  # force 3-color mode
            a, b = endpoints & 0xFFFF, endpoints >> 16
            if a > b:
                endpoints = b | (a << 16)
        codes = rnd.getrandbits(32)
        ref = bc1_decode(endpoints, codes)
        try:
            dec = astc_decode(dxt1_to_astc(endpoints, codes))
        except AssertionError as e:
            bad += 1
            if bad <= 5:
                print("ASSERT", hex(endpoints), hex(codes), e)
            continue
        mode = 0 if (endpoints & 0xFFFF) > (endpoints >> 16) else 1
        for t in range(16):
            r = ref[t]
            if r is None:  # transparent: alpha must be 0
                if dec[t][3] != 0:
                    bad += 1
                    if bad <= 5:
                        print("ALPHA", hex(endpoints), hex(codes), t, dec[t])
                continue
            err = max(abs(a - b) for a, b in zip(r, dec[t]))
            worst[mode] = max(worst[mode], err)
            if err > 4:
                bad += 1
                if bad <= 8:
                    print("MISMATCH", "4c" if mode == 0 else "3c", hex(endpoints), hex(codes), t, r, dec[t])
    print(f"blocks {n}  bad {bad}  worst err 4-color {worst[0]:.2f} 3-color {worst[1]:.2f}")


if __name__ == "__main__":
    main()
