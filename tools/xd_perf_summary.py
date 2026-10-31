#!/usr/bin/env python3
"""FPS medi in gameplay da xe-after.log (righe VkFrameSync di log_gpu_frame_time_breakdown).

Considera solo le righe di gameplay (draws/frame > 2000), scarta le ultime 8
(cattura). Uso: xd_perf_summary.py <evidence dir> [...]
"""
import re
import statistics
import sys

rx = re.compile(r"VkFrameSync: (\d+) frames.*?submissions=([\d.]+).*?gpu exec avg=([\d.]+)ms.*?gap avg=([\d.]+)ms"
                r".*?draws=(\d+) rp_begins=(\d+)")
for d in sys.argv[1:]:
    rows = []
    for l in open(d + "/xe-after.log", errors="replace"):
        m = rx.search(l)
        if m:
            fr, sub, gpu, gap, draws, rp = m.groups()
            rows.append((int(fr), float(sub), float(gpu), float(gap), int(draws), int(rp)))
    game = [r for r in rows if r[4] > 2000][:-8]
    if not game:
        print(d, "no gameplay rows")
        continue
    fps = [r[0] for r in game]
    gpu_frame = [r[1] * r[2] for r in game]
    print(f"{d.split('/')[-1]:24} n={len(game):3} fps mean={statistics.mean(fps):5.1f} median={statistics.median(fps):4.0f} "
          f"p10={sorted(fps)[len(fps)//10]:3} | gpu/frame={statistics.mean(gpu_frame):5.1f}ms "
          f"gap={statistics.mean(r[3] for r in game):4.1f}ms draws={statistics.mean(r[4] for r in game):5.0f} "
          f"rp={statistics.mean(r[5] for r in game):4.0f}")
