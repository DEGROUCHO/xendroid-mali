#!/usr/bin/env python3
"""One "package" run over several games with one APK.

usage: pkg_run.py TAG APK "game-substr1,game-substr2,..." [k=v,k=v diagnostics] [seconds]

Per game: waits Home + no :emu + battery <= START_MAX, launches the ISO, samples
every 10 s (temperature, focus, process), screenshots at 20/60/120/180 s,
meminfo at 60/120/180 s, process CPU time, and the GuestFps lines of xe.log.
Writes TAG/<n>-<game>/result.json and TAG/summary.json.
finally: restores the pre-run APK (best-installed-build.json) and the base
global config.
"""
import json, pathlib, re, shlex, statistics, subprocess, sys, time, hashlib

A = '/opt/homebrew/bin/adb'; S = 'YOUR_DEVICE_SERIAL'; P = 'xendroid.compose'
BASE = '/sdcard/Android/data/' + P + '/files/compose'
GLOBAL = BASE + '/xenia-canary.config.toml'
W = pathlib.Path(__file__).parent
START_MAX = int(__import__("os").environ.get("XD_START_MAX", "410"))  # battery temp (0.1 C)
STOP_AT = 465        # abort a game at this temperature (existing multigame value)

tag, apk = sys.argv[1], pathlib.Path(sys.argv[2])
# "game" or "game:seconds" - a per-game duration for titles with long intros
# (Halo Reach needs well over 180 s to reach gameplay).
games = [g for g in sys.argv[3].split(',') if g]
# "game:seconds@touch" - also a per-game second for the one-shot touch (RT trace) instead
# of XD_TOUCH_AT, so the trace lands in gameplay.
game_touch = {g.split(':')[0]: {int(g.split('@')[1])} for g in games if '@' in g}
games = [g.split('@')[0] for g in games]
game_seconds = {g.split(':')[0]: int(g.split(':')[1]) for g in games if ':' in g}
games = [g.split(':')[0] for g in games]
diag = sys.argv[4] if len(sys.argv) > 4 else ''
seconds = int(sys.argv[5]) if len(sys.argv) > 5 else 180
D = W / tag; D.mkdir(exist_ok=True)
import os
# Optional one-shot request files (e.g. RT trace) touched at given seconds of each game.
TOUCH_AT = {int(x) for x in os.environ.get('XD_TOUCH_AT', '').split(',') if x}
TOUCH_PATH = os.environ.get('XD_TOUCH_PATH', '')
# Optional simpleperf CPU profile (debuggable app, no root) starting at second
# XD_SIMPLEPERF_AT of each game for XD_SIMPLEPERF_SECS seconds; perf.data and the
# JIT perf map (a64_perf_map) are pulled into the game directory.
SIMPLEPERF_AT = int(os.environ.get('XD_SIMPLEPERF_AT', '0') or 0)
SIMPLEPERF_SECS = int(os.environ.get('XD_SIMPLEPERF_SECS', '15') or 15)
# Optional live A/B inside one game (same scene, same heat): XD_LIVE_AB="a.toml b.toml"
# (flat key = value cvar files) pushed in turn to the live cvar file every
# XD_LIVE_STEP seconds from second XD_LIVE_FROM to the end. The diag must set
# live_cvar_file to LIVE_FILE. Switch times go to live_phases.txt.
LIVE_AB = [pathlib.Path(x) for x in os.environ.get('XD_LIVE_AB', '').split() if x]
LIVE_FROM = int(os.environ.get('XD_LIVE_FROM', '60') or 60)
LIVE_STEP = int(os.environ.get('XD_LIVE_STEP', '10') or 10)
LIVE_FILE = BASE + '/live.toml'
# Optional native heap profile (heapprofd, debuggable app): XD_HEAPPROFD=<seconds>
# starts perfetto right before the game launch, so every allocation of the :emu
# process is seen; the trace lands in heap.pftrace (analyse with heapan.py).
HEAPPROFD_SECS = int(os.environ.get('XD_HEAPPROFD', '0') or 0)
# Extra screenshot seconds (multiples of 10), e.g. XD_EXTRA_SHOTS=140,150,160.
EXTRA_SHOTS = {int(x) for x in os.environ.get('XD_EXTRA_SHOTS', '').split(',') if x}
HEAPPROFD_CFG = """buffers { size_kb: 131072 fill_policy: DISCARD }
data_sources { config { name: "android.heapprofd" heapprofd_config {
  sampling_interval_bytes: 262144
  process_cmdline: "xendroid.compose:emu"
  shmem_size_bytes: 67108864
  block_client: true
  block_client_timeout_us: 5000000
  continuous_dump_config { dump_phase_ms: 30000 dump_interval_ms: 30000 }
} } }
duration_ms: %d
"""
import threading


