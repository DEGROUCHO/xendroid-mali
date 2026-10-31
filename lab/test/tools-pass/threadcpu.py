#!/usr/bin/env python3
"""threadcpu.py OUT.txt [wait_s=60] [window_s=10]: waits for the emulator process, waits wait_s, then samples
/proc/<pid>/task/*/stat over window_s and writes per-thread CPU (cores) sorted, by thread name."""
import subprocess, sys, time, re
A = '/opt/homebrew/bin/adb'; out = sys.argv[1]
wait_s = float(sys.argv[2]) if len(sys.argv) > 2 else 60; win = float(sys.argv[3]) if len(sys.argv) > 3 else 10
def sh(c): return subprocess.run([A, 'shell', c], capture_output=True, text=True).stdout
pid = ''
for _ in range(600):
    pid = sh('pidof xendroid.compose:emu').strip()
    if pid: break
    time.sleep(1)
if not pid: open(out, 'w').write('no emulator process\n'); sys.exit()
# Savestate runs: sample 15 s after the restore (the run ends ~50 s after it);
# runs without a restore: after wait_s as before.
XE = '/sdcard/Android/data/xendroid.compose/files/compose/xe.log'
# xe.log accumulates across runs: wait for a restore line newer than the ones already there.
def restored_count():
    c = sh(f'grep -a -c "RuntimeState: restored" {XE}').strip()
    return int(c) if c.isdigit() else 0
base = restored_count()
t_end = time.time() + wait_s
while time.time() < t_end:
    if restored_count() > base:
        time.sleep(15); break
    time.sleep(2)
# The savestate restore relaunches the emulator process: take the pid now.
pid = sh('pidof xendroid.compose:emu').strip().split()[0] if sh('pidof xendroid.compose:emu').strip() else pid
def snap():
    txt = sh(f'for t in /proc/{pid}/task/*; do cat $t/stat 2>/dev/null; echo; done')
    r = {}
    for line in txt.splitlines():
        m = re.match(r'(\d+) \((.*)\) (.*)', line)
        if not m: continue
        f = m.group(3).split()
        r[m.group(1)] = (m.group(2), int(f[11]) + int(f[12]))  # utime + stime (after the last ')')
    return r
a = snap(); t0 = time.time(); time.sleep(win); b = snap(); dt = time.time() - t0
hz = 100.0
rows = []
for tid, (name, ticks) in b.items():
    if tid in a: rows.append(((ticks - a[tid][1]) / hz / dt, name, tid))
rows.sort(reverse=True)
with open(out, 'w') as f:
    f.write(f'pid {pid} window {dt:.1f}s total {sum(r[0] for r in rows):.2f} cores\n')
    for c, n, t in rows[:25]: f.write(f'{c:5.2f}  {n}  ({t})\n')
