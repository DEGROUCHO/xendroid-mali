#!/usr/bin/env python3
"""xdtest.py TAG BUILD GAME [Table.key=value ...] [--prof] [--secs N] [--live k=v,k=v] [--ledger] [--capture] [--rttrace] [--dump-shaders]

One standard phone test (rules in "xendroid-ultimate-mali/docs/TEST-RULES.md"). GAME: BO, DANTE, REACH (restore from
states/<TITLE>/slot-1.xds). Wraps run-restore-only.py and adds the checks that used to be done by memory:
  - BUILD must be the latest qNNN-ui26.apk (XD_ALLOW_OLD=1 to compare an older one on purpose)
  - string values are quoted for TOML; tags of control runs never carry profilers (--prof => tag ends -prof)
  --prof          threadcpu + simpleperf (cpprof) + Mali counters (GPUCTR-TAG.txt, tools/xd_gpuctr/analyze.py) from gameplay
  - no other runner active; gate: launcher, emulator closed, battery >= 25 %, temperature <= 38.0 C
  - xe.log line count saved before the run: metrics come only from this run's lines (TAG/xe-run.log)
  - one standard row in TESTS.tsv (and NIGHT.tsv)
In-gameplay actions (fired once THIS run's restore is OK and its GuestFps reach the game's gameplay draws, never on the
previous run's xe.log lines; TEST-RULES 18-19):
  --live k=v,...  append scalar cvars to compose/ui-live.toml (the only live file the UI lets the core read); after the
                  run the row says "live OK" only if the core logged them as applied
  --ledger        = --live log_pass_bytes=3,log_render_pass_breaks=60 + tools-pass/passreport.py -> TAG/passreport.txt
  --rttrace       one-frame RTTRACE (claims, transfers, resolves, clears) in gameplay -> xe-run.log
  --capture       one guest-output capture (+ texaudit/texdump if their cvars are on): pulled into TAG/, texaudit
                  analysed into TAG/texaudit.txt, phone leftovers removed
  --dump-shaders  GPU.dump_shaders: ucode disassembly + host shaders of every shader of the run -> TAG/shdump
  --move          walk forward and turn the camera (virtual joystick, xd-pad.dex) for the rest of the window, so the
                  game streams new scenery as in real play; logs every frame over 45 ms with its cause (FrameSpike)
                  and the row gets the slow-frame count and what they had in common. Use with --secs 90.
"""
import os, re, subprocess, sys, threading, time
from pathlib import Path

W = Path(__file__).resolve().parent
ROOT = W.parents[1]
L = Path(os.environ.get('XD_APK_DIR', ROOT / 'lab/build/apk'))
ADB = os.environ.get('XD_ADB', 'adb'); PKG = os.environ.get('XD_PKG', 'xendroid.compose')
C = '/sdcard/Android/data/' + PKG + '/files/compose'
XE = C + '/xe.log'
LIVE = C + '/ui-live.toml'; REQ = C + '/guest-output.request'; RTREQ = C + '/rt-trace.request'
GAMES = {'BO': ('Black Ops', '41560855'), 'DANTE': ('Dante', '454108CF'), 'REACH': ('Reach', '4D53085B'),
         'GTA': ('Grand Theft Auto IV', '545407F2')}
# Draws/frame that mean "restored scene is rendering" (Reach loading 2-60, gameplay 2500; BO gameplay ~5000;
# GTA IV street 5000-6000).
GAMEPLAY_DRAWS = {'BO': 4000, 'DANTE': 200, 'REACH': 2000, 'GTA': 3000}
LEDGER = 'log_pass_bytes=3,log_render_pass_breaks=60'
ANALYZE = str(ROOT / 'tools/xd_texaudit_analyze.py')
DIAG = ['GPU.log_guest_fps=true', 'GPU.log_cp_frame_breakdown=true']
# XD_LEAN=1: no per-draw phase breakdown (its ~14 clock reads per draw cost up to half of the CP thread, simpleperf
# q555 25/10): fps/frame times as the daily config sees them; the cp/prim/vf columns stay empty.
if os.environ.get('XD_LEAN') == '1':
    DIAG = ['GPU.log_guest_fps=true']


