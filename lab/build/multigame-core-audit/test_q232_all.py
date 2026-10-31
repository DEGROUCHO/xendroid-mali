import subprocess,pathlib,json,sys
W=pathlib.Path(__file__).parent
A=['/opt/homebrew/bin/adb','-s','YOUR_DEVICE_SERIAL']
r=subprocess.run(A+['shell','dumpsys window'],capture_output=True,text=True,check=True)
focus=next((x for x in r.stdout.splitlines() if 'mCurrentFocus=' in x),'')
import time
for attempt in range(8):
 if 'com.miui.home.launcher.Launcher' in focus:break
 print('WAIT PHONE HOME',flush=True);time.sleep(45)
 r=subprocess.run(A+['shell','dumpsys window'],capture_output=True,text=True,check=True)
 focus=next((x for x in r.stdout.splitlines() if 'mCurrentFocus=' in x),'')
else:raise RuntimeError('Phone remained occupied; no APK/config changes')
assert not subprocess.run(A+['shell','pidof xendroid.compose:emu || true'],capture_output=True,text=True,check=True).stdout.strip(),'Emulator occupied'
backup=json.loads((W.parent/'best-installed-build.json').read_text())['apk']
subprocess.run(A+['install','-r',str(W/'q232-test.apk')],check=True)
try:
 settings='apu_aaudio_log_stats=true,vulkan_guest_output_capture_request="/sdcard/Android/data/xendroid.compose/files/compose/q232-guest.request"'
 subprocess.run([sys.executable,str(W/'run_direct_probe.py'),'q232-all',settings,''],check=True)
finally:
 subprocess.run(A+['install','-r',backup],check=True)
 print('RESTORED BEST APK',flush=True)
