#!/usr/bin/env python3
"""RAM summary of a pkg_run game directory: the last meminfo (PSS, swap, native
heap, GL mtrack) and the last log_vk_memory lines (Vulkan memory by site, driver
CPU memory by object kind, shader modules / pipeline slots, pipeline eviction,
texture heap).

usage: memreport.py <game dir> [...]
"""
import pathlib
import re
import sys

for d in sys.argv[1:]:
    d = pathlib.Path(d)
    print(f"== {d.parent.name}/{d.name}")
    infos = sorted(d.glob('meminfo-*.txt'), key=lambda p: int(re.findall(r'\d+', p.name)[0]))
    for p in infos[-1:]:
        t = p.read_text(errors='replace')
        def row(name):
            m = re.search(r'^\s*' + re.escape(name) + r'\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)', t, re.M)
            return f"{int(m[1]) // 1024} MB (swap {int(m[4]) // 1024})" if m else '?'
        tot = re.search(r'TOTAL PSS:\s*(\d+).*TOTAL SWAP PSS:\s*(\d+)', t)
        print(f"  {p.name}: PSS {int(tot[1]) // 1024 if tot else '?'} MB + swap {int(tot[2]) // 1024 if tot else '?'} MB | "
              f"native heap {row('Native Heap')} | GL mtrack {row('GL mtrack')} | EGL {row('EGL mtrack')}")
    log = (d / 'session.log').read_text(errors='replace').splitlines() if (d / 'session.log').exists() else []
    for tag in ('VkMemory: total', 'VkHostMemory', 'ShaderModules:', 'PipelineEviction', 'TextureHeap'):
        ls = [l for l in log if tag in l]
        if ls:
            l = ls[-1]
            print('  ' + l[l.index(tag):][:420])
