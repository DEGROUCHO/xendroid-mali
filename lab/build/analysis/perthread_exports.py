#!/usr/bin/env python3
"""perthread_exports.py perf-report.txt.gz - per thread: share of CPU samples inside each guest kernel export
(first 'EXPORT:&xe::kernel::...::NAME_entry' frame of the chain) and inside pure JIT code."""
import gzip, sys, collections, re
per = collections.defaultdict(collections.Counter); tot = collections.Counter()
cur = None
def flush():
    if not cur or cur.get('ev') != 'cpu-clock' or not cur['frames']: return
    t = cur.get('thread', '?'); tot[t] += 1
    name = None
    for s in cur['frames']:
        if s.startswith('xe::cpu::Export*') and '&xe::' in s:
            m = re.search(r'&xe::kernel::\w+::(\w+)_entry', s); name = m[1] if m else s[s.find('&xe::'):][:50]; break
        m = re.match(r'xe::kernel::\w+::(\w+)_entry', s)
        if m: name = m[1]; break
    if name is None:
        top = cur['frames'][0]
        name = 'JIT' if top.startswith('sub_') or top == 'a64_host_code' else 'altro host'
    per[t][name] += 1
with gzip.open(sys.argv[1], 'rt', errors='replace') as f:
    for line in f:
        s = line.strip()
        if line.startswith('sample:'): flush(); cur = {'frames': []}
        elif line.startswith('context_switch:'): flush(); cur = None
        elif cur is not None:
            if s.startswith('event_type:'): cur['ev'] = s.split(': ', 1)[1]
            elif s.startswith('thread_name:'): cur['thread'] = s.split(': ', 1)[1]
            elif s.startswith('symbol:'): cur['frames'].append(s.split(': ', 1)[1])
flush()
total = sum(tot.values())
for t, n in tot.most_common(9):
    print(f'== {t}: {100*n/total:.1f}% of samples')
    for k, v in per[t].most_common(9): print(f'    {100*v/n:5.1f}%  {k}')
