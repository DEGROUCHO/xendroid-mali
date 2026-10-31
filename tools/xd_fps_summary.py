#!/usr/bin/env python3
"""Gameplay fps from GuestFps log lines (log_guest_fps, no GPU timestamps).

Uses rows with more than 2000 draws/frame (gameplay). The main numbers are over
a fixed window, gameplay seconds 3-42, so runs compare the same part of the
level (a faster menu used to stretch the window into other scenes); "all" is
every gameplay row but the last 8 (capture). usage: xd_fps_summary.py <dir>...
"""
import re
import statistics
import sys

rx = re.compile(r"GuestFps: ([\d.]+) fps, (\d+) draws/frame")
for d in sys.argv[1:]:
    rows = [(float(m.group(1)), int(m.group(2)))
            for l in open(d + "/xe-after.log", errors="replace")
            for m in [rx.search(l)] if m]
    game = [r for r in rows if r[1] > 2000][:-8]
    if not game:
        print(d.split('/')[-1], "no gameplay GuestFps rows")
        continue
    window = [r for r in rows if r[1] > 2000][2:42]
    fps = sorted(r[0] for r in window)
    every = sorted(r[0] for r in game)
    print(f"{d.split('/')[-1]:24} n={len(window):3} fps mean={statistics.mean(fps):5.1f} "
          f"median={statistics.median(fps):5.1f} p10={fps[len(fps)//10]:5.1f} "
          f"draws={statistics.mean(r[1] for r in window):5.0f} | all n={len(game)} "
          f"median={statistics.median(every):5.1f}")
