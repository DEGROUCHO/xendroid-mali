#!/usr/bin/env python3
"""heapprofd trace (perfetto ProfilePacket) -> live native heap by callsite.

usage: heapan.py heap.pftrace [libe.so with symbols] [top=40]

No trace_processor needed: minimal protobuf wire decoder. libe.so frames are
symbolized with the NDK llvm-symbolizer; other libraries use the names
heapprofd resolved on the device.
Prints, per dump: live bytes; for the last dump: live bytes by library, by
first emulator (libe.so) function on the stack, and the top full callsites.
"""
import collections, os, subprocess, sys

NDK_SYM = os.path.expanduser(
    '~/Library/Android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-symbolizer')


def varint(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7F) << s; s += 7
        if c < 0x80:
            return r, i


def fields(b):
    i, n = 0, len(b)
    while i < n:
        k, i = varint(b, i); f, t = k >> 3, k & 7
        if t == 0:
            v, i = varint(b, i)
        elif t == 1:
            v = int.from_bytes(b[i:i + 8], 'little'); i += 8
        elif t == 2:
            l, i = varint(b, i); v = b[i:i + l]; i += l
        elif t == 5:
            v = int.from_bytes(b[i:i + 4], 'little'); i += 4
        else:
            raise ValueError('wire type %d' % t)
        yield f, t, v


def packed_or_single(t, v):
    if t == 2:
        out, i = [], 0
        while i < len(v):
            x, i = varint(v, i); out.append(x)
        return out
    return [v]