def live_ab_thread(t0, dest, stop_event):
    k = 0; log = open(dest / 'live_phases.txt', 'w')
    next_t = t0 + LIVE_FROM
    while not stop_event.is_set():
        if stop_event.wait(max(0.0, next_t - time.time())):
            break
        f = LIVE_AB[k % len(LIVE_AB)]
        try:
            adb('push', str(f), LIVE_FILE + '.tmp')
            sh('sh', '-c', f"mv '{LIVE_FILE}.tmp' '{LIVE_FILE}' && touch '{LIVE_FILE}'")
            log.write(f"{time.time() - t0:.1f} {f.name}\n"); log.flush()
        except Exception as ex:
            log.write(f"# push failed {ex}\n"); log.flush()
        k += 1; next_t += LIVE_STEP
    log.close()
# Per-game controller input to get through menus into gameplay: game_inputs.json maps a
# game-name substring to [[second, "BUTTON_A"], ...] (Android KEYCODE_ names without the
# prefix). Sent with `input keyevent --duration` while the emulator has focus; the app
# maps controller key codes to the guest pad whatever the source device.
GAME_INPUTS = {}
if (W / 'game_inputs.json').exists() and os.environ.get('XD_NO_INPUTS') != '1':
    GAME_INPUTS = json.loads((W / 'game_inputs.json').read_text())


def game_input_schedule(name):
    for k, v in GAME_INPUTS.items():
        if k.lower() in name.lower():
            return [(int(t), key) for t, key in v]
    return []


