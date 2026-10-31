#!/usr/bin/env python3
"""cpprof.py OUT_PREFIX BUILD [wait_s=80] [secs=10]: profiles the running emulator with the device simpleperf
(the APK is profileable from the shell), pulls the samples and reports self time per function for the busiest
threads, symbolized with the build's snapshot symbols (tools-pass/sym.sh)."""
import subprocess, sys, time, collections, os, re
A = '/opt/homebrew/bin/adb'; pre, build = sys.argv[1], sys.argv[2]
wait_s = float(sys.argv[3]) if len(sys.argv) > 3 else 80; secs = int(sys.argv[4]) if len(sys.argv) > 4 else 10
here = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.expanduser('~/Library/Android/sdk/ndk/29.0.14206865/simpleperf/bin/darwin/x86_64/simpleperf')
def sh(c): return subprocess.run([A, 'shell', c], capture_output=True, text=True).stdout
pid = ''
REPORT_ONLY = os.environ.get('CPPROF_REPORT_ONLY') == '1'  # re-report an existing PREFIX.data
for _ in range(0 if REPORT_ONLY else 600):
    pid = sh('pidof xendroid.compose:emu').strip()
    if pid: break
    time.sleep(1)
if not pid and not REPORT_ONLY: open(pre + '.txt', 'w').write('no emulator process\n'); sys.exit()
# Savestate runs: sample 15 s after the restore (the run ends ~50 s after it);
# runs without a restore: after wait_s as before.
XE = '/sdcard/Android/data/xendroid.compose/files/compose/xe.log'
# xe.log accumulates across runs: wait for a restore line newer than the ones already there.
def restored_count():
    c = sh(f'grep -a -c "RuntimeState: restored" {XE}').strip()
    return int(c) if c.isdigit() else 0
base = 0 if REPORT_ONLY else restored_count()
t_end = time.time() + (0 if REPORT_ONLY else wait_s)
while time.time() < t_end:
    if restored_count() > base:
        time.sleep(15); break
    time.sleep(2)
# The savestate restore relaunches the emulator process: take the pid now.
if not REPORT_ONLY:
    pid = sh('pidof xendroid.compose:emu').strip().split()[0] if sh('pidof xendroid.compose:emu').strip() else pid
    # CPPROF_CALLGRAPH=1: dwarf call chains (which of our calls enters the driver, tools-pass/driverentry.py);
    # lower rate and 5 s to keep the file small.
    graph = '--call-graph dwarf,8192 -f 1000 --duration 5' if os.environ.get('CPPROF_CALLGRAPH') == '1' else f'-f 2000 --duration {secs}'
    rec = subprocess.run([A, 'shell', f'simpleperf record --app xendroid.compose -e cpu-clock {graph} -o /data/local/tmp/xdprof.data'],
                         capture_output=True, text=True)
    pulled = subprocess.run([A, 'pull', '/data/local/tmp/xdprof.data', pre + '.data'], capture_output=True, text=True)
    if pulled.returncode != 0:
        open(pre + '.txt', 'w').write(f'simpleperf record failed for pid {pid}:\n{rec.stdout}{rec.stderr}{pulled.stderr}\n')
        sys.exit()
    sh('rm -f /data/local/tmp/xdprof.data')
rep = subprocess.run([HOST, 'report', '-i', pre + '.data', '--sort', 'comm,dso,vaddr_in_file',
                      '--percent-limit', '0.05', '-n'], capture_output=True, text=True).stdout
rows = []; threads = collections.Counter()
for line in rep.splitlines():
    m = re.match(r'\s*([0-9.]+)%\s+(\d+)\s+(.+?)\s{2,}(\S+)\s+(0x[0-9a-f]+)\s*$', line)
    if not m: continue
    pct, n, comm, dso, va = float(m.group(1)), int(m.group(2)), m.group(3).strip(), m.group(4), m.group(5)
    rows.append((pct, comm, dso, va)); threads[comm] += pct
# -p PID from the adb shell has no permission on the (profileable, non-debuggable) app: the file came back
# without samples and the report was empty (24/10). --app goes through the profileable path.
if not rows:
    open(pre + '.txt', 'w').write(f'simpleperf: no samples for pid {pid}:\n{rec.stdout}{rec.stderr}\n{rep[:2000]}\n')
    sys.exit()
out = [f'pid {pid}, {secs}s, cpu-clock 2 kHz; share of all samples by thread:']
out += [f'  {p:5.1f}%  {c}' for c, p in threads.most_common(8)]
# The CP thread is the one that sets fps on draw-heavy games: always reported, even when not in the top 3.
for comm in list(dict.fromkeys([c for c, _ in threads.most_common(3)] + [c for c in threads if c.startswith('GPU Commands')])):
    per = collections.Counter(); offs = {}
    for pct, c, dso, va in rows:
        if c != comm: continue
        key = (os.path.basename(dso), va); per[key] += pct
    # Every libe address, symbolized in one llvm-symbolizer call (the top-60 cut undercounted functions).
    libe = [va[2:] for (d, va) in per if d.startswith('libe')]
    names = {}
    if libe:
        subprocess.run(['bash', os.path.join(here, 'sym.sh'), build], capture_output=True)  # unpacks symcache/<build>.so
        res = subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-symbolizer', '--obj=' + os.path.join(here, 'symcache', build + '.so')],
                             input='\n'.join('0x' + a for a in libe) + '\n', capture_output=True, text=True).stdout
        funcs = [b.splitlines()[0] for b in res.strip().split('\n\n')]
        for a, n in zip(libe, funcs):
            names[a] = n.split('(')[0].replace('xe::gpu::vulkan::', '').replace('xe::gpu::', '').replace('xe::', '')[:70]
    func = collections.Counter()
    for (d, va), p in per.items():
        func[names.get(va[2:], d) if d.startswith('libe') else d] += p
    out.append(f'\n== {comm}: top functions (self, % of all samples)')
    out += [f'  {p:5.2f}%  {f}' for f, p in func.most_common(25)]
open(pre + '.txt', 'w').write('\n'.join(out) + '\n')
