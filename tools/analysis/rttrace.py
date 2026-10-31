#!/usr/bin/env python3
"""Summarize the one-shot RTTRACE frame(s) of an xe.log / session.log.

usage: rttrace.py <session.log>

Per traced frame:
- draw runs, and draws per render target binding;
- ownership transfers (xfer), split by destination and depth/color;
- resolves and resolve clears;
- the per-frame stencil bit transfer counters (StencilBitTransfers) seen
  anywhere in the log.
"""
import collections
import re
import sys

text = open(sys.argv[1], errors="replace").read().splitlines()
frames = []
cur = None
for line in text:
    m = re.search(r"RTTRACE (.*)$", line)
    if not m:
        continue
    body = m.group(1)
    if body.startswith("begin frame="):
        cur = {"frame": body.split("=")[1], "runs": [], "xfers": [],
               "resolves": [], "rclears": [], "other": collections.Counter()}
        frames.append(cur)
        continue
    if cur is None:
        continue
    if body.startswith("end "):
        cur["end"] = body
        cur = None
    elif body.startswith("run "):
        cur["runs"].append(body)
    elif body.startswith("xfer "):
        cur["xfers"].append(body)
    elif body.startswith("resolve "):
        cur["resolves"].append(body)
    elif body.startswith("rclear "):
        cur["rclears"].append(body)
    else:
        cur["other"][body.split()[0]] += 1

for f in frames:
    print(f"== frame {f['frame']} {f.get('end', '(no end)')}")
    draws = 0
    rt_draws = collections.Counter()
    for r in f["runs"]:
        c = int(re.search(r"count=(\d+)", r)[1])
        draws += c
        rts = re.search(r"rts=\[(.*?)\]", r)[1]
        rt_draws[rts] += c
    print(f"  runs {len(f['runs'])}  draws {draws}  binding changes ~{len(f['runs'])}")
    for rts, c in rt_draws.most_common(8):
        print(f"    {c:6d} draws  {rts[:110]}")
    dest = collections.Counter()
    for x in f["xfers"]:
        d = re.search(r"dest=(\S+)", x)[1]
        s = re.search(r"src=(\S+)", x)[1]
        dest[(d, s)] += 1
    print(f"  transfers {len(f['xfers'])}")
    for (d, s), c in dest.most_common(10):
        print(f"    {c:4d}  {s[:48]} -> {d[:48]}")
    print(f"  resolves {len(f['resolves'])}  resolve clears {len(f['rclears'])}  other {dict(f['other'])}")

sb = [l for l in text if "StencilBitTransfers:" in l]
if sb:
    print("stencil bit transfers (first, last):")
    print("  ", sb[0].split("StencilBitTransfers:")[1].strip())
    print("  ", sb[-1].split("StencilBitTransfers:")[1].strip())
