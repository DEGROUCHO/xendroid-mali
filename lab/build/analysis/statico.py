#!/usr/bin/env python3
# statico.py <run>...  : GPU work per frame in the measurement window of still-scene runs (medians).
import sys, re, glob, json, statistics as st
print('%-28s %6s %7s %9s %9s %9s %8s %8s %8s %6s' % ('run', 'FPS', 'draw/f','GPU Mc/f', 'GPU Mc/s', 'quad M/f', 'lzsK M/f', 'rd MB/f', 'wr MB/f', 'core'))
for run in sys.argv[1:]:
    try:
        d = glob.glob(run + '/*/')[0]; r = json.load(open(d + 'result.json'))
        ctr = [dict((k, int(v)) for k, v in re.findall(r'(\w+)=(-?\d+)', l)) for l in open(d + 'gpuctr.txt', errors='replace') if l.startswith('GPUCTR')]
        ctr = ctr[5:-2]; fp = r['fps_median_gameplay_window']
        m = lambda k: st.median([x.get(k, 0) for x in ctr]) / 1e6
        print('%-28s %6.1f %7d %9.1f %9.0f %9.2f %8.2f %8.0f %8.0f %6.2f' % (run.rstrip('/').split('/')[-1], fp, r['draws_per_frame_median'], m('gpu') / fp, m('gpu'), m('shadedQd') / fp, m('lzsKill') / fp, m('extRd') / fp, m('extWr') / fp, r['cpu_cores_gameplay_window']))
    except Exception as e:
        print('%-28s %s' % (run.split('/')[-1], e))
