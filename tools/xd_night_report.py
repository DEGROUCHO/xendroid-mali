#!/usr/bin/env python3
"""Summarizes a batch of xd_capture_safe runs: guest fps / GPU time (via
xd_perf_summary), RTCLEAR and FrameGen log lines, a contact sheet of the guest
output captures, and - for runs with a screenrecord.mp4 - the rate of distinct
presented frames plus a strip of consecutive recorded frames.

usage: xd_night_report.py <out_dir> <tag> [<tag> ...]
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EV = os.path.join(ROOT, 'evidence', '2026-09-27-guest-output')


def perf_line(tag):
    try:
        return subprocess.check_output(
            [sys.executable, os.path.join(ROOT, 'tools', 'xd_perf_summary.py'),
             os.path.join(EV, tag)], text=True).strip()
    except Exception as e:  # noqa: BLE001 - report and continue
        return f'{tag}: perf summary failed ({e})'


def log_lines(tag, needle, limit):
    path = os.path.join(EV, tag, 'xe-after.log')
    if not os.path.exists(path):
        return []
    out = []
    with open(path, errors='replace') as f:
        for line in f:
            if needle in line:
                out.append(line.rstrip()[:220])
    return out[-limit:]


def presented_rate(mp4, out_png):
    """Distinct frames per second in the recording, and a strip of frames."""
    tmp = mp4 + '.frames'
    os.makedirs(tmp, exist_ok=True)
    for f in os.listdir(tmp):
        os.remove(os.path.join(tmp, f))
    # Every recorded frame, as is (screenrecord only emits frames on change).
    subprocess.run(['ffmpeg', '-v', 'error', '-i', mp4, '-vsync', '0',
                    '-vf', 'scale=640:-1', os.path.join(tmp, 'f%05d.png')],
                   check=False)
    probe = subprocess.run(
        ['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
         'frame=pts_time', '-of', 'csv=p=0', mp4],
        capture_output=True, text=True).stdout.split()
    times = [float(t) for t in probe if t.replace('.', '', 1).isdigit()]
    frames = sorted(os.listdir(tmp))
    # Lossy encoding perturbs repeated frames slightly - count a frame as new
    # only if it differs from the previous one by more than the codec noise.
    distinct = 0
    last = None
    for name in frames:
        cur = np.asarray(Image.open(os.path.join(tmp, name)).convert('L'),
                         dtype=np.float32)
        if last is None or np.abs(cur - last).mean() > 0.6:
            distinct += 1
        last = cur
    duration = (times[-1] - times[0]) if len(times) > 1 else 0.0
    rate = distinct / duration if duration > 0 else 0.0
    # Strip of 8 consecutive frames from the middle.
    mid = max(0, len(frames) // 2 - 4)
    strip = [Image.open(os.path.join(tmp, n)) for n in frames[mid:mid + 8]]
    if strip:
        w, h = strip[0].size
        sheet = Image.new('RGB', (w * 4, h * 2))
        for i, im in enumerate(strip):
            sheet.paste(im, ((i % 4) * w, (i // 4) * h))
        sheet.save(out_png)
    return distinct, duration, rate


def main():
    out_dir = sys.argv[1]
    tags = sys.argv[2:]
    os.makedirs(out_dir, exist_ok=True)
    lines = []
    thumbs = []
    for tag in tags:
        lines.append(perf_line(tag))
        for l in log_lines(tag, 'FrameGen:', 2):
            lines.append('    ' + l)
        rtclear = log_lines(tag, 'RTCLEAR', 1000)
        if rtclear:
            lines.append(f'    RTCLEAR lines: {len(rtclear)}; last: {rtclear[-1]}')
        mp4 = os.path.join(EV, tag, 'screenrecord.mp4')
        if os.path.exists(mp4):
            distinct, duration, rate = presented_rate(
                mp4, os.path.join(out_dir, f'{tag}_strip.png'))
            lines.append(f'    screenrecord: {distinct} distinct frames in '
                         f'{duration:.1f}s = {rate:.1f}/s')
        caps = [f for f in os.listdir(os.path.join(EV, tag))
                if f.startswith('guest-output-pre-present')] if os.path.isdir(
                    os.path.join(EV, tag)) else []
        if caps:
            im = Image.open(os.path.join(EV, tag, caps[0])).convert('RGB')
            im.thumbnail((640, 360))
            thumbs.append((tag, im))
    if thumbs:
        cols = 3
        rows = (len(thumbs) + cols - 1) // cols
        sheet = Image.new('RGB', (640 * cols, 380 * rows), 'black')
        from PIL import ImageDraw
        d = ImageDraw.Draw(sheet)
        for i, (tag, im) in enumerate(thumbs):
            x, y = (i % cols) * 640, (i // cols) * 380
            sheet.paste(im, (x, y + 20))
            d.text((x + 4, y + 4), tag, fill='yellow')
        sheet.save(os.path.join(out_dir, 'captures.png'))
    report = '\n'.join(lines)
    with open(os.path.join(out_dir, 'report.txt'), 'w') as f:
        f.write(report + '\n')
    print(report)


if __name__ == '__main__':
    main()
