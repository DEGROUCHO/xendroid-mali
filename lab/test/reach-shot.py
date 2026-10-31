import subprocess,os,time,sys,signal
from pathlib import Path
# usage: reach-shot.py TAG APK GAME TITLE_ID SHOT_AT_S INPUTS_JSON
# Boots GAME with the scripted inputs, screenshots shot.png at SHOT_AT_S seconds after launch, keeps
# the xe.log tail, then closes. No savestate (image A/B in gameplay while restore is unavailable).
WORK=Path(__file__).resolve().parent;LAB=Path("/path/to/xendroid-ultimate-mali/lab")
ADB="/opt/homebrew/bin/adb";PKG="xendroid.compose"
TAG,APK,GAME,TITLE,AT,INPUTS=sys.argv[1:7];AT=int(AT)
# The runner works in another directory: a relative inputs path was silently not found = no inputs (24/10).
INPUTS=str((WORK/INPUTS).resolve()) if not os.path.isabs(INPUTS) else INPUTS
if not os.path.isfile(INPUTS): sys.exit(f'inputs file not found: {INPUTS}')
DEST=WORK/TAG;DEST.mkdir(exist_ok=True)
def adb(*a,timeout=30):return subprocess.check_output([ADB,*a],timeout=timeout)
env=os.environ.copy();env.update(XD_INPUTS_DONE_FILE=str(DEST/"inputs-complete"),XD_LAUNCH_TITLE_ID=TITLE,XD_GAME_INPUTS_FILE=INPUTS,XD_EXTRA_SHOTS="200,300,400,500,600,700,750",MEASURE_FROM="600",XD_BASE_GLOBAL=str(LAB/"build/multigame-core-audit/global-user-20261014-mig.toml"))
cmd=["bash",str(LAB/"build/analysis/arm.sh"),TAG,APK,"Kernel.savestate_enable=true,GPU.framerate_limit=0,GPU.framerate_limit_adaptive=false,GPU.guest_display_refresh_cap=true"+os.environ.get("XD_DIAG_EXTRA",""),GAME,"900"]
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
  (DEST/"shot.png").write_bytes(adb("exec-out","screencap","-p"))
  time.sleep(20)
  (DEST/"shot2.png").write_bytes(adb("exec-out","screencap","-p"))
  (DEST/"xe-tail.log").write_bytes(adb("shell","tail","-n","500","/sdcard/Android/data/"+PKG+"/files/compose/xe.log"))
  print("shot ok",flush=True)
 finally:
  if r.poll() is None:
   os.killpg(r.pid,signal.SIGINT)
   try:r.wait(timeout=120)
   except subprocess.TimeoutExpired:pass
