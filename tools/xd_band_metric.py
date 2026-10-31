#!/usr/bin/env python3
"""Band metric on a source-RT capture (q15b+).

Usage: xd_band_metric.py <evidence folder with source-rt-frame-*.bin/.json and guest-output-*.png>

Prints:
  - ratio of jumps at the 16-row edges / jumps elsewhere, for the area overlapping the RT output (rows 0-335)
    and the non-overlapping area (336-599). Clean ~= 1.0-1.5; q15b: 6.3 (overlapping) / 1.5.
  - MAD between the top of the scene and the output remapped via EDRAM (base 608 pitch 16)
    against wrong controls. If the exact map wins by a lot -> output content leaked.
"""
import glob
import json
import os
import sys

import numpy as np
from PIL import Image

d = sys.argv[1]
meta_path = sorted(glob.glob(os.path.join(d, "source-rt-frame-*.json")))[0]
meta = json.load(open(meta_path))
bpp = meta["bytes_per_pixel"]
w, h = meta["width"], meta["height"]
raw = np.fromfile(os.path.join(d, meta["data_file"]),
                  dtype=np.uint8 if bpp == 4 else np.uint16)
img = raw.reshape(h, w, 4)[:, :, :3]
if bpp == 8:
    img = (img >> 8).astype(np.uint8)
Image.fromarray(img[:608].astype(np.uint8)).save(os.path.join(d, "source-rt-top608.png"))
src = img[:600, :1024].astype(float)

g = np.abs(np.diff(src, axis=0)).mean(axis=(1, 2))


def ratio(lo, hi):
    rows = range(max(lo, 1), hi - 1)
    b = [g[r - 1] for r in rows if r % 16 == 0]
    nb = [g[r - 1] for r in rows if r % 16 != 0]
    return np.mean(b), np.mean(nb)


for lo, hi, name in [(0, 336, "sovrapposta output (0-335)"), (336, 600, "non sovrapposta (336-599)")]:
    b, nb = ratio(lo, hi)
    print(f"{name}: bordi16={b:.1f} altrove={nb:.1f} rapporto={b / nb:.2f}")

pngs = sorted(glob.glob(os.path.join(d, "guest-output-pre-present-frame-*.png")))
if pngs:
    out = np.asarray(Image.open(pngs[0]).convert("RGB")).astype(float)
    top = src[:336]

    def mad(obase, opitch):
        tot = 0.0
        n = 0
        for y in range(336):
            r = y // 16
            for c in range(13):
                j = 1060 + 13 * r + c - obase
                if j < 0:
                    continue
                ty, tx = j // opitch, j % opitch
                oy = ty * 16 + y % 16
                if tx * 80 + 80 > 1280 or oy >= 720 or c * 80 + 80 > 1024:
                    continue
                tot += np.abs(top[y, c * 80:(c + 1) * 80] - out[oy, tx * 80:(tx + 1) * 80]).sum()
                n += 240
        return tot / max(n, 1)

    exact = mad(608, 16)
    ctrl = min(mad(607, 16), mad(609, 16), mad(592, 16), mad(624, 16))
    print(f"MAD EDRAM exact map={exact:.1f}  best wrong control={ctrl:.1f}  "
          f"-> {'OUTPUT LEAKED INTO THE SCENE' if exact * 2 < ctrl else 'no leak signature'}")
