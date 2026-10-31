#!/usr/bin/env python3
"""Test 'output trapelato nella scena' usando SOLO un PNG guest-output 1280x720.
Per ogni pixel di output nella zona alta (scena righe 0-335) confronta con il pixel
dell'output stesso ottenuto rimappando i tile EDRAM scena(1060,p13)->output(608,p16).
Controlli: basi sbagliate. Mappa esatta << controlli => firma di trapelamento."""
import sys
import numpy as np
from PIL import Image

out = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(float)
H, W = out.shape[:2]
assert (W, H) == (1280, 720), (W, H)
Y, X = np.mgrid[0:720, 0:1280]
sx = np.minimum((X / 1.25).astype(int), 1023)
sy = np.minimum((Y / 1.2).astype(int), 599)
sel = sy < 336


def mad(obase, opitch=16):
    r, c = sy // 16, sx // 80
    j = 1060 + 13 * r + c - obase
    ty, tx = j // opitch, j % opitch
    px = tx * 80 + sx % 80
    py = ty * 16 + sy % 16
    ok = sel & (j >= 0) & (py < 720) & (px < 1280) & (c < 13)
    a = out[Y[ok], X[ok]]
    b = out[py[ok], px[ok]]
    return float(np.abs(a - b).mean()), int(ok.sum())


exact, n = mad(608)
ctrls = [mad(b)[0] for b in (592, 607, 609, 624)]
print(f"{sys.argv[1].split('/')[-2]}: MAD esatta={exact:.1f} controlli={[round(c, 1) for c in ctrls]} "
      f"-> {'TRAPELAMENTO' if exact * 2 < min(ctrls) else 'nessuna firma'} (n={n})")
