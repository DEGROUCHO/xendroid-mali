#!/usr/bin/env python3
# Applies offline to a Xenia config (toml) the UPDATE_from_* migrations from the source, with the same rule as
# Xenia (value == old default -> new default; UPDATE_from_any -> default), and moves defaults_date
# to the last migration. Used for the test BASE configs: otherwise on every Xenia start it re-migrates and cancels the
# injected options equal to an old default (JIT map bug, 18/10).
# usage: tools/dev/migrate_config.py IN.toml OUT.toml
import re, subprocess, sys
SRC = "/path/to/xendroid-ultimate-mali/src/xendroid-src/emulator-core/src/main/cpp/xenia/src/xenia"
inp, out = sys.argv[1], sys.argv[2]
text = open(inp).read()
m = re.search(r"(?m)^defaults_date\s*=\s*(\d+)", text); base = int(m.group(1)) if m else 0
src = subprocess.run(["grep", "-rhoE", r"UPDATE_from_[a-z0-9_]+\([^;]*\)", SRC], capture_output=True, text=True).stdout
ups = []
for l in src.splitlines():
    mm = re.match(r"UPDATE_from_(\w+)\((\w+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)(?:,\s*(.*))?\)", l)
    if not mm: continue
    t, n, y, mo, d, h, old = mm.groups()
    ups.append((int(y) * 1000000 + int(mo) * 10000 + int(d) * 100 + int(h), t, n, (old or "").strip()))
ups.sort()
def default_of(name):
    r = subprocess.run(["grep", "-rhoE", r"DEFINE_[a-z0-9_]+\(\s*" + name + r",\s*[^,]+", SRC], capture_output=True, text=True).stdout.split("\n")[0]
    return r.split(",", 1)[1].strip() if "," in r else None
norm = lambda v: v.strip().strip('"').strip("'").lower()
for date, t, n, old in ups:
    if date <= base: continue
    mm = re.search(r"(?m)^(" + n + r"\s*=\s*)([^#\n]*?)(\s*#|\s*$)", text)
    if not mm: print(f"{date} {n}: missing (default applies)"); continue
    new = default_of(n)
    if new is None: print(f"{date} {n}: default not found, left as is"); continue
    cur = mm.group(2).strip()
    if t == "any" or norm(cur) == norm(old):
        text = text[:mm.start(2)] + new + text[mm.end(2):]; print(f"{date} {n}: {cur} -> {new}")
    else: print(f"{date} {n}: {cur} kept (explicit choice, old default {old})")
last = ups[-1][0] if ups else base
if last > base:
    text = re.sub(r"(?m)^(defaults_date\s*=\s*)\d+", lambda x: x.group(1) + str(last), text, count=1)
open(out, "w").write(text); print(f"defaults_date {base} -> {max(base, last)}; written {out}")
