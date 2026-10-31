#!/usr/bin/env python3
"""Per-thread CPU / run-queue wait / core placement from /proc samples.

Input: file with blocks
  SAMPLE <uptime_seconds>
  /proc/<pid>/task/<tid>/schedstat:<run_ns> <wait_ns> <slices>
  <tid> (<comm>) <state> ... (full /proc/<pid>/task/<tid>/stat lines)
  FREQ <policy0_khz> <policy4_khz> <policy7_khz>
Only intervals where the busiest thread is >= min_cpu% are used (gameplay).
usage: xd_thread_sched.py <sched.txt> [min_cpu=50]
"""
import collections
import re
import sys

path = sys.argv[1]
min_cpu = float(sys.argv[2]) if len(sys.argv) > 2 else 50.0
samples = []
cur = None
for line in open(path, errors='replace'):
    line = line.rstrip('\n')
    if line.startswith('SAMPLE '):
        cur = {'t': float(line.split()[1]), 'run': {}, 'wait': {}, 'name': {},
               'cpu': {}, 'freq': None}
        samples.append(cur)
        continue
    if cur is None:
        continue
    m = re.match(r'/proc/\d+/task/(\d+)/schedstat:(\d+) (\d+) (\d+)', line)
    if m:
        tid = int(m.group(1))
        cur['run'][tid] = int(m.group(2))
        cur['wait'][tid] = int(m.group(3))
        continue
    if line.startswith('FREQ '):
        cur['freq'] = [int(x) for x in line.split()[1:]]
        continue
    if line.startswith('TEMP '):
        m = re.search(r'mValue=([0-9.]+)', line)
        if m:
            cur['temp'] = float(m.group(1))
        continue
    m = re.match(r'(\d+) \((.*)\) (.*)$', line)
    if m:
        tid = int(m.group(1))
        rest = m.group(3).split()
        cur['name'][tid] = m.group(2)
        if len(rest) > 36:
            cur['cpu'][tid] = int(rest[36])

rows = collections.defaultdict(lambda: {'run': [], 'wait': [], 'cpus': collections.Counter()})
used = 0
freqs = []
temps = []
for a, b in zip(samples, samples[1:]):
    dt = (b['t'] - a['t']) * 1e9
    if dt <= 0:
        continue
    loads = {}
    for tid, r in b['run'].items():
        if tid in a['run']:
            loads[tid] = ((r - a['run'][tid]) / dt * 100.0,
                          (b['wait'][tid] - a['wait'][tid]) / dt * 100.0)
    if not loads or max(v[0] for v in loads.values()) < min_cpu:
        continue
    used += 1
    if b['freq']:
        freqs.append(b['freq'])
    if b.get('temp'):
        temps.append(b['temp'])
    for tid, (run, wait) in loads.items():
        name = b['name'].get(tid, str(tid))
        key = name
        rows[key]['run'].append(run)
        rows[key]['wait'].append(wait)
        if tid in b['cpu']:
            rows[key]['cpus'][b['cpu'][tid]] += 1
print(f'{len(samples)} samples, {used} gameplay intervals (busiest >= {min_cpu}%)')
out = []
for k, v in rows.items():
    run = sorted(v['run'] + [0.0] * (used - len(v['run'])))
    wait = sorted(v['wait'] + [0.0] * (used - len(v['wait'])))
    out.append((run[len(run) // 2], run[int(len(run) * 0.9)],
                wait[len(wait) // 2], k, v['cpus']))
tot_run = 0.0
for med, p90, wmed, k, cpus in sorted(out, reverse=True)[:18]:
    tot_run += med
    little = sum(c for cpu, c in cpus.items() if cpu < 4)
    allc = sum(cpus.values()) or 1
    print(f'{k[:22]:22s} run med {med:5.1f}% p90 {p90:5.1f}% | rq-wait med {wmed:5.1f}% | on 2.1GHz cores {100*little/allc:3.0f}%')
if temps:
    temps.sort()
    print(f'SoC temp: median {temps[len(temps)//2]:.1f} C (min {temps[0]:.1f} max {temps[-1]:.1f})')
if freqs:
    for i, lab in enumerate(['cpu0-3', 'cpu4-6', 'cpu7']):
        f = sorted(x[i] for x in freqs if len(x) > i)
        if f:
            print(f'freq {lab}: median {f[len(f)//2]/1000:.0f} MHz (min {f[0]/1000:.0f} max {f[-1]/1000:.0f})')
