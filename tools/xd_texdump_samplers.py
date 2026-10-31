#!/usr/bin/env python3
"""Stampa per ogni texture del texdump: stato dei mip in memoria (zero/dati/ok),
stato dell'heap fisico per base e mip, e i campi dei fetch constant usati dal gioco.

Uso: xd_texdump_samplers.py <cartella texdump>
"""
import json
import sys

import numpy as np

D = sys.argv[1]
FILT = {0: "point", 1: "linear", 2: "basemap", 3: "fetch"}


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


for l in open(D + "/index.jsonl"):
    r = json.loads(l)
    raw = open(f"{D}/{r['file']}", "rb").read()
    bs, ms = r["base_size"], r["mips_size"]
    mips = np.frombuffer(raw[bs:bs + ms], np.uint8)
    heap = r.get("heap", [])
    hs = " ".join(f"[alloc@{h[1] >> 12:x}+{h[2] >> 12}p st={h[4]:x}]" for h in heap)
    print(f"{r['index']:2} {r['format_name'][:9]:9} {r['width']}x{r['height']} base={r['base_page']:x} "
          f"mip={r['mip_page']:x} mips_zero={np.mean(mips == 0):.2f} uses={r.get('sampler_uses')} {hs}")
    for v in r.get("samplers", []):
        d0, d1, d2, d3, d4, d5, b, sp, fc = v
        print(f"     fc{fc:2} mipf={FILT[(d3 >> 23) & 3]} instr_mipf={FILT[b & 15]} "
              f"min={(d4 >> 2) & 15} max={(d4 >> 6) & 15} lod_bias={sx((d4 >> 12) & 1023, 10) / 32:.2f} "
              f"tri_clamp={(d5 >> 3) & 3} aniso_bias={sx((d5 >> 5) & 15, 4)} packed={(d5 >> 11) & 1} "
              f"base={(d1 >> 12):x} mipaddr={(d5 >> 12):x} -> xenia min/max={sp & 255}/{(sp >> 8) & 255} basemap={(sp >> 16) & 1}")
