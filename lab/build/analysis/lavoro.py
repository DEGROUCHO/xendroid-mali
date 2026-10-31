#!/usr/bin/env python3
# lavoro.py TAG [TAG...] - the phone's real work per frame, not just the FPS:
# CPU ms per frame, GPU megacycles per frame, and the same per draw.
import json, glob, sys
T = '/path/to/xendroid-ultimate-mali/lab/build/multigame-core-audit'
print('%-34s %5s %6s %6s | %9s %9s | %9s %9s | %5s' % ('run', 'FPS', 'draw', 'core', 'CPUms/frm', 'GPUMc/frm', 'CPUus/drw', 'GPUkc/drw', 'endC'))
for tag in sys.argv[1:]:
    for f in glob.glob(f'{T}/{tag}/*/result.json'):
        j = json.load(open(f)); g = j.get('games', [j])[0] if 'games' in j else j
        fps, dr, co, gc = g.get('fps_median_gameplay_window'), g.get('draws_per_frame_median'), g.get('cpu_cores_gameplay_window'), g.get('gpu_active_Gcyc_s_median')
        if not fps: print(tag, 'no FPS'); continue
        cpu = co * 1000 / fps if co else float('nan'); gpu = gc * 1000 / fps if gc else float('nan')
        end = (g.get('end') or {}).get('temperature', 0) / 10
        print('%-34s %5.1f %6s %6.2f | %9.1f %9.1f | %9.1f %9.1f | %5.1f' % (tag, fps, dr, co or 0, cpu, gpu, cpu * 1000 / dr if dr else 0, gpu * 1000 / dr if dr else 0, end))
