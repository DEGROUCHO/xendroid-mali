"""Summarise VkPassSeq lines: per logged submission pair, the ordered passes + totals by kind."""
import re, sys, collections
rx = re.compile(r'VkPassSeq sub=(\d+) #(\d+) (xfer )?(\d+)x(\d+) draws=(\d+) sc=(\d+)x(\d+) vp=(\d+)x(\d+) gpu=([\d.]+)ms gap_before=([\d.]+)ms')
subs = collections.OrderedDict()
for line in open(sys.argv[1], errors='replace'):
    m = rx.search(line)
    if m:
        sub = int(m.group(1))
        subs.setdefault(sub, []).append(m)
verbose = len(sys.argv) > 2
groups = []  # consecutive submissions (sub, sub+1) = ~1 frame
keys = list(subs)
i = 0
while i < len(keys):
    g = [keys[i]]
    if i + 1 < len(keys) and keys[i + 1] == keys[i] + 1:
        g.append(keys[i + 1]); i += 1
    groups.append(g); i += 1
agg = collections.defaultdict(lambda: [0.0, 0, 0, 0.0])  # gpu ms, passes, draws, gap ms
nframes = 0
for g in groups:
    if len(g) < 2:
        continue
    nframes += 1
    for sub in g:
        for m in subs[sub]:
            kind = ('xfer ' if m.group(3) else '') + f"{m.group(4)}x{m.group(5)}"
            a = agg[kind]; a[0] += float(m.group(11)); a[1] += 1; a[2] += int(m.group(6)); a[3] += float(m.group(12))
    if verbose and nframes <= int(sys.argv[2]):
        print(f"--- frame (subs {g})")
        for sub in g:
            for m in subs[sub]:
                print(f"  s{sub}#{m.group(2):>2} {('xfer ' if m.group(3) else ''):5}{m.group(4)}x{m.group(5):<5} draws={m.group(6):>5} sc={m.group(7)}x{m.group(8):<5} gpu={float(m.group(11)):6.3f} gap={float(m.group(12)):6.3f}")
print(f"frames(sub pairs)={nframes}")
tot = 0; totgap = 0
for kind, (ms, n, d, gap) in sorted(agg.items(), key=lambda kv: -kv[1][0]):
    print(f"  {kind:16} {ms/nframes:6.2f} ms/fr  passes {n/nframes:5.1f}  draws {d/nframes:7.0f}  gaps-before {gap/nframes:6.2f} ms/fr")
    tot += ms; totgap += gap
print(f"  TOTAL passes {tot/nframes:.2f} ms/fr + gaps {totgap/nframes:.2f} ms/fr")