def sh(cmd):
    return subprocess.run([ADB, 'shell', cmd], capture_output=True, text=True).stdout.strip()


def die(msg):
    sys.exit('xdtest: ' + msg)


def quote(kv):
    k, v = kv.split('=', 1)
    if v in ('true', 'false') or re.fullmatch(r'-?[0-9.]+', v) or v[:1] in ('"', "'"):
        return kv
    return f"{k}='{v}'"


SRC = ROOT / 'src/xendroid-src/emulator-core/src/main/cpp/xenia/src'


def cvar_tables():
    """name -> table of every DEFINE_* cvar in the source (a cvar in the wrong [table] is silently ignored)."""
    tables = {}
    for f in SRC.rglob('*.cc'):
        t = f.read_text(errors='replace')
        for m in re.finditer(r'DEFINE_(?:bool|int32|uint32|int64|uint64|double|string|path|unique_path)\(\s*([a-z0-9_]+)\s*,'
                             r'(?:(?!DEFINE_).)*?"([A-Za-z0-9]+)"\s*\)\s*;', t, re.S):
            tables[m.group(1)] = m.group(2)
    return tables


def latest_build():
    nums = [int(m.group(1)) for p in L.glob('q*-ui26.apk') if (m := re.fullmatch(r'q(\d+)-ui26\.apk', p.name))]
    return f'q{max(nums)}' if nums else None


def temp():
    t = re.search(r'temperature: (\d+)', sh('dumpsys battery'))
    return int(t.group(1)) if t else 999


def gate():
    while True:
        bat = sh('dumpsys battery')
        m = re.search(r'level: (\d+)', bat)
        if not m:  # adb briefly gone (cable, USB reset): wait for it instead of failing the queue
            time.sleep(30)
            continue
        level = int(m.group(1))
        idle = 'launcher' in sh('dumpsys window | grep -m1 mCurrentFocus').lower() and not sh(f'pidof {PKG}:emu')
        if idle and level >= int(os.environ.get('XD_MIN_BATTERY', '25')) and temp() <= 380:
            return 'AC' if 'AC powered: true' in bat else 'USB' if 'USB powered: true' in bat else 'batt'
        time.sleep(30)


# Run environment every 10 s -> TAG/env.tsv: CPU clocks (FPSGO/thermal lower them when GPU-bound), SoC temperature,
# hidden Mali cooling cap, free memory and the emulator's swap. REACH73 (25/10) looked like a 2x2 code regression
# but every CP phase was 1.8x slower and the emulator had 219 MB in swap with lmkd killing apps.
ENV_CMD = ('cat /proc/uptime; for c in 0 4 7; do echo f$c $(cat /sys/devices/system/cpu/cpufreq/policy$c/scaling_cur_freq); done; '
           'grep -E "^(MemAvailable|SwapTotal|SwapFree):" /proc/meminfo; p=$(pidof ' + PKG + ':emu); '
           '[ -n "$p" ] && grep -E "^Vm(RSS|Swap):" /proc/$p/status; dumpsys thermalservice | grep -E "mName=SOC|CoolingDevice.*mali"')
ENV_COLS = ['t', 'f0', 'f4', 'f7', 'soc', 'mali', 'avail', 'swap', 'rss', 'emuswap']


