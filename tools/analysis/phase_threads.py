#!/usr/bin/env python3
"""Per-phase thread CPU for a live A/B run with a /proc sampler.

Reads <work>/<tag>-sched.txt (blocks: "SAMPLE <uptime>", schedstat lines,
/proc/<pid>/task/<tid>/stat lines, "FREQ a b c") and the phase windows from
<work>/<tag>_phase<i>-counter-start.txt / -counter-end.txt ("<uptime> <pid>
f:<frame>"). For every phase prints fps (guest frames / seconds) and the CPU
of each busy thread in % of one core and in ms per guest frame, grouping the
guest CPU threads.

usage: phase_threads.py <work_dir> <tag> <arms e.g. ABBAAB>
"""
import collections
import re
import sys
from pathlib import Path

work, tag, arms = Path(sys.argv[1]), sys.argv[2], sys.argv[3]

samples = []
cur = None
for line in (work / f"{tag}-sched.txt").read_text(errors="replace").splitlines():
    if line.startswith("SAMPLE "):
        cur = {"t": float(line.split()[1]), "run": {}, "name": {}, "freq": None}
        samples.append(cur)
        continue
    if cur is None:
        continue
    m = re.match(r"/proc/\d+/task/(\d+)/schedstat:(\d+) (\d+) (\d+)", line)
    if m:
        cur["run"][int(m.group(1))] = int(m.group(2))
        continue
    m = re.match(r"(\d+) \((.*?)\) ", line)
    if m:
        cur["name"][int(m.group(1))] = m.group(2)
        continue
    if line.startswith("FREQ "):
        cur["freq"] = [int(x) for x in line.split()[1:] if x.isdigit()]


def window(i):
    s = (work / f"{tag}_phase{i}-counter-start.txt").read_text().split()
    e = (work / f"{tag}_phase{i}-counter-end.txt").read_text().split()
    return (float(s[0]), int(s[2][2:]), float(e[0]), int(e[2][2:]))


def group(name):
    if name.startswith("Guest CPU") or name.startswith("XThread"):
        return "guest (all)"
    return name


per_arm = collections.defaultdict(list)
for i, arm in enumerate(arms):
    try:
        t0, f0, t1, f1 = window(i)
    except (FileNotFoundError, IndexError, ValueError):
        continue
    inside = [s for s in samples if t0 - 1.0 <= s["t"] <= t1 + 1.0]
    if len(inside) < 2:
        print(f"phase {i} {arm}: not enough samples")
        continue
    a, b = inside[0], inside[-1]
    dt = b["t"] - a["t"]
    fps = (f1 - f0) / (t1 - t0)
    use = collections.Counter()
    for tid, run in b["run"].items():
        if tid in a["run"]:
            use[group(b["name"].get(tid, str(tid)))] += (run - a["run"][tid]) / 1e9 / dt * 100
    total = sum(use.values())
    freqs = [s["freq"] for s in inside if s["freq"]]
    fr = [round(sum(f[k] for f in freqs) / len(freqs) / 1000) for k in range(3)] if freqs else []
    top = use.most_common(8)
    per_arm[arm].append((fps, total, dict(use)))
    print(f"phase {i} {arm}: fps={fps:.1f} total={total:.0f}% core "
          f"({total / 100 * 1000 / fps:.1f} ms CPU/frame) freqMHz={fr}")
    for name, pct in top:
        print(f"    {name:24s} {pct:5.1f}%  {pct / 100 * 1000 / fps:5.2f} ms/frame")

print()
for arm, rows in sorted(per_arm.items()):
    n = len(rows)
    fps = sum(r[0] for r in rows) / n
    tot = sum(r[1] for r in rows) / n
    names = collections.Counter()
    for r in rows:
        for k, v in r[2].items():
            names[k] += v / n
    print(f"{arm}: n={n} fps={fps:.1f} cpu={tot:.0f}% ({tot / 100 * 1000 / fps:.1f} ms/frame)")
    for name, pct in names.most_common(6):
        print(f"    {name:24s} {pct:5.1f}%  {pct / 100 * 1000 / fps:5.2f} ms/frame")
