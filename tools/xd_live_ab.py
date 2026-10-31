#!/usr/bin/env python3
"""Split an in-game A/B run (LIVE_AB, "LiveCvar:" log lines) by variant.
usage: xd_live_ab.py <xe-after.log>
Per variant: GuestFps median (first second after each switch dropped), draws,
and the mean of every numeric CPWait field."""
import re, sys, statistics, collections

log = sys.argv[1]
# GuestFps lines (~1 s each) counted per window after the switch second; the
# rest of a window (e.g. the end-of-run capture after the last switch) is cut.
window_max = int(sys.argv[2]) if len(sys.argv) > 2 else 6
variant = None
skip = 0
in_window = 0
fps = collections.defaultdict(list)
draws = collections.defaultdict(list)
cpw = collections.defaultdict(lambda: collections.defaultdict(list))
order = []
for line in open(log, errors="replace"):
    m = re.search(r"LiveCvar: frame=\d+ applied:(.*?)(\s\|\s|$)", line)
    if m:
        variant = m.group(1).strip()
        if variant not in order:
            order.append(variant)
        skip = 1  # first GuestFps line spans the switch
        in_window = 0
        continue
    if variant is None or in_window > window_max:
        continue
    m = re.search(r"GuestFps: ([\d.]+) fps, (\d+) draws/frame", line)
    if m:
        if skip:
            skip -= 1
            continue
        in_window += 1
        if in_window > window_max:
            continue
        fps[variant].append(float(m.group(1)))
        draws[variant].append(int(m.group(2)))
        continue
    m = re.search(r"ShmProtect: per frame: mprotect calls=([\d.]+).*?time=([\d.]+) ms", line)
    if m:
        cpw[variant]["mprotect_calls"].append(float(m.group(1)))
        cpw[variant]["mprotect_ms"].append(float(m.group(2)))
        continue
    m = re.search(r"FrameGen: .*?([\d.]+) presented/s", line)
    if m:
        cpw[variant]["fg_presented"].append(float(m.group(1)))
    m = re.search(r"analysis ([\d.]+) ms", line)
    if m:
        cpw[variant]["fg_analysis_ms"].append(float(m.group(1)))
    if "CPWait:" in line:
        body = line.split("CPWait:", 1)[1]
        for k, v in re.findall(r"([A-Za-z_]+(?: [A-Za-z_]+)?)=([\d.]+)", body):
            cpw[variant][k].append(float(v))
for v in order:
    f = fps[v]
    if not f:
        continue
    print("%-50s fps n=%2d median %.1f mean %.1f p10 %.1f | draws %d" % (
        v, len(f), statistics.median(f), statistics.mean(f),
        sorted(f)[len(f) // 10], statistics.median(draws[v])))
    if cpw[v]:
        print("    CPWait " + " ".join("%s=%.0f" % (k.replace(" ", "_"), statistics.mean(x))
                                    for k, x in cpw[v].items()))
