#!/usr/bin/env python3
"""ms/s per thread of the samples whose call chain (or own frame) contains a pattern.
usage: chain_cost.py report.txt PATTERN [PATTERN...]  (simpleperf report-sample text, weights by event_count)"""
import collections, sys

def main():
    path, pats = sys.argv[1], sys.argv[2:]
    cost = {p: collections.Counter() for p in pats}
    t0 = t1 = None
    cur = None

    def flush():
        if cur:
            for p in pats:
                if any(p in s for s in cur['syms']):
                    cost[p][cur['thr']] += cur['ev']

    for line in open(path, errors='replace'):
        if line.startswith('sample:'):
            flush()
            cur = {'syms': [], 'ev': 0, 'thr': '?'}
            continue
        if cur is None:
            continue
        if not line.startswith(' '):
            flush()
            cur = None
            continue
        s = line.strip()
        if s.startswith('event_count:') and not cur['ev']:
            cur['ev'] = int(s.split()[1])
        elif s.startswith('thread_name:'):
            cur['thr'] = s[13:]
        elif s.startswith('symbol:'):
            cur['syms'].append(s[8:])
        elif s.startswith('time:'):
            t = int(s.split()[1])
            t0 = t if t0 is None else min(t0, t)
            t1 = t if t1 is None else max(t1, t)
    flush()
    span = (t1 - t0) / 1e9 if t0 is not None and t1 > t0 else 1.0
    for p in pats:
        total = sum(cost[p].values()) / 1e6 / span
        print(f'{p}: {total:.1f} ms/s total over {span:.2f} s')
        for thr, ev in cost[p].most_common(6):
            print(f'  {ev / 1e6 / span:7.1f} ms/s  {thr}')

if __name__ == '__main__':
    main()
