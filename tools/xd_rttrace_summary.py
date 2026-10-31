#!/usr/bin/env python3
"""Riassume le righe RTTRACE di un xe.log in una timeline compatta.

Uso: xd_rttrace_summary.py xe-after.log [--raw]
Raggruppa run di draw consecutivi con lo stesso set di RT in blocchi e riporta:
draw totali, draw con color write, draw con pipeline nulla, copertura Y di scissor
e viewport, window offset. Eventi xfer/rclear/resolve restano in ordine.
"""
import re
import sys

path = sys.argv[1]
raw = "--raw" in sys.argv
lines = [l.rstrip("\n") for l in open(path, errors="replace") if "RTTRACE" in l]
lines = [l[l.index("RTTRACE"):] for l in lines]
if raw:
    print("\n".join(lines))
    sys.exit()

run_re = re.compile(
    r"run first_draw=(\d+) count=(\d+) rts=\[d:(\S+) c0:(\S+) c1:(\S+) c2:(\S+) c3:(\S+)\] "
    r"scissor=\[(\d+),(\d+),(\d+),(\d+)\] woff=\[(-?\d+),(-?\d+),(\d)\] "
    r"vp=\[([-\d.e+]+),([-\d.e+]+),([-\d.e+]+),([-\d.e+]+),(\d+)\] "
    r"cmask=0x([0-9A-F]+) zctl=0x([0-9A-F]+) rast=(\d) prim=(\d+)x(\d+) pnull=(\d)")


def short(rt):
    return rt if rt == "-" else rt.split("@")[0] + ("@" + rt.split("@")[1] if "@" in rt else "")


block = None


def flush():
    global block
    if not block:
        return
    b = block
    print(f"  DRAWS rts=[d:{b['d']} c0:{b['c0']}{(' c1:' + b['c1']) if b['c1'] != '-' else ''}] "
          f"draws={b['n']} colorwrite={b['cw']} pnull={b['pn']} "
          f"scissorY=[{b['sy0']},{b['sy1']}) vpY=[{b['vy0']:.0f},{b['vy1']:.0f}] "
          f"woff={sorted(b['woff'])[:3]} zctl={sorted(b['z'])[:4]}")
    block = None


for l in lines:
    m = run_re.match(l.replace("RTTRACE ", "", 1))
    if m:
        g = m.groups()
        count = int(g[1])
        d, c0, c1 = short(g[2]), short(g[3]), short(g[4])
        sy0, sy1 = int(g[8]), int(g[10])
        ys, yo, vte = float(g[16]), float(g[17]), int(g[18])
        # VTE bit2 = VPORT_Y_SCALE_ENA, bit3 = VPORT_Y_OFFSET_ENA
        if (vte >> 2) & 1 and (vte >> 3) & 1:
            vy0, vy1 = yo - abs(ys), yo + abs(ys)
        else:
            vy0, vy1 = float("nan"), float("nan")
        cw = count if int(g[19], 16) else 0
        pn = count if g[24] == "1" else 0
        key = (d, c0, c1)
        if block is None or block["key"] != key:
            flush()
            block = dict(key=key, d=d, c0=c0, c1=c1, n=0, cw=0, pn=0, sy0=sy0, sy1=sy1,
                         vy0=vy0, vy1=vy1, woff=set(), z=set())
        b = block
        b["n"] += count
        b["cw"] += cw
        b["pn"] += pn
        b["sy0"], b["sy1"] = min(b["sy0"], sy0), max(b["sy1"], sy1)
        if vy0 == vy0:
            b["vy0"] = vy0 if b["vy0"] != b["vy0"] else min(b["vy0"], vy0)
            b["vy1"] = vy1 if b["vy1"] != b["vy1"] else max(b["vy1"], vy1)
        b["woff"].add((int(g[11]), int(g[12]), int(g[13])))
        b["z"].add(g[20])
        continue
    flush()
    print(l.replace("RTTRACE ", ""))
flush()
