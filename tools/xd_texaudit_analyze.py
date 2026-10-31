#!/usr/bin/env python3
"""Analizza texaudit/ (vulkan_debug_texture_audit).

Per ogni texture campionata nel frame catturato: immagine host (primo livello
letto) vs decodifica CPU dei byte guest attuali (DXT/DXN/DXT5A, ridotta al
livello letto). Scrive contact sheet PNG in texaudit/sheets e una tabella.

Uso: xd_texaudit_analyze.py <cartella texaudit> [--nocmp]
"""
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(__file__))
from xd_texdump_analyze import BPB, decode_level, downsample  # noqa: E402

D = sys.argv[1]
NOCMP = "--nocmp" in sys.argv
images = np.memmap(os.path.join(D, "images.bin"), np.uint8, "r")
guest = np.memmap(os.path.join(D, "guest.bin"), np.uint8, "r")
rows = [json.loads(l) for l in open(os.path.join(D, "audit.jsonl"))]
out = os.path.join(D, "sheets")
os.makedirs(out, exist_ok=True)


def skips_top_mip(r):
    """texture_skip_top_mip_min_size (256): BC 2D/cube with guest mips, larger side >= 256 -> the host image is
    guest mip 1 (half size) while audit.jsonl still lists the guest size."""
    return (r["format"] in (18, 19, 20, 49, 51, 52, 53, 58, 59, 60, 61) and r["mip_max_level"] > 0 and r["mip_page"]
            and r.get("dimension", 1) != 2 and max(r["width"], r["height"]) >= SKIP_MIN)


SKIP_MIN = int(os.environ.get("XD_SKIP_TOP_MIP_MIN", "256"))  # 0 = the run had the option off


def host_rgb(r, li=0):
    lv, off, w, h = r["levels"][li]
    ts = r["texel_size"]
    a = np.array(images[off:off + w * h * ts]).reshape(h, w, ts)
    if "mip_skip" not in r and SKIP_MIN and skips_top_mip(r):  # pre-q573: rows keep the listed width
        w, h = max(w >> 1, 1), max(h >> 1, 1)
        a = a[:h, :w]
    if ts == 4:
        return a[:, :, :3].astype(np.int32), a
    if ts == 2 and r["vk_format"] in (16, 17):  # R8G8 unorm/snorm
        b = a.astype(np.int32)
        if r["vk_format"] == 17:
            b = (a.view(np.int8).astype(np.int32) + 128)
        return np.dstack([b[:, :, 0], b[:, :, 1], np.zeros((h, w), np.int32)]), a
    if ts == 1:
        g = a[:, :, 0].astype(np.int32)
        if r["vk_format"] == 10:
            g = a[:, :, 0].view(np.int8).astype(np.int32) + 128
        return np.dstack([g, g, g]), a
    g = a[:, :, 0].astype(np.int32)
    return np.dstack([g, g, g]), a


def cpu_level(r):
    fmt = {51: 18, 52: 19, 53: 20}.get(r["format"], r["format"])  # *_AS_16_16_16_16 = same blocks
    if fmt not in BPB or not r["guest_size"] or not r["base_page"] or r.get("dimension", 1) == 2:
        return None
    bpb = BPB[fmt]
    w, h = r["width"], r["height"]
    data = bytes(guest[r["guest_offset"]:r["guest_offset"] + r["guest_size"]])
    img = decode_level(data, fmt, r["endianness"], r["tiled"],
                       r["base_row_pitch_bytes"] // bpb, (w + 3) // 4, (h + 3) // 4)[:h, :w]
    skip = r["mip_skip"] if "mip_skip" in r else (1 if SKIP_MIN and skips_top_mip(r) else 0)
    for _ in range(r["levels"][0][0] + skip):
        img = downsample(img)
    return img


table = []
tiles = []
for r in rows:
    rgb, raw = host_rgb(r)
    mad = None
    if not NOCMP:
        c = cpu_level(r)
        if c is not None:
            hh, ww = min(c.shape[0], raw.shape[0]), min(c.shape[1], raw.shape[1])
            ch = min(c.shape[2], raw.shape[2], 3 if raw.shape[2] >= 3 else raw.shape[2])
            hv = raw[:hh, :ww, :ch].astype(np.int32)
            if r["vk_format"] in (17, 10):
                hv = raw[:hh, :ww, :ch].view(np.int8).astype(np.int32) + 128
                cv = c[:hh, :ww, :ch].astype(np.int32)
                cv = np.where(cv > 127, cv - 256, cv) + 128  # DXN decode gives unsigned bytes
                mad = float(np.abs(hv - cv).mean())
            else:
                mad = float(np.abs(hv - c[:hh, :ww, :ch]).mean())
    r["mad"] = mad
    table.append(r)
    t = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8)).resize((128, 128), Image.NEAREST)
    d = ImageDraw.Draw(t)
    d.rectangle((0, 0, 60, 11), fill=(0, 0, 0))
    d.text((1, 0), f"{r['index']} {'' if mad is None else int(mad)}", fill=(255, 255, 0))
    tiles.append(t)

for s in range(0, len(tiles), 100):
    sheet = Image.new("RGB", (10 * 130, 10 * 130), (40, 40, 40))
    for i, t in enumerate(tiles[s:s + 100]):
        sheet.paste(t, ((i % 10) * 130, (i // 10) * 130))
    sheet.save(os.path.join(out, f"sheet-{s // 100}.png"))

DIM = {0: "1D", 1: "2D", 2: "3D", 3: "CU"}  # xenos::DataDimension; "2D" = 2D or stacked
print("idx fmt        dim size       lvl vk  loads gen chg bo mo  mad   base_page")
for r in table:
    flag = "  <<<" if r["mad"] is not None and r["mad"] > 8 else ""
    dim = DIM.get(r.get("dimension", 1), "?") + (f"{r['depth']}" if r.get("depth", 1) > 1 else "")
    print(f"{r['index']:3} {r['format_name'][:10]:10} {dim:3} {r['width']:4}x{r['height']:<4} {r['levels'][0][0]:2} {r['vk_format']:3} "
          f"{r['loads']:4} {int(r['generate_mips'])}   {int(r['changed'])}   {int(r['base_outdated'])}  {int(r['mips_outdated'])} "
          f"{'   -' if r['mad'] is None else f'{r['mad']:5.1f}'} {r['base_page']:x}{flag}")
