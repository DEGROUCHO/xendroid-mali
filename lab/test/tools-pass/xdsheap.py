#!/usr/bin/env python3
"""xdsheap.py SLOT.xds [HEAP_BASE_HEX=80000000] : offline check of a XenDroid savestate (format 2).
Prints the kernel modules (boot objects: handle, name, LDR/header in kernel v2) and the region map of one
guest heap (state 0 free, 1 reserved, 3 committed). Needs the zstd CLI. Written 24/10 for the Reach restore:
the q537 save had the 0x80000000 image heap FREE except the last DLL (XexModule::ReadImage reset the heap)."""
import struct, subprocess, sys

path = sys.argv[1]
want = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x80000000
d = open(path, 'rb').read()
assert d[:8] == b'XDSTATE\0', 'not a savestate'
o = 124  # magic, version, endian, title, page size, 3 hashes, section count
entries = []
for _ in range(6):
    sid, codec, raw, stored, crc = struct.unpack_from('<IIQQI', d, o); o += 28
    entries.append((sid, codec, raw, stored))
sections = {}
for sid, codec, raw, stored in entries:
    blob = d[o:o + stored]; o += stored
    if codec == 1:
        blob = subprocess.run(['zstd', '-d', '-q', '-c'], input=blob, capture_output=True, check=True).stdout
    assert len(blob) == raw, (sid, len(blob), raw)
    sections[sid] = blob

k = sections[3]; o = 0
ver, = struct.unpack_from('<I', k, o); o += 4 + 20
n, = struct.unpack_from('<I', k, o); o += 4
print(f'kernel section v{ver}, {n} modules')
if ver >= 3:  # last list of the section: kernel GetProcAddress trampolines (module, ordinal, address)
    import re
    for m in re.finditer(rb'(xam|xboxkrnl|xbdm)', k):
        if m.start() >= 4 and struct.unpack_from('<I', k, m.start() - 4)[0] == len(m.group(1)) and m.end() + 8 <= len(k):
            o2, a2 = struct.unpack_from('<2I', k, m.end())
            if (a2 & 7) == 0 and 0x80000000 <= a2 < 0x90000000: print(f'  trampoline {m.group(1).decode()} ordinal {o2:X} at {a2:08X}')
for _ in range(n):
    h, t, go = struct.unpack_from('<3I', k, o); o += 12
    ln, = struct.unpack_from('<I', k, o); o += 4
    name = k[o:o + ln].decode(errors='replace'); o += ln
    extra = ''
    if ver >= 2:
        hm, xh = struct.unpack_from('<2I', k, o); o += 8
        extra = f' ldr {hm:08X} header {xh:08X}'
    print(f'  handle {h:08X}{extra}  {name}')

m = sections[2]; o = 8
for heap in range(5):
    base, ps, count = struct.unpack_from('<III', m, o); o += 12
    regions = []
    for i in range(count):
        w, = struct.unpack_from('<Q', m, o); o += 8
        st, ba, rc = (w >> 48) & 3, w & 0xFFFFF, (w >> 20) & 0xFFFFF
        if (st & 2) or heap == 4: o += ps  # committed pages (physical heap: all) carry contents
        key = (st, ba, rc)
        if regions and regions[-1][0] == key: regions[-1][2] = i
        else: regions.append([key, i, i])
    if base == want and heap != 4:
        print(f'heap {base:08X} page {ps:X}:')
        for (st, ba, rc), i0, i1 in regions:
            print(f'  {base + i0 * ps:08X}-{base + (i1 + 1) * ps:08X} state {st}' +
                  (f' region {base + ba * ps:08X}+{rc * ps:X}' if st else ''))
