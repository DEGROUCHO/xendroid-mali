#!/usr/bin/env python3
"""Per-phase Mali counters per guest frame for a run with diag phases.

Reads <work>/<tag>_phase<i>-gpuctr.txt (GPUCTR lines, one per second from
xd_gpuctr) and the phase windows <tag>_phase<i>-counter-start/end.txt
("<uptime> <pid> f:<frame>"). Prints fps, GPU cycles per frame (and per
second, to see saturation), queue split, and the depth/stencil test funnel:
rasterised quads, early ZS tested/killed, late ZS tested/killed, FPK kills,
shaded quads.

usage: phase_ctr.py <work_dir> <tag> [<tag> ...]
"""
import re
import sys
from pathlib import Path

work = Path(sys.argv[1])
for tag in sys.argv[2:]:
    print(f"== {tag}")
    rows = []
    for i in range(16):
        f = work / f"{tag}_phase{i}-gpuctr.txt"
        if not f.exists():
            continue
        try:
            s = (work / f"{tag}_phase{i}-counter-start.txt").read_text().split()
            e = (work / f"{tag}_phase{i}-counter-end.txt").read_text().split()
            fps = (int(e[2][2:]) - int(s[2][2:])) / (float(e[0]) - float(s[0]))
        except (FileNotFoundError, IndexError, ValueError, ZeroDivisionError):
            continue
        tot, n = {}, 0
        for line in f.read_text(errors="replace").splitlines():
            if line.startswith("GPUCTR"):
                n += 1
                for k, v in re.findall(r"(\w+)=(\d+)", line):
                    tot[k] = tot.get(k, 0) + int(v)
        if not n or fps <= 0:
            continue

        def pf(k):
            return tot.get(k, 0) / n / fps / 1e6

        row = dict(i=i, fps=fps, gpu=pf("gpu"), gps=tot["gpu"] / n / 1e6,
                   main=pf("mainQ"), bin=pf("binQ"), comp=pf("compQ"),
                   fthr=pf("fThr"), shaded=pf("shadedQd"), rast=pf("rastQd"),
                   ezt=pf("ezsTest"), ezk=pf("ezsKill"), lzt=pf("lzsTest"),
                   lzk=pf("lzsKill"), fpk=pf("fpkKill"), prims=pf("prim"),
                   extrd=tot.get("extRd", 0) / n / fps / 1e6,
                   extwr=tot.get("extWr", 0) / n / fps / 1e6)
        rows.append(row)
        print(f" ph{i} fps={fps:5.1f} gpu={row['gpu']:5.2f}M/fr ({row['gps']:5.0f}M/s) "
              f"main={row['main']:5.2f} bin={row['bin']:4.2f} comp={row['comp']:4.2f} "
              f"fThr={row['fthr']:5.2f}M | quads rast={row['rast']:5.2f} "
              f"ezs {row['ezt']:4.2f}/{row['ezk']:4.2f} lzs {row['lzt']:4.2f}/{row['lzk']:4.2f} "
              f"fpk={row['fpk']:4.2f} shaded={row['shaded']:4.2f} | prims={row['prims']:4.2f}M "
              f"extRd={row['extrd']:4.0f}MB extWr={row['extwr']:4.0f}MB")
    if rows:
        m = lambda k: sum(r[k] for r in rows) / len(rows)
        print(f" mean fps={m('fps'):.1f} gpu={m('gpu'):.2f}M/fr ({m('gps'):.0f}M/s) "
              f"main={m('main'):.2f} bin={m('bin'):.2f} comp={m('comp'):.2f} "
              f"fThr={m('fthr'):.2f}M shaded={m('shaded'):.2f}M "
              f"lzs {m('lzt'):.2f}/{m('lzk'):.2f} ezs {m('ezt'):.2f}/{m('ezk'):.2f} "
              f"prims={m('prims'):.2f}M gpu/Mprim={m('gpu') / max(m('prims'), 1e-9):.2f}")
