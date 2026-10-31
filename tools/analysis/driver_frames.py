#!/usr/bin/env python3
"""usage: driver_frames.py report.txt > driver-frames.txt
On-CPU samples whose leaf frame is in the Mali driver (libGLES_mali.so): per thread name, the leaf driver offsets
(vaddr_in_file) with ms/s, the driver entry frame closest to the emulator, and the first emulator (libe.so) caller."""
import collections, sys
samples, cur, fr = [], None, None
for line in open(sys.argv[1], errors="replace"):
    t = line.strip()
    if line.startswith(("sample:", "context_switch:")):
        if cur: samples.append(cur)
        cur = {"s": line[0] == "s", "frames": [], "ns": 1e6}; fr = None; continue
    if cur is None: continue
    k, _, v = t.partition(":"); v = v.strip()
    if k == "event_count": cur["ns"] = int(v.split()[0])
    elif k == "time": cur.setdefault("time", int(v.split()[0]))
    elif k == "thread_name": cur["name"] = v
    elif k == "event_type": cur["ev"] = v
    elif k == "vaddr_in_file": fr = {"vaddr": v}; cur["frames"].append(fr)
    elif k == "file" and fr is not None: fr["dso"] = v
    elif k == "symbol" and fr is not None: fr["sym"] = v
if cur: samples.append(cur)
times = [c["time"] for c in samples if "time" in c]; span = (max(times) - min(times)) / 1e9 if times else 1
leaf = collections.defaultdict(collections.Counter); entry = collections.defaultdict(collections.Counter)
caller = collections.defaultdict(collections.Counter); tot = collections.Counter()
def drv(f): return "libGLES_mali" in f.get("dso", "") or "vulkan.mali" in f.get("dso", "")
for c in samples:
    if not c["s"] or not c["frames"] or not c.get("ev", "").startswith("cpu-clock") or not drv(c["frames"][0]): continue
    n = c.get("name", "?").split("(")[0].strip()[:24]; w = c["ns"]; tot[n] += w
    leaf[n][c["frames"][0]["vaddr"]] += w
    last_drv = None
    for f in c["frames"]:
        if drv(f): last_drv = f
        else:
            caller[n][(f.get("sym") or f["vaddr"])[:90]] += w; break
    if last_drv: entry[n][last_drv["vaddr"] + " " + last_drv.get("sym", "")[:40]] += w
for n, v in tot.most_common():
    if v / 1e6 / span < 5: continue
    print(f"=== {n}: {v / 1e6 / span:.0f} ms/s in driver")
    print("  leaf offsets:"); [print(f"    {w / 1e6 / span:6.1f} ms/s  {k}") for k, w in leaf[n].most_common(15)]
    print("  outermost driver frame (entry):"); [print(f"    {w / 1e6 / span:6.1f} ms/s  {k}") for k, w in entry[n].most_common(10)]
    print("  emulator caller:"); [print(f"    {w / 1e6 / span:6.1f} ms/s  {k}") for k, w in caller[n].most_common(10)]