def main():
    path = sys.argv[1]
    so = sys.argv[2] if len(sys.argv) > 2 else None
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    data = open(path, 'rb').read()
    strings, frames, mappings, stacks = {}, {}, {}, {}
    fnames, buildids, mpaths, pkt_ts = {}, {}, {}, []
    dumps = collections.OrderedDict()  # (ts) -> {callstack: (alloc, freed, allocs, frees)}
    for f, t, pkt in fields(data):
        if f != 1:
            continue
        for pf, pt, pv in fields(pkt):
            if pf == 8:
                pkt_ts.append(pv)
            if pf == 12:
                # InternedData (Android 12+): function_names 5, frames 6,
                # callstacks 7, build_ids 16, mapping_paths 17, mappings 19.
                for qf, qt, qv in fields(pv):
                    if qf in (5, 16, 17):
                        d = {a: v for a, _, v in fields(qv)}
                        tbl = fnames if qf == 5 else (buildids if qf == 16 else mpaths)
                        tbl[d.get(1, 0)] = d.get(2, b'').decode('utf-8', 'replace')
                    elif qf == 6:
                        d = {a: v for a, _, v in fields(qv)}; frames[d.get(1, 0)] = (d.get(2, 0), d.get(3, 0), d.get(4, 0))
                    elif qf == 19:
                        d = {}; paths = []
                        for a, at, v in fields(qv):
                            if a == 7:
                                paths += packed_or_single(at, v)
                            else:
                                d[a] = v
                        mappings[d.get(1, 0)] = (paths, d.get(4, 0), d.get(5, 0), d.get(6, 0), d.get(3, 0))
                    elif qf == 7:
                        iid, fr = 0, []
                        for a, at, v in fields(qv):
                            if a == 1:
                                iid = v
                            elif a == 2:
                                fr += packed_or_single(at, v)
                        stacks[iid] = fr
                continue
            if pf != 37:
                continue
            for qf, qt, qv in fields(pv):
                if qf == 1:
                    d = {a: v for a, _, v in fields(qv)}; strings[d.get(1, 0)] = d.get(2, b'').decode('utf-8', 'replace')
                elif qf == 2:
                    d = {a: v for a, _, v in fields(qv)}; frames[d.get(1, 0)] = (d.get(2, 0), d.get(3, 0), d.get(4, 0))
                elif qf == 4:
                    d = {}; paths = []
                    for a, at, v in fields(qv):
                        if a == 7:
                            paths += packed_or_single(at, v)
                        else:
                            d[a] = v
                    mappings[d.get(1, 0)] = (paths, d.get(4, 0), d.get(5, 0), d.get(6, 0), d.get(3, 0))
                elif qf == 3:
                    iid, fr = 0, []
                    for a, at, v in fields(qv):
                        if a == 1:
                            iid = v
                        elif a == 2:
                            fr += packed_or_single(at, v)
                    stacks[iid] = fr
                elif qf == 5:
                    ts = 0; samples = []
                    meta = {}
                    for a, at, v in fields(qv):
                        if a == 9:
                            ts = v
                        elif a == 2:
                            samples.append({x: y for x, _, y in fields(v)})
                        elif a in (3, 4, 6, 7, 8, 10, 12):
                            meta[a] = v
                        elif a == 11:
                            meta['heap'] = v.decode()
                    dd = dumps.setdefault(ts, {})
                    for smp in samples:
                        dd[smp.get(1, 0)] = (smp.get(2, 0), smp.get(3, 0), smp.get(5, 0), smp.get(6, 0))
                    if meta:
                        print('dump meta', ts, meta)
    if pkt_ts:
        print('packet ts range %.1f s' % ((max(pkt_ts) - min(pkt_ts)) / 1e9), 'first', min(pkt_ts))
    if not dumps:
        print('no dumps'); return
    def mpath(mid):
        m = mappings.get(mid)
        return '/'.join(mpaths.get(p, strings.get(p, '?')) for p in m[0]) if m else '?'
    # symbolize libe.so frames
    libe_pcs = sorted({fr[2] for fr in frames.values() if mpath(fr[1]).endswith('libe.so')})
    sym = {}
    if so and libe_pcs and os.path.exists(NDK_SYM):
        inp = '\n'.join('0x%x' % pc for pc in libe_pcs) + '\n'
        out = subprocess.run([NDK_SYM, '--obj=' + so, '--functions=short', '--no-inlines', '--demangle'],
                             input=inp, capture_output=True, text=True).stdout.split('\n\n')
        for pc, block in zip(libe_pcs, out):
            ln = block.strip().split('\n')
            sym[pc] = ln[0] if ln and ln[0] else '?'

    def fname(fid):
        fr = frames.get(fid)
        if not fr:
            return ('?', '?')
        lib = os.path.basename(mpath(fr[1]))
        if lib == 'libe.so' and fr[2] in sym:
            return (lib, sym[fr[2]])
        n = fnames.get(fr[0], strings.get(fr[0], '')) if fr[0] else ''
        return (lib, n or '0x%x' % fr[2])

    MB = 1 << 20
    for ts, dd in dumps.items():
        live = sum(a - f for a, f, _, _ in dd.values())
        alloc = sum(a for a, _, _, _ in dd.values())
        print('dump ts=%d live=%.1f MB allocated_total=%.1f MB callstacks=%d' % (ts, live / MB, alloc / MB, len(dd)))
    last = list(dumps.values())[-1]
    alloc_libs = ('libc.so', 'libc++.so', 'libc++_shared.so', 'heapprofd_client_api.so', 'libheapprofd_client_api.so')
    by_lib = collections.Counter(); by_fn = collections.Counter(); by_stack = collections.Counter(); by_mali = collections.Counter()
    for cs, (a, f, ac, fc) in last.items():
        live = a - f
        if live <= 0:
            continue
        fr = [fname(x) for x in stacks.get(cs, [])]
        # frames are root first, leaf last
        leaf_first = list(reversed(fr))
        nonalloc = [x for x in leaf_first if x[0] not in alloc_libs]
        lib = nonalloc[0][0] if nonalloc else (leaf_first[0][0] if leaf_first else '?')
        by_lib[lib] += live
        emu = next((x[1] for x in leaf_first if x[0] == 'libe.so'), '(no libe frame)')
        by_fn[emu] += live
        if lib.startswith('libGLES_mali') or 'mali' in lib.lower():
            by_mali[(nonalloc[0][1] if nonalloc else '?', emu)] += live
        key = ' <- '.join('%s:%s' % (l[:12], n[:60]) for l, n in nonalloc[:6])
        by_stack[key] += live
    tot = sum(by_lib.values())
    print('\n== last dump live %.1f MB by library (first non-allocator frame)' % (tot / MB))
    for k, v in by_lib.most_common(15):
        print('%8.1f MB  %s' % (v / MB, k))
    print('\n== by first libe.so function on the stack')
    for k, v in by_fn.most_common(top):
        print('%8.1f MB  %s' % (v / MB, k))
    if by_mali:
        print('\n== Mali driver allocations: (driver frame, emulator caller)')
        for k, v in by_mali.most_common(20):
            print('%8.1f MB  %s  <=  %s' % (v / MB, k[0][:60], k[1][:80]))
    print('\n== top callsites')
    for k, v in by_stack.most_common(top):
        print('%8.1f MB  %s' % (v / MB, k))


if __name__ == '__main__':
    main()
