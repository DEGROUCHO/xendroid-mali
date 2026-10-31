#!/usr/bin/env python3
"""driverentry.py SAMPLES_TXT BUILD [thread_prefix]: from `simpleperf report-sample --show-callchain` output, the share of
the thread's samples spent in libGLES_mali.so grouped by the first libe.so caller (which of our calls enters the driver),
plus our caller two levels up. Symbolized with tools-pass/symcache/BUILD.so (tools-pass/sym.sh BUILD)."""
import collections, os, re, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
path, build = sys.argv[1], sys.argv[2]
prefix = sys.argv[3] if len(sys.argv) > 3 else 'GPU Commands'
samples, cur = [], None
for line in open(path, errors='replace'):
    line = line.strip()
    if line == 'sample:':
        cur = {'frames': []}; samples.append(cur)
    elif cur is not None and line.startswith('thread_name:'):
        cur['thread'] = line.split(':', 1)[1].strip()
    elif cur is not None and line.startswith('file:'):
        cur['frames'].append([line.split(':', 1)[1].strip(), None])
    elif cur is not None and line.startswith('vaddr_in_file:'):
        cur.setdefault('va', []).append(line.split(':', 1)[1].strip())
for s in samples:  # vaddr lines come before their file lines
    s['frames'] = [(f, v) for (f, _), v in zip(s['frames'], s.get('va', []))]
mine = [s for s in samples if s.get('thread', '').startswith(prefix)]
groups, total = collections.Counter(), len(mine)
in_driver = 0
for s in mine:
    fr = s['frames']
    if not fr or 'libGLES_mali' not in fr[0][0]:
        continue
    in_driver += 1
    ours = [v for f, v in fr if f.endswith('libe.so')]
    groups[tuple(ours[:2])] += 1
addrs = sorted({a for k in groups for a in k})
res = subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-symbolizer', '--obj=' + os.path.join(here, 'symcache', build + '.so')],
                     input='\n'.join('0x' + a for a in addrs) + '\n', capture_output=True, text=True).stdout
names = {a: b.splitlines()[0].split('(')[0].replace('xe::gpu::vulkan::', '').replace('xe::gpu::', '')[:55]
         for a, b in zip(addrs, res.strip().split('\n\n'))}
merged = collections.Counter()
for k, n in groups.items():
    merged[' <- '.join(names.get(a, a) for a in k)] += n
print(f'{prefix}: {total} samples, driver {in_driver} ({100 * in_driver / max(total, 1):.1f}%)')
for k, n in merged.most_common(25):
    print(f'  {100 * n / max(total, 1):5.1f}%  {k}')
