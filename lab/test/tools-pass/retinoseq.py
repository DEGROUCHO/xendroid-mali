#!/usr/bin/env python3
"""retinoseq.py DIR XE_RUN_LOG -> one line per guest-output-seq-N.rgbx + summary.

Dante "retino" at guest resolution: a grid of period 2 guest px is the Nyquist bin (0.5 cycles/px) of the
row/column FFT. Ratio = Nyquist power / median power of 0.40-0.48 cycles/px; a frame counts above 10 (the
retinovid.py threshold on the phone screenshot, where the same grid is the 3.77 px screen bin). The raw frames
are what the presenter samples (VulkanPresenter::CaptureGuestOutput), so grid here = rendering, no grid here
while the screen has it = presentation.
"""
import re, sys
from pathlib import Path
import numpy as np


def ratios(g):
    c = g[40:-40, 40:-40]
    out = []
    for axis in (1, 0):
        a = c - c.mean(axis=axis, keepdims=True)
        p = (np.abs(np.fft.rfft(a, axis=axis)) ** 2).mean(axis=1 - axis)
        f = np.fft.rfftfreq(a.shape[axis])
        out.append(p[-1] / np.median(p[(f > 0.40) & (f < 0.48)]))
    return out


def main():
    d, log = Path(sys.argv[1]), Path(sys.argv[2]).read_text(errors='replace')
    sizes = {int(n): (int(w), int(h)) for n, w, h in
             re.findall(r'GUEST-OUTPUT-SEQ\] status=ok frame=(\d+) width=(\d+) height=(\d+)', log)}
    hits = n = 0
    for p in sorted(d.glob('guest-output-seq-*.rgbx'), key=lambda p: int(p.stem.split('-')[-1])):
        frame = int(p.stem.split('-')[-1])
        if frame not in sizes:
            print(f'{p.name} no size in log')
            continue
        w, h = sizes[frame]
        g = np.fromfile(p, np.uint8).reshape(h, w, 4)[:, :, :3].astype(float).mean(axis=2)
        rx, ry = ratios(g)
        n += 1
        hits += max(rx, ry) > 10
        print(f'frame {frame} {w}x{h} nyquist/band orizz {rx:.1f} vert {ry:.1f}{" RETINO" if max(rx, ry) > 10 else ""}')
    print(f'retino guest {hits}/{n} frames')


if __name__ == '__main__':
    main()
