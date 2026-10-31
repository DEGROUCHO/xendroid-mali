#!/usr/bin/env python3
"""Per-phase summary of a live A/B run (LIVE_AB phases with xd_gpuctr samples).

usage: live_ab.py <work_dir> <tag> <arm-per-phase, e.g. DBCAACBD>

Reads <tag>_phase<i>-{gpuctr.txt,counter-start.txt,counter-end.txt} and the
last phase's xe-mid log. Counter rows are normalised per guest frame using the
frame/uptime snapshots around each counter window (first two rows dropped).
"""
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

KEYS = ['gpu', 'core', 'fThr', 'nfThr', 'posThr', 'varThr', 'prim', 'vis',
        'extRd', 'extWr', 'lsWr', 'lsExtRd', 'cvtI', 'tiles', 'binQ', 'mainQ',
        'compQ', 'posHit', 'posMiss', 'varHit', 'varMiss', 'vcStall']


def snap(path):
    t, p, f = path.read_text().split()
    return float(t), int(p), int(f.split(':')[1])


def phase_stats(work, tag, i):
    a = snap(work / f'{tag}_phase{i}-counter-start.txt')
    b = snap(work / f'{tag}_phase{i}-counter-end.txt')
    if a[1] != b[1] or b[2] <= a[2]:
        return None
    fps = (b[2] - a[2]) / (b[0] - a[0])
    rows = []
    for line in (work / f'{tag}_phase{i}-gpuctr.txt').read_text().splitlines():
        if line.startswith('GPUCTR '):
            rows.append({k: float(v) for k, v in
                         (t.split('=') for t in line.split()[1:])})
    rows = rows[2:]
    if len(rows) < 2:
        return None
    out = {'fps': fps, 'frames': (a[2], b[2])}
    for k in KEYS:
        if k in rows[0]:
            out[k] = statistics.median(r[k] for r in rows) / fps
    return out


def main():
    work, tag, arms = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    by_arm = defaultdict(list)
    print(f'{"ph":>2} {"arm":>3} {"fps":>5} {"gpuM":>6} {"fThrM":>6} '
          f'{"posThr/prim":>11} {"posHit%":>7} {"primK":>6} {"lsWrMB":>6} '
          f'{"compQ%":>6}')
    for i, arm in enumerate(arms):
        s = phase_stats(work, tag, i)
        if not s:
            print(i, arm, 'no data')
            continue
        by_arm[arm].append(s)
        hit = (100 * s['posHit'] / (s['posHit'] + s['posMiss'])
               if s.get('posHit') is not None and s.get('posMiss') else float('nan'))
        print(f'{i:>2} {arm:>3} {s["fps"]:5.1f} {s["gpu"] / 1e6:6.2f} '
              f'{s["fThr"] / 1e6:6.2f} {s["posThr"] / s["prim"]:11.2f} '
              f'{hit:7.1f} {s["prim"] / 1e3:6.0f} {s["lsWr"] / 1e6:6.0f} '
              f'{100 * s["compQ"] / s["gpu"]:6.1f}')
    print('\nper arm (mean of phases; GPU normalised per 1M input prims too):')
    for arm, lst in sorted(by_arm.items()):
        m = {k: statistics.mean(s[k] for s in lst) for k in lst[0]
             if k not in ('frames',)}
        print(f'{arm}: n={len(lst)} fps={m["fps"]:.1f} gpu={m["gpu"] / 1e6:.2f}M '
              f'gpu/Mprim={m["gpu"] / m["prim"]:.2f} fThr={m["fThr"] / 1e6:.2f}M '
              f'posThr/prim={m["posThr"] / m["prim"]:.2f} '
              f'binQ={m["binQ"] / 1e6:.2f}M mainQ={m["mainQ"] / 1e6:.2f}M '
              f'compQ={m["compQ"] / 1e6:.2f}M lsWr={m["lsWr"] / 1e6:.0f}MB '
              f'extRd={m["extRd"] / 1e6:.0f}MB')

    # Render pass breaks and in-pass texture loads, attributed to the phase
    # whose frame window contains the log line.
    last = work / f'{tag}_phase{len(arms) - 1}-xe-mid.log'
    if not last.exists():
        return
    text = last.read_text(errors='replace').splitlines()
    windows = []
    for i, arm in enumerate(arms):
        s = phase_stats(work, tag, i)
        if s:
            windows.append((s['frames'][0], s['frames'][1], arm))

    def arm_of(frame):
        for lo, hi, arm in windows:
            if lo <= frame <= hi + 60:
                return arm
        return None

    breaks = defaultdict(list)
    loads = defaultdict(list)
    for line in text:
        m = re.search(r'f:(\d+)', line)
        if not m:
            continue
        arm = arm_of(int(m[1]))
        if not arm:
            continue
        if '[RPBREAK]' in line:
            vals = [float(v) for v in re.findall(r'=([\d.]+)', line.split('offsets):')[1])]
            breaks[arm].append(sum(vals))
        elif 'TexLoadInPass:' in line:
            loads[arm].append(line.split('TexLoadInPass:')[1].strip())
    for arm in sorted(breaks):
        print(f'{arm}: render pass breaks/frame ~{statistics.mean(breaks[arm]):.1f}')
    for arm in sorted(loads):
        print(f'{arm}: {loads[arm][-1][:600]}')


if __name__ == '__main__':
    main()
