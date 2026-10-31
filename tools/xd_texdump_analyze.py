#!/usr/bin/env python3
"""Analizza un dump vulkan_debug_texture_dump (cartella texdump/ di un test).

Per ogni texture registrata confronta:
  A = byte del gioco (CPU) al momento del load, A' = stessi byte alla cattura
  B = byte della shared memory GPU letti dallo shader di load
  C = uscita decodificata dallo shader (scratch) che viene copiata nell'immagine
e decodifica A in Python (untiling Xenos + DXT) per livello 0 e 1: se la
decodifica CPU coincide con C il load e' corretto; se differisce, il bug e' nel
load/decodifica GPU. Scrive PNG di confronto per le prime texture.

Uso: xd_texdump_analyze.py <cartella texdump> [--png N]
"""
import json
import os
import struct
import sys
import zlib

import numpy as np

BPB = {18: 8, 19: 16, 20: 16, 49: 16, 58: 8, 59: 8}


def swap(buf, endian):
    a = np.frombuffer(buf, dtype=np.uint8).copy()
    n = len(a) - len(a) % 4
    if endian == 1:  # 8in16
        v = a[:n].reshape(-1, 2)
        a[:n] = v[:, ::-1].reshape(-1)
    elif endian == 2:  # 8in32
        v = a[:n].reshape(-1, 4)
        a[:n] = v[:, ::-1].reshape(-1)
    elif endian == 3:  # 16in32
        v = a[:n].reshape(-1, 2, 2)
        a[:n] = v[:, ::-1, :].reshape(-1)
    return a.tobytes()


