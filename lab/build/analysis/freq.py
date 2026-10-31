#!/usr/bin/env python3
# freq.py <run folder> ...  -> per run: share of time each CPU cluster spends at max frequency and average frequency,
# from the cpufreq-start/cpufreq-end comparison, plus FPS, cores used and final temperature from result.json.
import sys, glob, json
def load(p):
    d = {}; cur = None
    for l in open(p):
        l = l.strip()
        if l.startswith('/sys'): cur = l.split('/')[-1]; d[cur] = {}
        elif cur and len(l.split()) == 2:
            f, t = l.split(); d[cur][int(f)] = int(t)
    return d
print('%-28s %6s %6s %6s | %s' % ('run', 'FPS', 'core', 'T end', 'cluster: avg GHz / % at max'))
for run in sys.argv[1:]:
    dd = glob.glob(run.rstrip('/') + '/*/')[0]
    a, b = load(dd + 'cpufreq-start.txt'), load(dd + 'cpufreq-end.txt')
    r = json.load(open(dd + 'result.json')) if glob.glob(dd + 'result.json') else {}
    cols = []
    for pol in sorted(b):
        dt = {f: b[pol][f] - a.get(pol, {}).get(f, 0) for f in b[pol]}; tot = sum(dt.values())
        if tot: cols.append('%s %.2f/%2.0f%%' % (pol[-1], sum(f * t for f, t in dt.items()) / tot / 1e6, 100 * dt[max(dt)] / tot))
    end = r.get('end', {}).get('temperature')
    print('%-28s %6s %6s %6s | %s' % (run.rstrip('/').split('/')[-1][:28], r.get('fps_median_gameplay_window'), r.get('cpu_cores_gameplay_window'), end / 10 if end else None, '  '.join(cols)))
