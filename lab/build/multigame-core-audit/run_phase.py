import pathlib,sys,re,subprocess,os
W=pathlib.Path(__file__).parent;A='/opt/homebrew/bin/adb';S='YOUR_DEVICE_SERIAL';B='/sdcard/Android/data/xendroid.compose/files/compose/xenia-canary.config.toml'
phase=sys.argv[1];setting=sys.argv[2] if len(sys.argv)>2 else '';filter_=sys.argv[3] if len(sys.argv)>3 else ''
s=(W/'probe180.py').read_text().replace("/'baseline-180s'",f"/'{phase}'").replace("if st['temperature']>=420", "if st['temperature']>=450").replace("if st['temperature']>=420 or", "if st['temperature']>=430 or")
s=s.replace("paths=sh('find'", "paths=sh('find'")
s=s.replace("assert not sh('sh'", f"paths=[p for p in paths if {filter_!r} in p]\nassert not sh('sh'",1)
s=s.replace("if st['temperature']>=450 or st['level']<2:print('STOP thermal/battery',st,flush=True);break", "while st['temperature']>=410:\n  print('COOLING',st,flush=True);time.sleep(45);st=state()\n if st['level']<2:print('STOP battery',st,flush=True);break")
s=s.replace('time.sleep(15)','time.sleep(30)')
s=s.replace("st['temperature']>=450","st['temperature']>=465").replace("st['temperature']>=410","st['temperature']>=405")
s=s.replace("if n==2:sh('input','keyevent','--longpress','KEYCODE_SPACE')","if n==2:\n    sh('input','keyevent','--longpress','KEYCODE_ENTER')\n    sh('input','keyevent','--longpress','KEYCODE_SPACE')")
script=W/(phase+'-probe.py');script.write_text(s)
def adb(*a):
 r=subprocess.run([A,'-s',S,*a],capture_output=True,timeout=30);assert r.returncode==0,r.stderr;return r.stdout
original=adb('shell','cat '+B);backup=W/(phase+'-original-global.toml');backup.write_bytes(original)
try:
 if setting:
  text=original.decode()
  for assignment in setting.split(','):
   k,v=assignment.split('=',1);text,n=re.subn(r'(?m)^'+re.escape(k)+r'\s*=.*$',k+' = '+v,text);assert n==1,n
  cfg=W/(phase+'-global.toml');cfg.write_text(text);adb('push',str(cfg),B)
 r=subprocess.run([sys.executable,str(script)]);raise SystemExit(r.returncode)
finally:
 if setting:adb('push',str(backup),B);print('GLOBAL RESTORED',flush=True)
