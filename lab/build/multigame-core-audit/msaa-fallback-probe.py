import subprocess,time,pathlib,re,json,shlex,sys
A='/opt/homebrew/bin/adb';S='YOUR_DEVICE_SERIAL';P='xendroid.compose';B='/sdcard/Android/data/'+P+'/files/compose';D=pathlib.Path(__file__).parent/'msaa-fallback';D.mkdir(exist_ok=True)
def adb(*a,raw=False):
 r=subprocess.run([A,'-s',S,*a],capture_output=True,timeout=30)
 if r.returncode: raise RuntimeError(r.stderr.decode(errors='replace')[:350])
 return r.stdout if raw else r.stdout.decode(errors='replace')
def sh(*a):return adb('shell',shlex.join(a))
def state():
 b=sh('dumpsys','battery');return {'temperature':int(re.search(r'temperature:\s*(\d+)',b)[1]),'level':int(re.search(r'level:\s*(\d+)',b)[1])}
paths=sh('find','/sdcard/Download','-maxdepth','3','-type','f','-iname','*.iso').splitlines()
paths=[p for p in paths if '' in p]
assert not sh('sh','-c','pidof '+P+':emu || true').strip(),'Emulator occupied'
assert 'com.miui.home.launcher.Launcher' in sh('dumpsys','window'),'Phone occupied'
(D/'inventory.json').write_text(json.dumps(paths,indent=2))
for i,iso in enumerate(paths):
 name=pathlib.PurePosixPath(iso).stem;dest=D/(str(i+1)+'-'+re.sub(r'[^A-Za-z0-9]+','-',name)[:65]);dest.mkdir(exist_ok=True)
 if (dest/'result.json').exists():continue
 st=state()
 while st['temperature']>=405:
  print('COOLING',st,flush=True);time.sleep(45);st=state()
 if st['level']<2:print('STOP battery',st,flush=True);break
 if 'com.miui.home.launcher.Launcher' not in sh('dumpsys','window'):print('STOP occupied',flush=True);break
 before=int(sh('sh','-c','wc -c < '+B+'/xe.log').strip());started=False;shots=[]
 try:
  print('START',name,st,flush=True)
  sh('am','start','-n',P+'/xendroid.compose.EmulatorHostActivity','-a','xendroid.intent.action.xendroid','--es','game_uri',iso,'--es','game_name',name);started=True
  for n in range(1,19):
   time.sleep(10)
   st=state();pid=sh('sh','-c','pidof '+P+':emu || true').strip()
   if not pid:print('process absent',flush=True);break
   focus=sh('dumpsys','window')
   if P not in next((x for x in focus.splitlines() if 'mCurrentFocus=' in x),''):print('STOP focus changed',flush=True);break
   if n in (2,6,12,18):
    f=dest/f'screen-{n*10}.png';f.write_bytes(adb('exec-out','screencap','-p',raw=True));shots.append(str(f))
   if st['temperature']>=465:print('STOP thermal',st,flush=True);break
   if n==2:
    sh('input','keyevent','--longpress','KEYCODE_ENTER')
    sh('input','keyevent','--longpress','KEYCODE_SPACE')
   if n in (6,12,18): (dest/f'meminfo-{n*10}.txt').write_text(sh('dumpsys','meminfo',P+':emu'))
 finally:
  if started:
   after=int(sh('sh','-c','wc -c < '+B+'/xe.log').strip());offset=before+1 if after>=before else 1
   log=sh('sh','-c',f'tail -c +{offset} {B}/xe.log');(dest/'session.log').write_text(log)
   (dest/'logcat.txt').write_text(adb('logcat','-d','-t','700'))
   sh('am','force-stop',P)
  errors=[x for x in log.splitlines() if re.search(r'(^[we]>|Unsupported|unsupported|SIGSEGV|failed|Failed|Invalid)',x)][:80]
  result={'game':name,'iso':iso,'state':st,'screens':shots,'errors':errors,'session_bytes':len(log),'note':'180s target; distinguish menus from gameplay; thermal/crash may truncate','sample_seconds':n*10}
  (dest/'result.json').write_text(json.dumps(result,indent=2));print('DONE',name,st,'errors',len(errors),'logbytes',len(log),flush=True)
 time.sleep(30)
