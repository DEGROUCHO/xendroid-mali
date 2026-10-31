#!/usr/bin/env python3
"""One line per pkg_run game dir: fps, per-frame GPU work (perframe.py), pass
breaks and same-framebuffer re-entries per frame, pixel shader paths, in-pass
resolve stats. FROM/TO env restrict the counter window (see perframe.py).

usage: armcmp.py <game dir> [...]
"""
import re
import subprocess
import sys

here = __file__.rsplit('/', 1)[0]
for d in sys.argv[1:]:
    d = d.rstrip('/')
    pf = subprocess.run([sys.executable, here + '/perframe.py', d + '/'], capture_output=True, text=True).stdout.strip()
    log = open(d + '/session.log', errors='replace').read().splitlines()

    def last(tag):
        ls = [l for l in log if tag in l]
        return ls[-1] if ls else ''

    def per_frame_sum(tag):
        l = last(tag)
        if not l:
            return None
        body = l.split(':', 3)[-1]
        return round(sum(float(x) for x in re.findall(r'=([\d.]+)', body.split('offsets):')[-1] if 'offsets' in body else body)), 1)

    arm = d.split('/')[-2]
    print(f"== {arm}")
    print('  ' + pf)
    print(f"  pass breaks/frame {per_frame_sum('[RPBREAK]')}  same-fb re-entries/frame {per_frame_sum('[RPREENTRY]')}")
    for tag in ('PsPath:', 'VkInPass:', 'VkInPassReject:'):
        l = last(tag)
        if l:
            print('  ' + l[l.index(tag):][:300])
