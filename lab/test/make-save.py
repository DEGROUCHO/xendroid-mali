import subprocess,os,time,sys,signal
from pathlib import Path
# usage: make-save.py TAG APK GAME TITLE_ID SAVE_AT_S SLOT INPUTS_JSON
# Boots GAME with the scripted inputs, saves to states/<TITLE>/slot-<SLOT>.xds at SAVE_AT_S
# seconds after launch (screenshot saved.png first), then closes. One-time scene preparation.
WORK=Path(__file__).resolve().parent;LAB=Path("/path/to/xendroid-ultimate-mali/lab")
ADB="/opt/homebrew/bin/adb";PKG="xendroid.compose"
TAG,APK,GAME,TITLE,AT,SLOT,INPUTS=sys.argv[1:8];AT=int(AT)
DEST=WORK/TAG;DEST.mkdir(exist_ok=True)
def adb(*a,timeout=30):return subprocess.check_output([ADB,*a],timeout=timeout)
env=os.environ.copy();env.update(XD_INPUTS_DONE_FILE=str(DEST/"inputs-complete"),XD_LAUNCH_TITLE_ID=TITLE,XD_GAME_INPUTS_FILE=INPUTS,XD_EXTRA_SHOTS="",MEASURE_FROM=str(AT),XD_BASE_GLOBAL=str(LAB/"build/multigame-core-audit/global-user-20261014-mig.toml"))
cmd=["bash",str(LAB/"build/analysis/arm.sh"),TAG,APK,"Kernel.savestate_enable=true,GPU.framerate_limit=0,GPU.framerate_limit_adaptive=false,GPU.guest_display_refresh_cap=true"+os.environ.get("XD_DIAG_EXTRA",""),GAME,"600"]
with (DEST/"runner.log").open("w") as log:
 r=subprocess.Popen(cmd,env=env,stdout=log,stderr=log,start_new_session=True)
 try:
  t0=None;deadline=time.monotonic()+180
  while time.monotonic()<deadline:
   if subprocess.run([ADB,"shell","pidof",PKG+":emu"],capture_output=True,text=True).stdout.strip():t0=time.monotonic();break
   time.sleep(2)
  if t0 is None:raise RuntimeError("launch timeout")
  while time.monotonic()-t0<AT:
   if r.poll() is not None:raise RuntimeError("runner ended early")
   time.sleep(2)
  (DEST/"saved.png").write_bytes(adb("exec-out","screencap","-p"))
  pid=adb("shell","pidof",PKG+":emu").decode().strip()
  adb("logcat","-c")
  adb("shell","am","broadcast","-a","xendroid.compose.RUNTIME_STATE","-p",PKG,"--es","operation","save","--es","slot",SLOT)
  deadline=time.monotonic()+120;res=None
  while time.monotonic()<deadline and res is None:
   log2=adb("logcat","-d","-s","EmuHost:I","*:S").decode(errors="replace")
   lines=[x for x in log2.splitlines() if "RuntimeState result operation=1" in x]
   if lines:res=lines[-1]
   time.sleep(2)
  print(res or "save result timeout",flush=True)
 finally:
  if r.poll() is None:
   os.killpg(r.pid,signal.SIGINT)
   try:r.wait(timeout=120)
   except subprocess.TimeoutExpired:pass
