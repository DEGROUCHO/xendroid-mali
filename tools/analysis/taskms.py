import re,sys,collections
def load(p):
    S=[];cur=None;pend=None
    for line in open(p,errors='replace'):
        line=line.rstrip('\n')
        m=re.match(r'SAMPLE ([0-9.]+) PID \d+ FRAME (\d+)',line)
        if m: cur={'t':float(m.group(1)),'f':int(m.group(2)),'r':{},'n':{},'proc':None};S.append(cur);continue
        if cur is None: continue
        m=re.match(r'TASK (\d+) (.*)$',line)
        if m: pend=(int(m.group(1)),m.group(2));continue
        if pend and re.match(r'^\d+ \d+ \d+$',line):
            cur['r'][pend[0]]=int(line.split()[0]);cur['n'][pend[0]]=pend[1];pend=None;continue
        m=re.match(r'\d+ \(\.compose:emu\) \S (.*)$',line)
        if m and cur['proc'] is None:
            rest=m.group(1).split(); cur['proc']=int(rest[10])+int(rest[11])
    return S
for p in sys.argv[1:]:
    S=[s for s in load(p) if s['r'] and s['proc'] is not None and s['f']>0]
    S=S[-100:]
    a,b=S[0],S[-1]; frames=b['f']-a['f']; dt=b['t']-a['t']
    print(f"== {p.split('/')[-1]} frames={frames} dt={dt:.1f}s fps={frames/dt:.1f} process={(b['proc']-a['proc'])*10/frames:.1f} ms/frame ({(b['proc']-a['proc'])/dt:.0f}% cpu)")
    for tid,r in sorted(b['r'].items()):
        if tid in a['r']:
            ms=(r-a['r'][tid])/1e6
            if ms/dt>10: print(f"  {tid} {b['n'][tid]:14s} {ms/frames:6.2f} ms/frame {ms/10/dt:5.1f}%")
