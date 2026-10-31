"""usage: perfsum.py <simpleperf_dir> <perf.data> <symfs> [frames]  -> per-thread DSO split + top symbols."""
import sys, collections
sys.path.insert(0, sys.argv[1])
from simpleperf_report_lib import ReportLib
lib = ReportLib(); lib.SetSymfs(sys.argv[3]); lib.SetRecordFile(sys.argv[2])
frames = float(sys.argv[4]) if len(sys.argv) > 4 else 0
def cat(dso, sym):
    d = dso or ''
    if d.endswith('libe.so'): return 'emu'
    if 'kernel' in d or d.startswith('[kernel'): return 'kernel'
    if 'mali' in d.lower() or 'GLES' in d or 'vulkan' in d.lower(): return 'gpu-driver'
    if 'libc.so' in d or 'libc++' in d or 'libm.so' in d: return 'libc'
    if d.startswith('[anon') or d.startswith('//anon') or 'jit' in d.lower() or d == '' or 'unknown' in d: return 'JIT/anon'
    return 'other:' + d.split('/')[-1]
per = collections.defaultdict(collections.Counter); syms = collections.defaultdict(collections.Counter)
tot = collections.Counter(); period = collections.Counter(); n = 0
while True:
    s = lib.GetNextSample()
    if s is None: break
    sym = lib.GetSymbolOfCurrentSample()
    th = s.thread_comm
    c = cat(sym.dso_name, sym.symbol_name)
    per[th][c] += s.period; tot[th] += s.period
    nm = (sym.symbol_name or '?').split('(')[0][-70:]
    syms[th][c + ' ' + nm] += s.period
    n += 1
grand = sum(tot.values())
print(f"samples={n}  total={grand/1e9:.2f} s cpu" + (f"  per frame={grand/1e6/frames:.1f} ms" if frames else ""))
for th, t in tot.most_common(14):
    split = ' '.join(f"{k}={100*v/t:.0f}%" for k, v in per[th].most_common(5))
    pf = f" {t/1e6/frames:6.2f} ms/fr" if frames else ""
    print(f"\n[{th}] {100*t/grand:5.1f}% of process{pf} | {split}")
    for k, v in syms[th].most_common(8):
        print(f"     {100*v/t:5.1f}% {k}")
