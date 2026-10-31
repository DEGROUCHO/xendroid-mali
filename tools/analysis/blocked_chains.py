#!/usr/bin/env python3
"""On-CPU + off-CPU summary of a simpleperf --trace-offcpu recording.

Input: `simpleperf report-sample --show-callchain` text (with context_switch
records) and optionally a JIT map (perf-<pid>.map: "<hex start> <hex size>
<name>" per line). Prints, per thread group, on-CPU and off-CPU ms per second,
the top self frames (JIT code resolved through the map; driver/system code
grouped by library), and for selected groups the inclusive frames where they
block, weighted by blocked time.

usage: blocked_chains.py <report.txt> [perf.map] --filter SUBSTR [--group G] [--on]
"""
import bisect
import collections
import re
import sys

args = sys.argv[1:]
report = args.pop(0)
jit_map = args.pop(0) if args and not args[0].startswith("--") else None
flt = args[args.index("--filter") + 1] if "--filter" in args else ""
grp = args[args.index("--group") + 1] if "--group" in args else "guest"
want_on = "--on" in args
callers = "--callers" in args
depth = int(args[args.index("--depth") + 1]) if "--depth" in args else 7

jit_starts, jit_ends, jit_names = [], [], []
if jit_map:
    rows = []
    for line in open(jit_map, errors="replace"):
        p = line.split(None, 2)
        if len(p) == 3:
            try:
                rows.append((int(p[0], 16), int(p[1], 16), p[2].strip()))
            except ValueError:
                pass
    rows.sort()
    for s, n, name in rows:
        jit_starts.append(s)
        jit_ends.append(s + n)
        jit_names.append(name)


def jit_lookup(ip):
    i = bisect.bisect_right(jit_starts, ip) - 1
    if i >= 0 and ip < jit_ends[i]:
        return "JIT " + jit_names[i]
    return None


def group(name):
    if name.startswith("Guest CPU"):
        return "guest"
    if name.startswith("GPU Commands"):
        return "CP"
    if name.startswith("GPU Encode"):
        return "encoder"
    return name


def frame_label(fr):
    sym, dso, ip = fr.get("sym", "unknown"), fr.get("dso", ""), fr.get("ip")
    if (sym == "unknown" or not sym) and ip is not None:
        j = jit_lookup(ip)
        if j:
            return j
    if sym in ("unknown", ""):
        base = dso.rsplit("/", 1)[-1] if dso else "?"
        return f"[{base}]"
    sym = re.sub(r"\(.*", "", sym)
    if dso.startswith("perf-") and dso.endswith(".map"):
        return "JIT " + sym[-60:]
    if "libGLES_mali" in dso or "vulkan.mali" in dso or "libmali" in dso:
        return "DRIVER " + sym[-60:]
    if dso.endswith("libe.so"):
        return sym[-90:]
    base = dso.rsplit("/", 1)[-1]
    return f"{base}:{sym[-50:]}"


samples = []
cur = None
frame = None
with open(report, errors="replace") as f:
    for line in f:
        t = line.strip()
        if line.startswith("context_switch:") or line.startswith("sample:"):
            if cur:
                samples.append(cur)
            cur = {"k": "cs" if line[0] == "c" else "s", "frames": []}
            frame = None
            continue
        if cur is None:
            continue
        if t.startswith("switch_on:"):
            cur["on"] = t.endswith("true")
        elif t.startswith("time:"):
            cur.setdefault("time", int(t.split()[1]))
        elif t.startswith("thread_id:"):
            cur["tid"] = int(t.split()[1])
        elif t.startswith("thread_name:"):
            cur["name"] = t.split(":", 1)[1].strip()
        elif t.startswith("event_count:"):
            cur["ns"] = int(t.split()[1])
        elif t.startswith("event_type:"):
            cur["ev"] = t.split(":", 1)[1].strip()
        elif t.startswith("vaddr_in_file:"):
            frame = {"vaddr": t.split(":", 1)[1].strip()}
            cur["frames"].append(frame)
        elif t.startswith("ip:") and frame is not None and "ip" not in frame:
            try:
                frame["ip"] = int(t.split(":", 1)[1].strip(), 16)
            except ValueError:
                pass
        elif t.startswith("file:") and frame is not None:
            frame["dso"] = t.split(":", 1)[1].strip()
        elif t.startswith("symbol:") and frame is not None:
            frame["sym"] = t.split(":", 1)[1].strip()
if cur:
    samples.append(cur)


switch_on = collections.defaultdict(list)
for c in samples:
    if c["k"] == "cs" and c.get("on") and "tid" in c:
        switch_on[c["tid"]].append(c["time"])
for v in switch_on.values():
    v.sort()
times = [c["time"] for c in samples if "time" in c]
span = (max(times) - min(times)) / 1e9 if times else 1.0
chains = collections.Counter(); total = 0.0
for c in samples:
    if c["k"] != "s" or "tid" not in c or group(c.get("name", "?")) != grp:
        continue
    labels = [frame_label(fr) for fr in c["frames"]]
    if want_on:
        if not c.get("ev", "").startswith("cpu-clock"):
            continue
        d = c.get("ns", 1e6)
    else:
        if c.get("ev", "").startswith("cpu-clock"):
            continue
        ons = switch_on.get(c["tid"], [])
        i = bisect.bisect_right(ons, c["time"])
        if i >= len(ons):
            continue
        d = ons[i] - c["time"]
    if flt and not any(flt in l for l in labels):
        continue
    total += d
    # leaf-first frames until (and including) the filter frame
    out = []
    if callers:
        k = next(i for i, l in enumerate(labels) if flt in l)
        out = [l[-70:] for l in labels[k:k + depth]]
    else:
        for l in labels:
            out.append(l[-70:])
            if flt and flt in l:
                break
    chains[" <- ".join(out[:14])] += d
print(f"{grp} {'on' if want_on else 'blocked'} with '{flt}': {total / 1e6 / span:.0f} ms/s")
for k, v in chains.most_common(12):
    print(f"{v / 1e6 / span:7.1f} ms/s  {k}\n")
