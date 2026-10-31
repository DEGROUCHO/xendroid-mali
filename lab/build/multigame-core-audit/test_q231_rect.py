import subprocess,pathlib,json,sys
W=pathlib.Path(__file__).parent
A=['/opt/homebrew/bin/adb','-s','YOUR_DEVICE_SERIAL']
r=subprocess.run(A+['shell','dumpsys window'],capture_output=True,text=True,check=True)
focus=next((x for x in r.stdout.splitlines() if 'mCurrentFocus=' in x),'')
assert 'com.miui.home.launcher.Launcher' in focus,'Phone occupied'
backup=json.loads((W.parent/'best-installed-build.json').read_text())['apk']
subprocess.run(A+['install','-r',str(W/'q231-test.apk')],check=True)
try:
 settings='live_cvar_file="/sdcard/Android/data/xendroid.compose/files/compose/q231-live.toml",vulkan_rt_trace_request="/sdcard/Android/data/xendroid.compose/files/compose/q231-rt.request",vulkan_rectangle_lists_vs_expansion=true,vulkan_log_capability_audit=true,apu_aaudio_log_stats=true,vulkan_guest_output_capture_request="/sdcard/Android/data/xendroid.compose/files/compose/q231-rect-guest.request"'
 subprocess.run([sys.executable,str(W/'run_direct_probe.py'),'q231-rect',settings,'Call of Duty - Black Ops'],check=True)
finally:
 subprocess.run(A+['install','-r',backup],check=True)
 print('RESTORED BEST APK',flush=True)