def tiled2d(x, y, pitch_aligned, bpb_log2):
    outer = (((y >> 5) * (pitch_aligned >> 5)) + (x >> 5)) << 6
    inner = (((y >> 1) & 7) << 3) | (x & 7)
    oib = (outer | inner) << bpb_log2
    bank = (y >> 4) & 1
    pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1)
    return (((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oib & 15) |
            (((oib >> 4) & 1) << 5) | (((oib >> 5) & 7) << 8) |
            ((oib >> 8) << 12))


def rgb565(c):
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return np.array([r, g, b], dtype=np.int32)


def color_block(blk, dxt1):
    c0, c1, idx = struct.unpack("<HHI", blk)
    p0, p1 = rgb565(c0), rgb565(c1)
    pal = [np.append(p0, 255), np.append(p1, 255)]
    if c0 > c1 or not dxt1:
        pal.append(np.append((2 * p0 + p1) // 3, 255))
        pal.append(np.append((p0 + 2 * p1) // 3, 255))
    else:
        pal.append(np.append((p0 + p1) // 2, 255))
        pal.append(np.array([0, 0, 0, 0]))
    out = np.zeros((4, 4, 4), dtype=np.int32)
    for i in range(16):
        out[i // 4, i % 4] = pal[(idx >> (2 * i)) & 3]
    return out


def alpha5(blk):
    a0, a1 = blk[0], blk[1]
    bits = int.from_bytes(blk[2:8], "little")
    if a0 > a1:
        pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:
        pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
    return np.array([pal[(bits >> (3 * i)) & 7] for i in range(16)]).reshape(4, 4)


def decode_level(data, fmt, endian, tiled, pitch_blocks, wb, hb):
    """Decodifica un livello (non packed) in RGBA (DXT1/3/5) o R/RG (DXT5A/DXN)."""
    bpb = BPB[fmt]
    log2 = bpb.bit_length() - 1
    data = swap(data, endian)
    ch = {18: 4, 19: 4, 20: 4, 49: 2, 58: 1, 59: 1}[fmt]
    img = np.zeros((hb * 4, wb * 4, ch), dtype=np.int32)
    for by in range(hb):
        for bx in range(wb):
            off = tiled2d(bx, by, pitch_blocks, log2) if tiled else (by * pitch_blocks + bx) * bpb
            blk = data[off:off + bpb]
            if len(blk) < bpb:
                continue
            if fmt == 18:
                t = color_block(blk, True)
            elif fmt == 19:
                t = color_block(blk[8:], False)
                a = np.frombuffer(blk[:8], dtype=np.uint8)
                al = np.array([(a[i // 2] >> (4 * (i & 1))) & 15 for i in range(16)]) * 17
                t[:, :, 3] = al.reshape(4, 4)
            elif fmt == 20:
                t = color_block(blk[8:], False)
                t[:, :, 3] = alpha5(blk[:8])
            elif fmt == 59:
                t = alpha5(blk)[:, :, None]
            elif fmt == 49:
                t = np.dstack([alpha5(blk[:8]), alpha5(blk[8:])])
            else:
                continue
            img[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4] = t
    return img


def host_level(scratch, off, xpitch, ypitch, bph, w, h, ch):
    row = xpitch * bph
    a = np.frombuffer(scratch, dtype=np.uint8)
    out = np.zeros((h, w, ch), dtype=np.int32)
    for y in range(h):
        s = off + y * row
        r = a[s:s + w * bph]
        if len(r) < w * bph:
            break
        out[y] = r.reshape(w, bph)[:, :ch]
    return out


def png(path, img):
    img = np.clip(img, 0, 255).astype(np.uint8)
    if img.shape[2] == 1:
        img = np.repeat(img, 3, axis=2)
    elif img.shape[2] == 2:
        img = np.dstack([img, np.zeros(img.shape[:2], np.uint8)])
    img = img[:, :, :3] if img.shape[2] == 4 else img
    h, w, _ = img.shape
    raw = b"".join(b"\0" + img[y].tobytes() for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def mad(a, b):
    h, w = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
    ch = min(a.shape[2], b.shape[2], 3)
    return float(np.abs(a[:h, :w, :ch] - b[:h, :w, :ch]).mean())


def downsample(img):
    h, w = img.shape[0] // 2 * 2, img.shape[1] // 2 * 2
    i = img[:h, :w]
    return (i[0::2, 0::2] + i[1::2, 0::2] + i[0::2, 1::2] + i[1::2, 1::2]) // 4


def main():
    DIR = sys.argv[1]
    PNG_N = int(sys.argv[sys.argv.index("--png") + 1]) if "--png" in sys.argv else 8
    outdir = os.path.join(DIR, "analysis")
    os.makedirs(outdir, exist_ok=True)
    rows = [json.loads(l) for l in open(os.path.join(DIR, "index.jsonl")) if l.strip()]
    print(f"texture registrate: {len(rows)}")
    print("idx fmt        size      tiled end pk | A==A'(mips) A==B(base,mips) | L0 cpu~gpu  L1 cpu~gpu  L1cpu~down(L0cpu) L1gpu~down(L0gpu) | reloads")
    for r in rows:
        raw = open(os.path.join(DIR, r["file"]), "rb").read()
        bs, ms, ss = r["base_size"], r["mips_size"], r["scratch_size"]
        o = 0
        sec = {}
        for name, n in (("a_base", bs), ("a_mips", ms), ("an_base", bs), ("an_mips", ms)):
            sec[name] = raw[o:o + n]; o += n
        if r["gpu_ok"]:
            for name, n in (("b_base", bs), ("b_mips", ms), ("c", ss)):
                sec[name] = raw[o:o + n]; o += n
        same = lambda x, y: "=" if sec.get(x) == sec.get(y) else \
            f"DIFF{int(np.count_nonzero(np.frombuffer(sec[x], np.uint8) != np.frombuffer(sec[y], np.uint8)))}"
        a_mips_same = same("a_mips", "an_mips")
        ab = (same("a_base", "b_base") if bs else "-") + "," + same("a_mips", "b_mips")
        fmt = r["format"]
        info = ""
        if fmt in BPB and r["gpu_ok"]:
            bpb = BPB[fmt]
            ch = {18: 4, 19: 4, 20: 4, 49: 2, 58: 1, 59: 1}[fmt]
            w, h = r["width"], r["height"]
            res = {}
            if bs and r["load_base"]:
                pb = r["base_row_pitch_bytes"] // bpb
                l0c = decode_level(sec["a_base"], fmt, r["endianness"], r["tiled"], pb, (w + 3) // 4, (h + 3) // 4)[:h, :w]
                l0g = host_level(sec["c"], r["base_host_offset"], r["base_host_x_pitch_blocks"],
                                 r["base_host_y_pitch_blocks"], r["bytes_per_host_block"], w, h, ch)
                res["l0"] = (l0c, l0g)
            lv = r["levels"][0] if r["levels"] else None
            if lv and 1 < r["packed_level"]:
                w1, h1 = max(w >> 1, 1), max(h >> 1, 1)
                pb = lv["row_pitch_bytes"] // bpb
                l1c = decode_level(sec["a_mips"][lv["guest_offset"]:], fmt, r["endianness"], r["tiled"], pb,
                                   (w1 + 3) // 4, (h1 + 3) // 4)[:h1, :w1]
                l1g = host_level(sec["c"], lv["host_offset"], lv["host_x_pitch_blocks"], lv["host_y_pitch_blocks"],
                                 r["bytes_per_host_block"], w1, h1, ch)
                res["l1"] = (l1c, l1g)
            f = lambda k: f"{mad(*res[k]):6.1f}" if k in res else "     -"
            d1 = f"{mad(res['l1'][0], downsample(res['l0'][0])):6.1f}" if "l0" in res and "l1" in res else "     -"
            d2 = f"{mad(res['l1'][1], downsample(res['l0'][1])):6.1f}" if "l0" in res and "l1" in res else "     -"
            info = f"{f('l0')}      {f('l1')}      {d1}             {d2}"
            if r["index"] < PNG_N:
                for k, (c, g) in res.items():
                    png(os.path.join(outdir, f"tex{r['index']:03}-{k}-cpu.png"), c)
                    png(os.path.join(outdir, f"tex{r['index']:03}-{k}-gpu.png"), g)
        print(f"{r['index']:3} {r['format_name'][:10]:10} {r['width']:4}x{r['height']:<4} {r['tiled']:5} {r['endianness']:3} "
              f"{r['packed_level']:2} | {a_mips_same:11} {ab:15} | {info} | {r['later_loads']}{r['later_load_kinds'][:8]}")


if __name__ == "__main__":
    main()
