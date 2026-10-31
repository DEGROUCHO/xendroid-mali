#!/usr/bin/env python3
"""Cross-reference the [NTREAD] lines of xe-after.log with the textures in texdump/index.jsonl.

Usage: xd_ntread_vs_textures.py <evidence folder>
For each texture: how many disk reads cover the base and how many cover the mips.
Also prints a summary per file and per destination type.
"""
import collections
import json
import re
import sys

E = sys.argv[1]
rx = re.compile(r"\[NTREAD\] file=(\S*) dest=([0-9A-F]{8}) length=(\d+) offset=(-?\d+) read=(\d+) status=([0-9A-F]{8}) sync=(\S+)")
reads = []
for l in open(E + "/xe-after.log", errors="replace"):
    m = rx.search(l)
    if m:
        f, d, n, o, r, st, sy = m.groups()
        reads.append((f, int(d, 16), int(n), int(o), int(r), st, sy))
print("reads:", len(reads))


def phys(a):
    if a >= 0xE0000000:
        return a - 0xE0000000 + 0x1000
    if a >= 0xA0000000:
        return a & 0x1FFFFFFF
    return None


byfile = collections.Counter()
kinds = collections.Counter()
short = 0
for f, d, n, o, r, st, sy in reads:
    byfile[f] += n
    kinds["phys" if phys(d) is not None else "virt"] += 1
    if r != n or st != "00000000":
        short += 1
print("per file (byte):", byfile.most_common(8))
print("destinations:", dict(kinds), "short reads/errors:", short)
for f, d, n, o, r, st, sy in reads[:15]:
    print(f"  {f} dest={d:08X} len={n} off={o} read={r} st={st} sync={sy}")
print("  ...")
sizes = collections.Counter(n for _, _, n, _, _, _, _ in reads)
print("most common lengths:", sizes.most_common(8))

rows = [json.loads(l) for l in open(E + "/texdump/index.jsonl")]
pr = [(phys(d), n, f, o) for f, d, n, o, r, st, sy in reads if phys(d) is not None]
for r in rows[:30]:
    b0, b1 = r["base_page"] << 12, (r["base_page"] << 12) + r["base_size"]
    m0, m1 = r["mip_page"] << 12, (r["mip_page"] << 12) + r["mips_size"]
    hb = [x for x in pr if x[0] < b1 and x[0] + x[1] > b0]
    hm = [x for x in pr if x[0] < m1 and x[0] + x[1] > m0]
    print(f"{r['index']:2} {r['format_name'][:8]:8} {r['width']}x{r['height']} base@{b0:08X} reads_base={len(hb)} "
          f"reads_mips={len(hm)} {[(hex(x[0]), x[1], x[3]) for x in (hb + hm)[:3]]}")
