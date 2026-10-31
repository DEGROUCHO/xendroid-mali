#!/usr/bin/env python3
"""Estimate the per-frame cost of EDRAM ownership transfers in an RTTRACE frame.

usage: xfercost.py <session.log>

Per transfer: samples moved (tiles x 1280 for 32bpp) and fragment passes
(depth destination without stencil export: 1 depth pass + 8 stencil-bit
passes; color destination: 1 pass). Prints the totals per frame, the split
by kind (color->depth, depth->depth, ...), and the MSAA alias round trips
(same base, different sample count).
"""
import collections
import re
import sys

lines = [l.split("RTTRACE ", 1)[1].rstrip() for l in open(sys.argv[1], errors="replace") if "RTTRACE " in l]
is_depth = {}
for l in lines:
    m = re.match(r"claim slot=\d+ key=(\d+)/(\d+)/(\d+)/msaa(\d)/depth(\d)", l)
    if m:
        b, p, f, s, d = map(int, m.groups())
        is_depth[(b, p, f, s)] = d
frame = None
tot = collections.Counter()
kinds = collections.Counter()
rows = []
for l in lines:
    if l.startswith("begin frame="):
        frame = l
        continue
    m = re.match(r"xfer slot=\d+ dest=(\d+)/(\d+)/(\d+)/(\d)@\S+ src=(\d+)/(\d+)/(\d+)/(\d)@\S+ tiles=\[(\d+),(\d+)\) path=(\S+)", l)
    if not m:
        continue
    db, dp, df, dm, sb, sp, sf, sm, a, b = map(int, m.groups()[:10])
    path = m.group(11)
    dd = is_depth.get((db, dp, df, dm))
    sd = is_depth.get((sb, sp, sf, sm))
    samples = (b - a) * 1280
    passes = 9 if dd else 1
    kind = f"{'depth' if sd else 'color'}->{'depth' if dd else 'color'}"
    alias = "msaa-alias" if (db == sb and dp == dp and dm != sm) else ""
    rows.append((kind, samples, passes, dm, sm, alias, path, f"{sb}/{sp}/{sf}/{sm} -> {db}/{dp}/{df}/{dm} [{a},{b})"))
    kinds[kind] += samples * passes
    tot["invocations"] += samples * passes
    tot["samples"] += samples
    if alias:
        tot["alias_invocations"] += samples * passes
for r in rows:
    print(f"  {r[0]:14s} {r[1]/1e3:7.0f}k samples x{r[2]}  {r[5]:10s} {r[6]:9s} {r[7]}")
print("total fragment invocations per frame (approx): %.1f M" % (tot["invocations"] / 1e6))
print("  of which MSAA alias round trips: %.1f M" % (tot["alias_invocations"] / 1e6))
for k, v in kinds.most_common():
    print(f"  {k:14s} {v/1e6:6.1f} M")
# Cutouts (rt_cache_draw_overwrite_cutouts / clear chains): samples not moved.
saved = collections.Counter()
for l in lines:
    m = re.match(r"cutout mask=0x([0-9A-F]+) .* samples=(\d+)/(\d+)", l)
    if m:
        saved[int(m.group(1), 16)] += int(m.group(3)) - int(m.group(2))
for mask, n in saved.items():
    passes = 9 if mask == 1 else 1
    print(f"  cutout mask 0x{mask:X}: {n/1e3:.0f}k samples skipped (~{n*passes/1e6:.1f} M invocations{' if depth' if mask & 1 and mask != 1 else ''})")
chains = [l for l in lines if l.startswith("draw_chain")]
retargets = [l for l in lines if l.startswith("clear_new_owner") or l.startswith("rclear")]
print(f"  draw_chain lines {len(chains)}, retarget/clear lines {len(retargets)}")
# Clear chains: per flush, main_samples=kept/original. Slot 0 is the depth slot;
# its stencil-bit passes (8) are saved too when the stencil cutout is set
# (approximation: the stencil cutout matches the main one).
chain_saved = 0
for l in lines:
    m = re.match(r"draw_chain flush slot=(\d+) .* stencil_cutout=(\d) .* main_samples=(\d+)/(\d+)", l)
    if m:
        slot, sc, kept, orig = map(int, m.groups())
        passes = (9 if sc else 1) if slot == 0 else 1
        chain_saved += (orig - kept) * passes
if chain_saved:
    print(f"  draw_chain saved ~{chain_saved/1e6:.1f} M invocations -> effective ~{(tot['invocations']-chain_saved)/1e6:.1f} M per frame")
