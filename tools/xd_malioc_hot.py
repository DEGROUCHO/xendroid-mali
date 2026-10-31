#!/usr/bin/env python3
"""Mali-G720 static cost of the hottest guest vertex shaders.

Reads the VsHot lines (log_vertex_shader_hotness) of a run and the SPIR-V
dumped with dump_shaders, runs malioc on every dumped variant of each hot
shader and prints the per-vertex cycles of the IDVS position and varying
variants, weighted by indices/frame.
usage: xd_malioc_hot.py <evidence dir> <malioc> [top_n]
"""
import glob
import os
import re
import subprocess
import sys

ev, malioc = sys.argv[1], sys.argv[2]
top_n = int(sys.argv[3]) if len(sys.argv) > 3 else 12
hot = []
for line in open(os.path.join(ev, 'xe-after.log'), errors='replace'):
    m = re.search(r'VsHot: ([0-9A-F]{16}) (\d+) indices/frame ([\d.]+) draws', line)
    if m:
        hot.append((m.group(1), int(m.group(2)), float(m.group(3))))
# Keep the last report's entries (the latest gameplay window).
seen, last = set(), []
for h in reversed(hot):
    if h[0] not in seen:
        seen.add(h[0])
        last.append(h)
last.sort(key=lambda h: -h[1])
dump = os.path.join(ev, 'shaderdump')
rx = re.compile(r'(Position|Varying|Main) shader.*?Longest path cycles:\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+(\w+)', re.S)
for h, idx, draws in last[:top_n]:
    files = sorted(glob.glob(os.path.join(dump, f'shader_{h}_*.vulkan.bin.vert')))
    print(f'{h} {idx:8d} idx/frame {draws:6.1f} draws/frame, {len(files)} variants')
    for f in files[:3]:
        out = subprocess.run([malioc, '-c', 'Mali-G720', '--vulkan', '--vertex', f],
                             capture_output=True, text=True).stdout
        regs = re.search(r'Work registers: (\d+)', out)
        stack = re.search(r'Stack use: (\w+)', out)
        parts = [f'{m.group(1)[:3]} A={m.group(2)} LS={m.group(3)} T={m.group(4)} {m.group(5)}'
                 for m in rx.finditer(out)]
        print(f'    {os.path.basename(f)[23:40]} regs={regs.group(1) if regs else "?"} '
              f'stack={stack.group(1) if stack else "?"} | ' + ' | '.join(parts))
