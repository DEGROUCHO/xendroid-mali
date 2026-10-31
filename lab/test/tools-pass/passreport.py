#!/usr/bin/env python3
"""passreport.py DIAG.log qNNN : one-page report of a frame's render passes from the byte ledger
([PBYTES] f=3) - closes, summary bytes, consumers of stored bytes, adjacent reentries by cause (symbolized)."""
import sys, re, collections, subprocess, os
log, build = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))
ALL = [l for l in open(log, errors='ignore') if '[PBYTES] f=' in l]
# Report the last ledger frame (f=N: frames counted from boot, or from when a live cvar armed the ledger in
# gameplay); its event lines come after the previous summary and end with its own summary.
end = max((i for i, l in enumerate(ALL) if re.search(r'\[PBYTES\] f=\d+ summary', l)), default=-1)
F = re.search(r'\[PBYTES\] f=(\d+) ', ALL[end]).group(1) if end >= 0 else '3'
begin = max((i for i, l in enumerate(ALL[:end]) if ' summary' in l), default=-1)
L = [l for l in ALL[begin + 1:end + 1] if f'[PBYTES] f={F} ' in l]
summ = ALL[end] if end >= 0 else ''
vals = dict(re.findall(r' (load|store|resolve|ownership|copy|upload|total|reentry)=([0-9.]+)', summ))
print('summary MB:', ' '.join(f'{k}={v}' for k, v in vals.items()))
passes = collections.OrderedDict(); cur = None; cons = collections.Counter()
for l in L:
    if 'summary' in l: continue
    d = dict(re.findall(r'(\w+)=(\S+)', l.split('[PBYTES]')[1]))
    if 'kind' not in d:
        if cur is not None and d.get('class') not in (None, 'None'): cons[d['class']] += int(d.get('bytes', 0))
        continue
    k = int(d['kind']); cur = k if k == 1 else None
    P = passes.setdefault(d['p'], {'load': {}, 'store': {}, 'close': None, 'pusher': '0', 'rpb': '0/0'})
    key = (d['image'], d['aspect'])
    if k == 0: P['load'][key] = int(d['read'])
    if k == 1: P['store'][key] = int(d['write'])
    if k == 7: P['close'] = d.get('close'); P['pusher'] = d.get('pusher', '0'); P['rpb'] = d.get('rpb', '0/0')
print('closes:', sum(1 for P in passes.values() if P['close']))
print('stored bytes -> next consumer:', ', '.join(f'{c} {v/1e6:.1f}' for c, v in cons.most_common()))
addrs = sorted({a for P in passes.values() for a in [P['pusher'], P['rpb'].split('/')[-1]] if a and a != '0'})
names = {}
if addrs:
    out = subprocess.run(['bash', os.path.join(here, 'sym.sh'), build] + addrs, capture_output=True, text=True).stdout
    for line in out.splitlines():
        a, _, n = line.partition(' '); names[a] = n.replace('xe::gpu::vulkan::', '').replace('xe::gpu::', '')[:60]
keys = list(passes); agg = collections.Counter(); cnt = collections.Counter()
for i, p in enumerate(keys[:-1]):
    P, N = passes[p], passes[keys[i + 1]]
    same = [k for k in P['store'] if k in N['load']]
    if not same: continue
    mb = sum(P['store'][k] + N['load'][k] for k in same) / 1e6
    cause = f"{P['close']} | {names.get(P['pusher'], P['pusher'])} | via {names.get(P['rpb'].split('/')[-1], '-')}"
    agg[cause] += mb; cnt[cause] += 1
print(f'adjacent reentries {sum(agg.values()):.1f} MB over {sum(cnt.values())} breaks:')
for c, v in agg.most_common(): print(f'  {v:6.1f} MB x{cnt[c]:2}  {c}')
for tag in ('XFERPASS', 'TEXHEAD', 'VkRTTexture'):
    last = [l for l in open(log, errors='ignore') if tag in l][-1:]
    if last: print(tag + ':', last[0].split(tag)[-1].strip()[:300])