def adb(*a, raw=False, timeout=60):
    r = subprocess.run([A, '-s', S, *a], capture_output=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError(r.stderr.decode(errors='replace')[:300])
    return r.stdout if raw else r.stdout.decode(errors='replace')


def sh(*a):
    return adb('shell', shlex.join(a))


def state():
    b = sh('dumpsys', 'battery')
    return {'temperature': int(re.search(r'temperature:\s*(\d+)', b)[1]),
            'level': int(re.search(r'level:\s*(\d+)', b)[1])}


def focus():
    return next((x for x in sh('dumpsys', 'window').splitlines() if 'mCurrentFocus=' in x), '')


def emu_pid():
    return sh('sh', '-c', 'pidof ' + P + ':emu || true').strip()


def proc_cpu_s(pid):
    try:
        f = sh('cat', f'/proc/{pid}/stat').rsplit(')', 1)[1].split()
        return (int(f[11]) + int(f[12])) / 100.0
    except Exception:
        return None


def thread_ticks(pid):
    out = {}
    try:
        txt = sh('sh', '-c', f'for t in /proc/{pid}/task/*; do cat $t/stat 2>/dev/null; echo; done')
    except Exception:
        return out
    for line in txt.splitlines():
        if ')' not in line:
            continue
        head, rest = line.rsplit(')', 1)
        tid, comm = head.split(' (', 1)
        f = rest.split()
        out[tid] = (comm, int(f[11]) + int(f[12]))
    return out


def thread_breakdown(t0, t1, dt):
    rows = []
    for tid, (comm, ticks) in t1.items():
        d = ticks - t0.get(tid, (comm, 0))[1]
        if d > 0:
            rows.append((round(d / 100.0 / dt, 3), comm, tid))
    rows.sort(reverse=True)
    return rows[:16]


def wait_ready():
    for _ in range(40):
        st = state(); home = 'com.miui.home.launcher.Launcher' in focus(); busy = bool(emu_pid())
        if home and not busy and st['temperature'] <= START_MAX:
            return st
        print('WAIT', st, 'home', home, 'emu', busy, flush=True); time.sleep(45)
    raise RuntimeError('phone not ready (home/cool) after 30 min')


def log_size():
    path = shlex.quote(BASE + '/xe.log')
    command = f'if test -f {path}; then wc -c < {path}; else printf 0; fi'
    return int(sh('sh', '-c', command).strip() or 0)


apk_sha = hashlib.sha256(apk.read_bytes()).hexdigest()
# q229 manifest is stale (bands on new titles); q232 is the last verified-better build.
# The daily build restored after every run: best-apk.txt (an APK name in this
# directory), q232 if absent.
_best_file = W / 'best-apk.txt'
best = str(W / (_best_file.read_text().strip() if _best_file.exists() else 'q232-test.apk'))
base_global = W / 'q232-all-original-global.toml'   # verified pre-diagnostic global
phone_global = adb('shell', 'cat ' + GLOBAL, raw=True)
(D / 'phone-global-before.toml').write_bytes(phone_global)
paths = sh('find', '/sdcard/Download', '-maxdepth', '3', '-type', 'f', '-iname', '*.iso').splitlines()
sel = [(g, next((p for p in paths if g.lower() in p.lower()), None)) for g in games]
missing = [g for g, p in sel if not p]
assert not missing, ('ISO not found', missing, paths)
summary = {'tag': tag, 'apk': str(apk), 'apk_sha256': apk_sha, 'diag': diag, 'games': []}
wait_ready()
try:
    text = base_global.read_text()
    for kv in [x for x in diag.split(',') if x]:
        k, v = kv.split('=', 1)
        v = v.replace('|', ',')  # commas inside a value (lists) are written as '|'
        # "Table.key=value": a cvar newer than the base global - added to its table.
        table = None
        if '.' in k:
            table, k = k.split('.', 1)
        text, n = re.subn(r'(?m)^' + re.escape(k) + r'\s*=[^#\n]*', k + ' = ' + v + ' ', text)
        if n == 0 and table:
            text, n = re.subn(r'(?m)^\[' + re.escape(table) + r'\]\n', '[' + table + ']\n' + k + ' = ' + v + '\n', text)
        assert n == 1, (k, n)
    cfg = D / 'pushed-global.toml'; cfg.write_text(text); adb('push', str(cfg), GLOBAL)
    adb('install', '-r', str(apk), timeout=300)
    print('INSTALLED', apk.name, apk_sha[:12], flush=True)
    for gi, (g, iso) in enumerate(sel):
        name = pathlib.PurePosixPath(iso).stem
        dest = D / (f'{gi + 1}-' + re.sub(r'[^A-Za-z0-9]+', '-', name)[:60]); dest.mkdir(exist_ok=True)
        gsec = game_seconds.get(g, seconds)
        inputs = game_input_schedule(g)
        st0 = wait_ready(); before = log_size(); started = False; shots = []; samples = []
        cpu0 = cpu1 = t_cpu0 = t_cpu1 = None; n = 0; stop = 'completed'; ctr = None; th0 = th1 = None; perf = None; perf_pid = None
        try:
            print('START', name, st0, flush=True)
            heap = None
            if HEAPPROFD_SECS:
                (dest / 'heapprofd.cfg').write_text(HEAPPROFD_CFG % (HEAPPROFD_SECS * 1000))
                adb('push', str(dest / 'heapprofd.cfg'), '/data/local/tmp/xd_heapprofd.cfg')
                heap = subprocess.Popen(
                    [A, '-s', S, 'shell', 'cat /data/local/tmp/xd_heapprofd.cfg | perfetto --txt -c - '
                     '-o /data/misc/perfetto-traces/xd_heap.pftrace'],
                    stdout=open(dest / 'heapprofd.log', 'w'), stderr=subprocess.STDOUT)
                time.sleep(3)
            sh('am', 'start', '-n', P + '/xendroid.compose.EmulatorHostActivity', '-a',
               'xendroid.intent.action.xendroid', '--es', 'game_uri', iso, '--es', 'game_name', name)
            started = True
            live_stop = threading.Event(); live = None
            if LIVE_AB:
                live = threading.Thread(target=live_ab_thread, args=(time.time(), dest, live_stop), daemon=True)
                live.start()
            for n in range(1, gsec // 10 + 1):
                time.sleep(10)
                st = state(); pid = emu_pid(); samples.append({'t': n * 10, **st})
                if not pid:
                    stop = 'process absent'; break
                if P not in focus():
                    stop = 'focus changed'; break
                if n * 10 == 60:
                    cpu0, t_cpu0 = proc_cpu_s(pid), time.time(); th0 = thread_ticks(pid)
                    ctr = subprocess.Popen([A, '-s', S, 'shell', f'/data/local/tmp/xd_gpuctr {gsec - 60} 1000'],
                                           stdout=open(dest / 'gpuctr.txt', 'w'), stderr=subprocess.STDOUT)
                for t_in, key in inputs:
                    if (n - 1) * 10 < t_in <= n * 10:
                        sh('input', 'keyevent', '--duration', '180', 'KEYCODE_' + key)
                        print('INPUT', t_in, key, flush=True)
                if SIMPLEPERF_AT and n * 10 == SIMPLEPERF_AT and perf is None:
                    perf_pid = pid
                    perf = subprocess.Popen(
                        [A, '-s', S, 'shell', 'rm -f /data/local/tmp/xd_perf.data; simpleperf record --app ' + P +
                         f' --duration {SIMPLEPERF_SECS} -e cpu-clock -f 2000 --call-graph fp --trace-offcpu'
                         ' -o /data/local/tmp/xd_perf.data'],
                        stdout=open(dest / 'simpleperf.log', 'w'), stderr=subprocess.STDOUT)
                    print('SIMPLEPERF started', flush=True)
                if TOUCH_PATH and any((n - 1) * 10 < t <= n * 10 for t in game_touch.get(g, TOUCH_AT)):
                    sh('touch', TOUCH_PATH)
                if n * 10 in (20, 60, 120, 180) or n * 10 in EXTRA_SHOTS or (n * 10 > 180 and n * 10 % 60 == 0):
                    f = dest / f'screen-{n * 10}.png'
                    f.write_bytes(adb('exec-out', 'screencap', '-p', raw=True)); shots.append(f.name)
                if n * 10 in (60, 120, 180) or (n * 10 > 180 and n * 10 % 60 == 0):
                    (dest / f'meminfo-{n * 10}.txt').write_text(sh('dumpsys', 'meminfo', P + ':emu'))
                cpu1, t_cpu1 = proc_cpu_s(pid), time.time()
                if th0 is not None:
                    th1 = thread_ticks(pid)
                if st['temperature'] >= STOP_AT:
                    stop = 'thermal'; break
        finally:
            if ctr and ctr.poll() is None:
                ctr.terminate()
            if started and live is not None:
                live_stop.set(); live.join(timeout=30)
                try:
                    sh('rm', '-f', LIVE_FILE)
                except Exception:
                    pass
            if started and heap is not None:
                try:
                    heap.wait(timeout=HEAPPROFD_SECS + 120)
                    adb('pull', '/data/misc/perfetto-traces/xd_heap.pftrace', str(dest / 'heap.pftrace'), timeout=300)
                except Exception as ex:
                    print('HEAPPROFD pull failed', ex, flush=True)
            if perf is not None:
                try:
                    perf.wait(timeout=SIMPLEPERF_SECS + 60)
                    adb('pull', '/data/local/tmp/xd_perf.data', str(dest / 'perf.data'), timeout=300)
                    adb('shell', 'rm -f /data/local/tmp/xd_perf.data')
                    (dest / f'perf-{perf_pid}.map').write_bytes(
                        adb('shell', f'run-as {P} cat perf-{perf_pid}.map', raw=True, timeout=120))
                except Exception as ex:
                    print('SIMPLEPERF pull failed', ex, flush=True)
            log = ''
            if started:
                after = log_size(); off = before + 1 if after >= before else 1
                log = sh('sh', '-c', f'tail -c +{off} {BASE}/xe.log'); (dest / 'session.log').write_text(log)
                (dest / 'logcat.txt').write_text(adb('logcat', '-d', '-t', '700'))
                sh('am', 'force-stop', P)
        fps = [float(m[1]) for m in re.finditer(r'GuestFps: ([0-9.]+) fps', log)]
        draws = [float(m[1]) for m in re.finditer(r'GuestFps: [0-9.]+ fps, ([0-9.]+) draws', log)]
        tail = fps[len(fps) // 3:] if len(fps) > 6 else fps
        pss = None
        for t in sorted([t for t in range(60, gsec + 1, 60)], reverse=True):
            p = dest / f'meminfo-{t}.txt'
            if p.exists():
                m = re.search(r'TOTAL PSS:\s*([0-9,]+)', p.read_text()) or re.search(r'TOTAL\s+([0-9,]+)', p.read_text())
                if m:
                    pss = int(m[1].replace(',', '')) // 1024; break
        gpu = []; ext = []
        if (dest / 'gpuctr.txt').exists():
            for line in (dest / 'gpuctr.txt').read_text(errors='replace').splitlines():
                m = re.search(r'GPUCTR gpu=(\d+).*extRd=(\d+) extWr=(\d+)', line)
                if m and int(m[1]) > 0:
                    gpu.append(int(m[1]) / 1e9); ext.append((int(m[2]) + int(m[3])) / 1e9)
        cores = None
        if cpu0 is not None and cpu1 is not None and t_cpu1 > t_cpu0:
            cores = round((cpu1 - cpu0) / (t_cpu1 - t_cpu0), 2)
        q = sorted(tail)
        res = {'game': name, 'stop': stop, 'seconds': n * 10, 'start': st0,
               'end': samples[-1] if samples else None,
               'fps_median_last2of3': round(statistics.median(tail), 1) if tail else None,
               'fps_p10_last2of3': round(q[len(q) // 10], 1) if q else None,
               'fps_timeline': [round(x, 1) for x in fps],
               'draws_per_frame_median': round(statistics.median(draws)) if draws else None,
               'cpu_cores_60s_to_end': cores, 'pss_mb_last': pss, 'screens': shots,
               'threads_cores': thread_breakdown(th0, th1, t_cpu1 - t_cpu0) if th0 and th1 and t_cpu1 > t_cpu0 else None,
               'gpu_active_Gcyc_s_median': round(statistics.median(gpu), 3) if gpu else None,
               'gpu_ext_GB_s_median': round(statistics.median(ext), 2) if ext else None,
               'errors': [x for x in log.splitlines() if re.search(r'SIGSEGV|Assert|assert|VK_ERROR|fatal', x)][:40]}
        (dest / 'result.json').write_text(json.dumps(res, indent=2))
        summary['games'].append({k: res[k] for k in ('game', 'stop', 'seconds', 'fps_median_last2of3',
                                 'fps_p10_last2of3', 'draws_per_frame_median', 'cpu_cores_60s_to_end', 'gpu_active_Gcyc_s_median', 'gpu_ext_GB_s_median',
                                 'pss_mb_last', 'end')})
        print('DONE', json.dumps(summary['games'][-1]), flush=True)
        (D / 'summary.json').write_text(json.dumps(summary, indent=2))
finally:
    try:
        adb('push', str(base_global), GLOBAL); print('GLOBAL RESTORED', flush=True)
    finally:
        adb('install', '-r', best, timeout=300); print('RESTORED BEST APK', best, flush=True)