def env_sample():
    out = sh(ENV_CMD)
    kb = lambda k: int(m.group(1)) if (m := re.search(rf'^{k}:\s+(\d+)', out, re.M)) else None
    soc = re.findall(r'mValue=([0-9.]+)[^}]*mName=SOC', out)
    mali = re.findall(r'CoolingDevice\{mValue=(\d+)[^}]*mali', out)
    st, sf, rss, es = kb('SwapTotal'), kb('SwapFree'), kb('VmRSS'), kb('VmSwap')
    return {'t': float(out.split()[0]) if out else 0,
            **{f'f{c}': int(m.group(1)) // 1000 if (m := re.search(rf'^f{c} (\d+)', out, re.M)) else None for c in (0, 4, 7)},
            'soc': float(soc[-1]) if soc else None, 'mali': int(mali[-1]) if mali else None,
            'avail': kb('MemAvailable') // 1024 if kb('MemAvailable') else None,
            'swap': (st - sf) // 1024 if st is not None and sf is not None else None,
            'rss': rss // 1024 if rss is not None else None, 'emuswap': es // 1024 if es is not None else None}


def env_sampler(path, done):
    with open(path, 'w') as f:
        f.write('\t'.join(ENV_COLS) + '\n')
        while not done.wait(10):
            s = env_sample()
            f.write('\t'.join('' if s[k] is None else str(s[k]) for k in ENV_COLS) + '\n')
            f.flush()


def env_summary(path):
    rows = [dict(zip(ENV_COLS, l.rstrip('\n').split('\t'))) for l in open(path).readlines()[1:]]
    rows = [r for r in rows if r['rss']]  # emulator alive
    if not rows:
        return 'env none'
    col = lambda k: [float(r[k]) for r in rows if r[k]]
    rng = lambda k: f'{min(col(k)):.0f}-{max(col(k)):.0f}' if col(k) else '?'
    # Mali cooling reads 19 also with the SoC at 47 C (25/10), so only a value above it is a cap. Normal runs carry
    # 40-180 MB of emulator swap (system swap 3-4 GB); REACH73's 219 MB came with lmkd kills.
    flags = (['GPU-CAP'] if col('mali') and max(col('mali')) > 19 else []) + \
            (['EMU-SWAP'] if col('emuswap') and max(col('emuswap')) > 200 else [])
    return (f"env f7 {rng('f7')} f4 {rng('f4')} MHz soc {rng('soc')}C mali {rng('mali')} avail {rng('avail')}MB "
            f"emuswap {rng('emuswap')}MB" + (' ' + ' '.join(flags) if flags else ''))


def in_gameplay(base, min_draws, done, actions, events):
    """Runs `actions` once this run's restore is OK and a later GuestFps line has >= min_draws draws/frame."""
    while not done.is_set():
        time.sleep(3)
        n = int(sh(f'wc -l < {XE}') or 0)
        if n < base:
            base = 0  # the log was recreated by the new process
        out = sh(f"tail -n +{base + 1} {XE} | grep -a -E 'RuntimeState: restore OK|GuestFps:'")
        if 'restore OK' not in out:
            continue
        after = out[out.rindex('restore OK'):]
        if any(int(d) >= min_draws for d in re.findall(r'GuestFps: [0-9.]+ fps, (\d+) draws', after)):
            # XD_ACTION_DELAY: later actions (a capture after the new pipelines compiled).
            time.sleep(float(os.environ.get('XD_ACTION_DELAY', '5')))
            for a in actions:
                a()
            events.append(time.strftime('%T'))
            return


def walker(done):
    """Forward on the left stick, camera turning on the right one; the turn flips every 3 pushes."""
    i = 0
    while not done.is_set():
        rx = 0.55 if (i // 3) % 2 == 0 else -0.55
        subprocess.run([ADB, 'shell', 'CLASSPATH=/data/local/tmp/xd-pad.dex app_process /system/bin XdPad '
                        f'0 -1 {rx} 0 4500'], capture_output=True, timeout=30)
        i += 1


def spike_summary(run):
    """FrameSpike lines (log_frame_spikes_ms): count, worst, and how many had pipelines/textures/fence waits."""
    rows = re.findall(r'FrameSpike: ([0-9.]+) ms \| textures created (\d+) \(([0-9.]+) MB\), pipelines (\d+), '
                      r'uploads ([0-9.]+) MB \| CP waits ms: fence=([0-9.]+)', run)
    if not rows:
        return 'spikes 0'
    ms = sorted(float(r[0]) for r in rows)
    pipe = sum(1 for r in rows if int(r[3]))
    tex = sum(1 for r in rows if int(r[1]))
    fence = sum(1 for r in rows if float(r[5]) >= 10)
    return (f'spikes {len(rows)} (>45ms) med {ms[len(ms) // 2]:.0f} worst {ms[-1]:.0f}ms | with pipelines {pipe} '
            f'textures {tex} fence>=10ms {fence}')


def last(pattern, text, n=1):
    found = re.findall(pattern, text)
    return found[-n:] if found else []


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    prof = '--prof' in sys.argv
    secs = int(sys.argv[sys.argv.index('--secs') + 1]) if '--secs' in sys.argv else 50
    if '--secs' in sys.argv:
        args.remove(str(secs))
    live = sys.argv[sys.argv.index('--live') + 1] if '--live' in sys.argv else ''
    if live:
        args.remove(live)
    ledger, capture = '--ledger' in sys.argv, '--capture' in sys.argv
    dump = '--dump-shaders' in sys.argv
    move = '--move' in sys.argv
    rttrace = '--rttrace' in sys.argv
    if ledger:
        live = ','.join(x for x in (live, LEDGER) if x)
    live_kv = [kv for kv in live.split(',') if kv]
    for kv in live_kv:
        if not re.fullmatch(r'[a-z0-9_]+=(true|false|-?[0-9]+)', kv):
            die(f'bad --live entry {kv!r}, expected name=bool|int (no Table. prefix)')
    if len(args) < 3:
        die(__doc__)
    tag, build, game, cvars = args[0], args[1], args[2].upper(), args[3:]
    if game not in GAMES:
        die(f'unknown game {game}, use one of {list(GAMES)}')
    apk = L / f'{build}-ui26.apk'
    while subprocess.run(['pgrep', '-f', f'build-{build}.py'], capture_output=True).stdout:
        time.sleep(20)  # its build is still running: wait for the APK instead of failing
    if not apk.exists():
        die(f'missing {apk}')
    if build != latest_build() and not os.environ.get('XD_ALLOW_OLD'):
        die(f'{build} is not the latest build ({latest_build()}); set XD_ALLOW_OLD=1 only for a deliberate comparison')
    if prof != tag.endswith('-prof'):
        die('profiled runs and only them must have a tag ending in -prof (controls never carry profilers)')
    for kv in cvars:
        if not re.fullmatch(r'[A-Za-z]+\.[a-z0-9_]+=.+', kv):
            die(f'bad cvar {kv!r}, expected Table.key=value')
        if kv.split('=')[0].endswith(('.live_cvar_file', '.vulkan_guest_output_capture_request', '.dump_shaders', '.vulkan_rt_trace_request')):
            die(f'{kv!r}: use --live / --ledger / --capture / --rttrace / --dump-shaders')
    tables = cvar_tables()
    for kv in cvars:
        table, name = kv.split('=')[0].split('.', 1)
        if name in tables and tables[name] != table:
            die(f'{kv!r}: {name} lives in table [{tables[name]}], not [{table}]')
    if capture:
        cvars = cvars + [f'Vulkan.vulkan_guest_output_capture_request={REQ}']
    if dump:
        cvars = cvars + [f'GPU.dump_shaders={C}/shdump']
    if rttrace:
        cvars = cvars + [f'Vulkan.vulkan_rt_trace_request={RTREQ}']
    if move:
        cvars = cvars + ['GPU.log_frame_spikes_ms=45']
    if subprocess.run(['pgrep', '-f', 'run-restore-only.py|reach-shot.py|reach-save.py|pkg_run_c.py'],
                      capture_output=True).stdout:
        die('another runner is active')
    title_name, title_id = GAMES[game]
    power = gate()
    t0 = temp()
    base = int(sh(f'wc -l < {XE}') or 0)
    if dump:
        sh(f'rm -rf {C}/shdump')
    actions = []
    if live_kv:
        lines = ''.join(kv.replace('=', ' = ') + '\\n' for kv in live_kv)
        actions.append(lambda: sh(f"printf '{lines}' >> {LIVE}"))
    if capture:
        sh(f'rm -rf {REQ} {C}/texaudit {C}/texdump {C}/guest-output-*.png')
        actions.append(lambda: sh(f'touch {REQ}'))
    if rttrace:
        sh(f'rm -f {RTREQ}')
        actions.append(lambda: sh(f'touch {RTREQ}'))
    done, fired = threading.Event(), []
    if move:
        actions.append(lambda: threading.Thread(target=walker, args=(done,), daemon=True).start())
    watcher = threading.Thread(target=in_gameplay, args=(base, GAMEPLAY_DRAWS[game], done, actions, fired), daemon=True)
    if actions:
        watcher.start()
    extra = ',' + ','.join(DIAG + [quote(kv) for kv in cvars])
    env = dict(os.environ, XD_DIAG_EXTRA=extra)
    profilers = []
    if prof:
        # Started as soon as gameplay is reached (in_gameplay), not at a fixed 120 s: a profiled run lasts as long
        # as a plain one. cpprof waits for the restore by itself.
        def start_profilers():
            profilers.append(subprocess.Popen(['python3', str(W / 'tools-pass/threadcpu.py'), f'THREADS-{tag}.txt', '5', '10'], cwd=W))
            # Mali counters (binning / fragment / compute queue cycles) over the same window.
            profilers.append(subprocess.Popen([ADB, 'shell', 'sleep 5; /data/local/tmp/xd_gpuctr 12 1000'],
                                              stdout=open(W / f'GPUCTR-{tag}.txt', 'w'), stderr=subprocess.STDOUT))
        actions.append(start_profilers)
        profilers.append(subprocess.Popen(['python3', str(W / 'tools-pass/cpprof.py'), f'PROF-{tag}', build, '120', '10'], cwd=W))
        if watcher.ident is None:
            watcher.start()
    (W / tag).mkdir(exist_ok=True)
    env_done = threading.Event()
    sampler = threading.Thread(target=env_sampler, args=(W / tag / 'env.tsv', env_done), daemon=True)
    sampler.start()
    with open(W / f'{tag}.log', 'w') as log:
        subprocess.run(['python3', str(W / 'run-restore-only.py'), tag, str(apk), title_name, title_id, str(secs)],
                       cwd=W, env=env, stdout=log, stderr=log)
    done.set()
    env_done.set()
    sampler.join()
    for p in profilers:
        p.wait()
    run = subprocess.run([ADB, 'shell', f'tail -n +{base + 1} {XE}'], capture_output=True, text=True,
                         errors='replace').stdout
    (W / tag / 'xe-run.log').write_text(run)
    t1 = temp()
    notes = [env_summary(W / tag / 'env.tsv')]
    if actions:
        notes.append(f'ingame {fired[0]}' if fired else 'GAMEPLAY NEVER REACHED (actions not fired)')
    if move:
        notes.append(spike_summary(run))
    if live_kv:
        applied = ' '.join(last(r'LiveCvar: frame=\d+ applied:([^\n|]*)', run, 50))
        ok = all(kv.split('=')[0] + '=' in applied for kv in live_kv)
        notes.append('live OK' if ok else 'LIVE NOT APPLIED')
    if capture:
        for d in ('texaudit', 'texdump'):
            if sh(f'ls -d {C}/{d} 2>/dev/null'):
                subprocess.run([ADB, 'pull', f'{C}/{d}', str(W / tag / d)], capture_output=True)
        for png in sh(f'ls {C}/guest-output-*.png {C}/guest-output-seq-*.rgbx 2>/dev/null').split():
            subprocess.run([ADB, 'pull', png, str(W / tag)], capture_output=True)
        sh(f'rm -rf {REQ} {C}/texaudit {C}/texdump {C}/guest-output-*.png {C}/guest-output-seq-*.rgbx')
        if list((W / tag).glob('guest-output-seq-*.rgbx')):
            with open(W / tag / 'retinoseq.txt', 'w') as f:
                subprocess.run(['python3', str(W / 'tools-pass/retinoseq.py'), str(W / tag), str(W / tag / 'xe-run.log')],
                               stdout=f, stderr=f)
            notes.append((W / tag / 'retinoseq.txt').read_text(errors='replace').strip().splitlines()[-1])
        if (W / tag / 'texaudit' / 'audit.jsonl').exists():
            n = sum(1 for _ in open(W / tag / 'texaudit' / 'audit.jsonl'))
            with open(W / tag / 'texaudit.txt', 'w') as f:
                subprocess.run(['python3', ANALYZE, str(W / tag / 'texaudit')], stdout=f, stderr=f)
            notes.append(f'texaudit {n} textures')
    if live_kv:
        sh(f'rm -f {LIVE}')
    if dump:
        subprocess.run([ADB, 'pull', f'{C}/shdump', str(W / tag / 'shdump')], capture_output=True)
        sh(f'rm -rf {C}/shdump')
        notes.append(f"shdump {len(list((W / tag / 'shdump').glob('*'))) if (W / tag / 'shdump').exists() else 0} files")
    if ledger:
        with open(W / tag / 'passreport.txt', 'w') as f:
            subprocess.run(['python3', str(W / 'tools-pass/passreport.py'), str(W / tag / 'xe-run.log'), build],
                           stdout=f, stderr=f)
        notes.append('passreport ' + ' '.join((W / tag / 'passreport.txt').read_text(errors='replace').split())[:160])
    runner = (W / f'{tag}.log').read_text(errors='replace')
    restore = (last(r'RuntimeState: restore (OK|FAILED[^\n]{0,120})', run) or ['none'])[0]
    if 'process absent' in (W / tag / 'runner.log').read_text(errors='replace') if (W / tag / 'runner.log').exists() else False:
        restore += ' | EMULATOR DIED (config parse? crash?)'
    if notes:
        restore += ' | ' + ' | '.join(notes)
    fps = ' '.join(f'{f}' for f, d, g in last(r'GuestFps: ([0-9.]+) fps, (\d+) draws/frame, gpu ([0-9.]+) ms', run, 3))
    gpu = (last(r'GuestFps: [0-9.]+ fps, (\d+) draws/frame, gpu ([0-9.]+) ms', run) or [('', '')])[0]
    cp = (last(r'CpFrame: cpu=([0-9.]+)ms.*?prim=([0-9.]+).*?vf=([0-9.]+)', run) or [('', '', '')])[0]
    mp = (last(r'ShmProtect: per frame: mprotect calls=([0-9.]+) pages=(\d+) time=([0-9.]+) ms', run) or [('', '', '')])[0]
    ps = (last(r'preswap hits=(\d+) misses=(\d+)(?: stale=(\d+))?', run) or [('', '', '')])[0]
    rc = (last(r'CpMemCache: hits=(\d+)', run) or [''])[0]
    # Stability: per-window worst frame interval (GpuFrame, ~1 s windows) of the last 20 windows.
    worst = sorted(float(x) for x in last(r'GpuFrame: \d+ frames, interval avg=[0-9.]+ms max=([0-9.]+)ms', run, 20))
    spikes = (f'{worst[len(worst) // 2]:.1f}', f'{worst[-1]:.1f}') if worst else ('', '')
    shown = ' '.join(cvars + ([f'--live {live}'] if live else []) + (['--capture'] if capture else []) + (['--rttrace'] if rttrace else []) + (['--dump-shaders'] if dump else []) + (['--move'] if move else [])) or 'default'
    row = [time.strftime('%F %T'), tag, build, game, shown, power, f'{t0/10:.1f}', f'{t1/10:.1f}',
           restore, fps, spikes[0], spikes[1], gpu[0], gpu[1], cp[0], cp[1], cp[2], mp[0], mp[1], mp[2], ps[0], ps[1], ps[2], rc]
    new = not (W / 'TESTS.tsv').exists()
    with open(W / 'TESTS.tsv', 'a') as f:
        if new:
            f.write('\t'.join(['when', 'tag', 'build', 'game', 'cvars', 'power', 'tstart', 'tend', 'restore', 'fps',
                               'maxframe_med_ms', 'maxframe_worst_ms', 'draws', 'gpu_ms', 'cp_ms', 'prim_ms', 'vf_ms', 'mprot_calls', 'mprot_pages',
                               'mprot_ms', 'preswap_hits', 'preswap_miss', 'preswap_stale', 'reqcache_hits']) + '\n')
        f.write('\t'.join(row) + '\n')
    line = (f"{time.strftime('%T')} {tag} {build} {game} [{shown}] {power} "
            f"{t0/10:.1f}->{t1/10:.1f}C | restore {restore[:400]} | fps {fps} | maxframe med {spikes[0]} worst {spikes[1]}ms | draws {gpu[0]} gpu {gpu[1]}ms | "
            f"cp {cp[0]} prim {cp[1]} vf {cp[2]} | mprot {mp[0]}/{mp[1]}p {mp[2]}ms | preswap {ps[0]}/{ps[1]} stale {ps[2]} | reqcache {rc}")
    with open(W / 'NIGHT.tsv', 'a') as f:
        f.write(line + '\n')
    print(line)


if __name__ == '__main__':
    main()
