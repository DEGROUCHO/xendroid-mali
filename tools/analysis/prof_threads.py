#!/usr/bin/env python3
"""On-CPU + off-CPU summary of a simpleperf --trace-offcpu recording.

Input: `simpleperf report-sample --show-callchain` text (with context_switch
records) and optionally a JIT map (perf-<pid>.map: "<hex start> <hex size>
<name>" per line). Prints, per thread group, on-CPU and off-CPU ms per second,
the top self frames (JIT code resolved through the map; driver/system code
grouped by library), and for selected groups the inclusive frames where they
block, weighted by blocked time.

usage: prof_threads.py <report.txt> [perf.map] [--group-detail NAME ...]
"""
import bisect
import collections
import re
import sys

args = sys.argv[1:]
report = args.pop(0)
jit_map = args.pop(0) if args and not args[0].startswith("--") else None
detail = []
if args and args[0] == "--group-detail":
    detail = args[1:]

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
    # simpleperf prints unmapped JIT code as "*unknown[+<hex addr>]": resolve it through the map too.
    m = re.match(r"\*unknown\[\+([0-9a-fA-F]+)\]", sym or "")
    if m and jit_starts:
        j = jit_lookup(int(m.group(1), 16))
        if j:
            return j
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

on = collections.Counter()
off = collections.Counter()
self_on = collections.defaultdict(collections.Counter)
block_inc = collections.defaultdict(collections.Counter)
block_self = collections.defaultdict(collections.Counter)
names = collections.defaultdict(set)
for c in samples:
    if c["k"] != "s" or "tid" not in c:
        continue
    g = group(c.get("name", "?"))
    names[g].add(c["tid"])
    labels = [frame_label(fr) for fr in c["frames"]]
    if c.get("ev", "").startswith("cpu-clock"):
        # cpu-clock event_count = ns covered by the sample (the runner records at 2000 Hz).
        on[g] += c.get("ns", 1e6)
        if labels:
            self_on[g][labels[0]] += c.get("ns", 1e6)
        continue
    ons = switch_on.get(c["tid"], [])
    i = bisect.bisect_right(ons, c["time"])
    if i >= len(ons):
        continue
    d = ons[i] - c["time"]
    off[g] += d
    if labels:
        block_self[g][labels[0]] += d
    for lab in set(labels):
        block_inc[g][lab] += d

print(f"span {span:.2f} s, {len(samples)} records")
print(f"{'group':26s} {'threads':>7s} {'on ms/s':>8s} {'off ms/s':>9s}")
for g, v in sorted(on.items(), key=lambda x: -x[1])[:16]:
    print(f"{g[:26]:26s} {len(names[g]):7d} {v / 1e6 / span:8.0f} {off[g] / 1e6 / span:9.0f}")

for g in ["CP", "encoder", "guest"] + detail:
    tot = sum(self_on[g].values())
    if not tot:
        continue
    print(f"\n=== {g}: on-CPU self frames ({tot / 1e6 / span:.0f} ms/s)")
    drv = sum(v for k, v in self_on[g].items() if k.startswith("DRIVER") or k.startswith("[libGLES") or "mali" in k)
    jit = sum(v for k, v in self_on[g].items() if k.startswith("JIT"))
    print(f"    driver share {100 * drv / tot:.1f}%   JIT share {100 * jit / tot:.1f}%")
    for k, v in self_on[g].most_common(30):
        print(f"  {100 * v / tot:5.1f}%  {k}")
for g in ["CP", "encoder", "guest"] + detail:
    tot = sum(block_self[g].values())
    if not tot:
        continue
    print(f"\n=== {g}: blocked {tot / 1e6 / span:.0f} ms/s, inclusive frames by blocked time")
    for k, v in block_inc[g].most_common(25):
        print(f"  {100 * v / tot:5.1f}%  {k}")

# Per thread: off-CPU time split by the top frame when the thread went off. A thread that leaves the CPU in
# the middle of guest JIT code or plain computation (no syscall/wait primitive on top) was preempted: it was
# runnable and lost its core (contention). Waiting primitives on top = a real wait.
WAITISH = ("syscall", "futex", "pthread_cond", "ppoll", "epoll", "nanosleep", "ioctl", "Wait", "sem_", "sleep",
           "[vdso]", "read", "write", "SignalChain")
pre = collections.Counter(); wait = collections.Counter(); ton = collections.Counter(); tname = {}
for c in samples:
    if c["k"] != "s" or "tid" not in c:
        continue
    tid = c["tid"]; tname[tid] = c.get("name", "?")
    if c.get("ev", "").startswith("cpu-clock"):
        ton[tid] += c.get("ns", 1e6); continue
    ons = switch_on.get(tid, [])
    i = bisect.bisect_right(ons, c["time"])
    if i >= len(ons):
        continue
    d = ons[i] - c["time"]
    top = frame_label(c["frames"][0]) if c["frames"] else "?"
    (wait if any(k in top for k in WAITISH) else pre)[tid] += d
print("\n=== per thread: on-CPU, preempted (lost the core mid-code), real wait (ms/s)")
for tid, v in ton.most_common(12):
    print(f"{tname[tid][:24]:24s} on {v / 1e6 / span:6.0f}  preempted {pre[tid] / 1e6 / span:5.0f}  wait {wait[tid] / 1e6 / span:5.0f}")
