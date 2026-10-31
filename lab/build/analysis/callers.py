import gzip,sys,collections
targets=sys.argv[2].split(',')
depth=int(sys.argv[3]) if len(sys.argv)>3 else 5
agg={t:collections.Counter() for t in targets}; tot=collections.Counter(); total=0
cur=None
def flush():
    global total
    if not cur or cur.get('ev')!='cpu-clock' or not cur['frames']: return
    total+=1
    top=cur['frames'][0][1]
    for t in targets:
        if top.startswith(t):
            tot[t]+=1
            ch=[]
            for ff,s in cur['frames'][1:]:
                s=s.split('(')[0][-60:] if not s.startswith('xe::cpu::Export*') else 'EXPORT:'+s[s.find('&xe::'):][:70] if '&xe::' in s else s[:40]+'..'+s[-60:]
                ch.append(s)
                if len(ch)>=depth: break
            agg[t][' < '.join(ch)]+=1
with gzip.open(sys.argv[1],'rt',errors='replace') as f:
    infile=None
    for line in f:
        s=line.strip()
        if line.startswith('sample:'): flush(); cur={'frames':[]}; infile=None
        elif line.startswith('context_switch:'): flush(); cur=None
        elif cur is not None:
            if s.startswith('event_type:'): cur['ev']=s.split(': ',1)[1]
            elif s.startswith('file:'): infile=s
            elif s.startswith('symbol:'): cur['frames'].append((infile,s.split(': ',1)[1]))
flush()
for t in targets:
    print(f'=== {t}: {100*tot[t]/total:.2f}%')
    for k,v in agg[t].most_common(12): print(f'  {100*v/total:5.2f}%  {k}')
