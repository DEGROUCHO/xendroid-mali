import sys,re,collections
names={}; cnt=collections.defaultdict(lambda: collections.Counter())
tid=None;cpu=None;per=None;insample=False
tmin=None;tmax=None
for l in sys.stdin:
    if l.startswith('record '):
        if insample and tid is not None and per==500000:
            cnt[tid][cpu]+=1
        insample = l.startswith('record sample'); tid=cpu=per=None
        iscomm = l.startswith('record comm'); continue
    s=l.strip()
    if insample:
        if s.startswith('pid '):
            m=re.match(r'pid (\d+), tid (\d+)',s); tid=int(m.group(2))
        elif s.startswith('cpu '): cpu=int(s.split()[1].rstrip(','))
        elif s.startswith('period '): per=int(s.split()[1])
        elif s.startswith('time '):
            t=int(s.split()[1]); tmin=t if tmin is None else min(tmin,t); tmax=t if tmax is None else max(tmax,t)
    else:
        m=re.match(r'pid (\d+), tid (\d+), comm (.*)',s)
        if m: names[int(m.group(2))]=m.group(3)
dur=(tmax-tmin)/1e9
print(f'duration {dur:.1f}s')
rows=[]
for t,c in cnt.items():
    tot=sum(c.values()); rows.append((tot,t,c))
rows.sort(reverse=True)
print(f"{'thread':28s} cores  | cpu0-3 (2.1) | cpu4-6 (3.0) | cpu7 (3.25) | per-cpu %")
S=[0,0,0]
for tot,t,c in rows[:22]:
    a=sum(c[i] for i in range(4)); b=sum(c[i] for i in range(4,7)); d=c[7]
    S[0]+=a;S[1]+=b;S[2]+=d
    print(f"{names.get(t,str(t))[:28]:28s} {tot*0.0005/dur:5.2f}  | {a/tot*100:5.0f}%       | {b/tot*100:5.0f}%       | {d/tot*100:5.0f}%      | "+' '.join(f"{c[i]/tot*100:3.0f}" for i in range(8)))
T=sum(S); print('TOTAL top threads: slow %.0f%% mid %.0f%% prime %.0f%%  cores slow %.2f mid %.2f prime %.2f'%(S[0]/T*100,S[1]/T*100,S[2]/T*100,S[0]*0.0005/dur,S[1]*0.0005/dur,S[2]*0.0005/dur))
