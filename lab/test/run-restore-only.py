import subprocess,os,time,sys,json,signal,pathlib
from pathlib import Path
WORK=Path(__file__).resolve().parent
LAB=WORK.parent
ADB=os.environ.get("XD_ADB","adb")
PKG=os.environ.get("XD_PKG","xendroid.compose")
BASE="/sdcard/Android/data/"+PKG+"/files/compose"
# usage: run-restore-only.py TAG APK GAME TITLE_ID [secs_after_restore]
# Boots GAME, loads its fixed save state (states/<TITLE_ID>/slot-1.xds), screenshots, closes.
TAG=sys.argv[1];APK=pathlib.Path(sys.argv[2]);GAME=sys.argv[3];TITLE=sys.argv[4]
AFTER=int(sys.argv[5]) if len(sys.argv)>5 else 20
DEST=WORK/TAG;DEST.mkdir(exist_ok=True)
def adb(*args,timeout=30):return subprocess.check_output([ADB,*args],timeout=timeout)
def capture(name):
 (DEST/(name+".png")).write_bytes(adb("exec-out","screencap","-p"))
def operation(op,timeout=180):
 pid=adb("shell","pidof",PKG+":emu").decode().strip()
 extra=["--es","slot",os.environ["XD_SLOT"]] if os.environ.get("XD_SLOT") else []
 adb("shell","am","broadcast","-a","xendroid.compose.RUNTIME_STATE","-p",PKG,"--es","operation",op,*extra)
 code={"capability":0,"save":1,"restore":2}[op];deadline=time.monotonic()+timeout
 while time.monotonic()<deadline:
  log=adb("logcat","-d","--pid="+pid,"-s","EmuHost:I","*:S").decode(errors="replace")
  (DEST/(op+"-logcat.txt")).write_text(log)
  lines=[x for x in log.splitlines() if "RuntimeState result operation="+str(code) in x]
  if lines:
   print(lines[-1],flush=True)
   return "ok=true" in lines[-1]
  time.sleep(2)
 raise RuntimeError("snapshot result timeout: "+op)
