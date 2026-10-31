#!/usr/bin/env python3
"""cpreport.py PREFIX.data BUILD THREAD_SUBSTRING [N=30]: offline self-time report of one thread of a profile
pulled by cpprof.py (cpprof prints only the 3 busiest threads; the CP thread is often 5th). Same symbolization."""
import subprocess, sys, collections, os, re
data, build, want = sys.argv[1], sys.argv[2], sys.argv[3]; top = int(sys.argv[4]) if len(sys.argv) > 4 else 30
here = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.expanduser('~/Library/Android/sdk/ndk/29.0.14206865/simpleperf/bin/darwin/x86_64/simpleperf')
rep = subprocess.run([HOST, 'report', '-i', data, '--sort', 'comm,dso,vaddr_in_file', '--percent-limit', '0.01', '-n'],
                     capture_output=True, text=True).stdout
per = collections.Counter(); total = 0.0
for line in rep.splitlines():
    m = re.match(r'\s*([0-9.]+)%\s+(\d+)\s+(.+?)\s{2,}(\S+)\s+(0x[0-9a-f]+)\s*$', line)
    if not m or want not in m.group(3): continue
    pct = float(m.group(1)); total += pct; per[(os.path.basename(m.group(4)), m.group(5))] += pct
libe = [va[2:] for (d, va) in per if d.startswith('libe')]
names = {}
if libe:
    subprocess.run(['bash', os.path.join(here, 'sym.sh'), build], capture_output=True)
    res = subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-symbolizer', '--obj=' + os.path.join(here, 'symcache', build + '.so')],
                         input='\n'.join('0x' + a for a in libe) + '\n', capture_output=True, text=True).stdout
    for a, b in zip(libe, res.strip().split('\n\n')):
        names[a] = b.splitlines()[0].split('(')[0].replace('xe::gpu::vulkan::', '').replace('xe::gpu::', '').replace('xe::', '')[:80]
func = collections.Counter()
for (d, va), p in per.items():
    func[names.get(va[2:], d) if d.startswith('libe') else d] += p
print(f'{want}: {total:.2f}% of all samples; self time, % of this thread')
for f, p in func.most_common(top):
    print(f'  {100 * p / total:5.1f}%  {f}')