env=os.environ.copy();env.update(XD_INPUTS_DONE_FILE=str(DEST/"inputs-complete"),XD_LAUNCH_TITLE_ID=TITLE,XD_NO_INPUTS="1",XD_GAME_INPUTS_FILE=str(WORK/"snapshot-inputs.json"),XD_EXTRA_SHOTS="140,150,160,180,220,260,300",MEASURE_FROM="150",XD_BASE_GLOBAL=str(LAB/"build/multigame-core-audit/global-user-20261014-mig.toml"))
cmd=["bash",str(LAB/"build/analysis/arm.sh"),TAG,str(APK),"Kernel.savestate_enable=true,GPU.framerate_limit=0,GPU.framerate_limit_adaptive=false,GPU.guest_display_refresh_cap=true,GPU.log_guest_fps=true,Kernel.log_guest_sync_stats=true"+os.environ.get("XD_DIAG_EXTRA",""),GAME,"600"]
hash_base=int(subprocess.run([ADB,"shell","grep","-hc","'launch media hashed'",BASE+"/xe.log"],capture_output=True,text=True).stdout.strip() or 0)
with (DEST/"runner.log").open("w") as log:
 runner=subprocess.Popen(cmd,env=env,stdout=log,stderr=log,start_new_session=True)
 try:
  deadline=time.monotonic()+180
  while time.monotonic()<deadline:
   pid=subprocess.run([ADB,"shell","pidof",PKG+":emu"],capture_output=True,text=True).stdout.strip()
   if pid:break
   if runner.poll() is not None:raise RuntimeError("runner ended before launch")
   time.sleep(2)
  else:raise RuntimeError("launch timeout")
  print("Game started",flush=True)
  # xe.log is appended across boots: wait for this boot's hash line, then for
  # the title to present frames (a restore needs a running title).
  def hashes():return int(subprocess.run([ADB,"shell","grep","-hc","'launch media hashed'",BASE+"/xe.log"],capture_output=True,text=True).stdout.strip() or 0)
  hash_deadline=time.monotonic()+300;hashed=False
  while time.monotonic()<hash_deadline:
   if runner.poll() is not None:raise RuntimeError("runner ended before restore")
   c=hashes()
   if c>0 and c!=hash_base:hashed=True;break
   time.sleep(3)
  while not os.environ.get("XD_EARLY_RESTORE") and time.monotonic()<hash_deadline:
   epid=subprocess.run([ADB,"shell","pidof",PKG+":emu"],capture_output=True,text=True).stdout.strip()
   fl=subprocess.run([ADB,"logcat","-d","--pid="+epid,"-s","EmuHost:I","*:S"],capture_output=True,text=True).stdout
   if any(("fps=" in x and "fps=0.0" not in x) for x in fl.splitlines()[-5:]):break
   time.sleep(3)
  if not os.environ.get("XD_EARLY_RESTORE"):time.sleep(5)
  print("media hash ready" if hashed else "media hash wait timed out",flush=True)
  capture("before-restore")
  t0=time.monotonic()
  if not operation("restore",240):raise RuntimeError("restore failed; see logcat")
  print("restore took %.1f s"%(time.monotonic()-t0),flush=True)
  if os.environ.get("XD_SAVE_AFTER")=="0":  # immediately, before the title runs far (diff right after the restore)
   prev=os.environ.get("XD_SLOT");os.environ["XD_SLOT"]=os.environ.get("XD_SAVE_SLOT","2")
   print("post-restore save",operation("save",120),flush=True)
   if prev is None:del os.environ["XD_SLOT"]
   else:os.environ["XD_SLOT"]=prev
  time.sleep(3);capture("after-restore")
  # Optional consistency check (25/10): save to another slot XD_SAVE_AFTER s after the restore, to diff it offline
  # against the restored slot (tools-pass/xdsdiff.py). The test slot is deleted by the caller after pulling it.
  if os.environ.get("XD_SAVE_AFTER") not in (None,"","0"):
   time.sleep(max(0,int(os.environ["XD_SAVE_AFTER"])-3))
   prev=os.environ.get("XD_SLOT");os.environ["XD_SLOT"]=os.environ.get("XD_SAVE_SLOT","2")
   print("post-restore save",operation("save",120),flush=True)
   if prev is None:del os.environ["XD_SLOT"]
   else:os.environ["XD_SLOT"]=prev
  # Optional trace trigger: touch XD_TOUCH_PATH on the phone XD_TOUCH_AFTER s after the restore (recorder builds).
  if os.environ.get("XD_TOUCH_PATH"):
   time.sleep(int(os.environ.get("XD_TOUCH_AFTER","10")))
   subprocess.run([ADB,"shell","touch",os.environ["XD_TOUCH_PATH"]])
   print("trace request touched",flush=True)
  # CPU cores of the :emu process over the measured window (heat): /proc ticks, CLK_TCK 100.
  def emu_ticks():
   try:
    p=subprocess.run([ADB,"shell","pidof",PKG+":emu"],capture_output=True,text=True).stdout.strip()
    f=subprocess.run([ADB,"shell","cat","/proc/"+p+"/stat"],capture_output=True,text=True).stdout.rsplit(")",1)[1].split()
    return int(f[11])+int(f[12]),time.monotonic()
   except Exception:return None
  cpu0=emu_ticks()
  # Optional profile of the restored scene: XD_SIMPLEPERF_SECS seconds starting 5 s after the restore.
  perf_secs=int(os.environ.get("XD_SIMPLEPERF_SECS","0") or 0);perf=None
  if perf_secs:
   time.sleep(2)
   perf=subprocess.Popen([ADB,"shell","rm -f /data/local/tmp/xd_perf.data; simpleperf record --app "+PKG+f" --duration {perf_secs} -e cpu-clock -f 2000 --call-graph fp --trace-offcpu -o /data/local/tmp/xd_perf.data"],stdout=open(DEST/"simpleperf.log","w"),stderr=subprocess.STDOUT)
   time.sleep(max(AFTER-2,0))
  else:time.sleep(AFTER)
  cpu1=emu_ticks()
  if cpu0 and cpu1 and cpu1[1]>cpu0[1]:
   cores=(cpu1[0]-cpu0[0])/100.0/(cpu1[1]-cpu0[1]);(DEST/"cores.txt").write_text("%.2f\n"%cores);print("cores %.2f"%cores,flush=True)
  capture("resumed")
  if perf:
   try:
    perf.wait(timeout=perf_secs+60)
    adb("pull","/data/local/tmp/xd_perf.data",str(DEST/"perf.data"),timeout=300);adb("shell","rm -f /data/local/tmp/xd_perf.data")
    for m in subprocess.run([ADB,"shell","ls "+BASE+"/perf-*.map"],capture_output=True,text=True).stdout.split():adb("pull",m,str(DEST),timeout=120)
   except Exception as e:print("SIMPLEPERF pull failed",e,flush=True)
  print("RESTORE-ONLY cycle complete",flush=True)
 finally:
  # Signal the existing runner, whose finally block restores daily APK/config.
  if runner.poll() is None:
   os.killpg(runner.pid,signal.SIGINT)
   try:runner.wait(timeout=120)
   except subprocess.TimeoutExpired:raise RuntimeError("runner restore cleanup timeout")
  try:(DEST/"xe-tail.log").write_bytes(adb("shell","tail","-n","500",BASE+"/xe.log"))
  except Exception as e:print("xe tail failed",e,flush=True)
  # Never leave the emulator running or in Recents after a test.
  subprocess.run(["bash",str(WORK/"close-emulator.sh")])
  print("Runner stopped; daily cleanup completed",flush=True)
