import gzip,sys,collections,json
th=collections.Counter(); fi=collections.Counter(); sy=collections.Counter(); thsy=collections.defaultdict(collections.Counter)
incl=collections.Counter()
cur=None; total=0
def flush():
    global total
    if not cur or cur.get('ev')!='cpu-clock': return
    total+=1
    t=cur.get('thread_name','?'); th[t]+=1
    f=cur['frames'][0] if cur['frames'] else ('?','?')
    fn=f[0].split('/')[-1]
    fi[fn]+=1
    name=f[1] if fn!='?' else '?'
    key=(fn,name[:110]); sy[key]+=1; thsy[t][key]+=1
    seen=set()
    for ff,s in cur['frames']:
        k=(ff.split('/')[-1],s[:110])
        if k not in seen: seen.add(k); incl[k]+=1
with gzip.open(sys.argv[1],'rt',errors='replace') as f:
    infile=None
    for line in f:
        s=line.strip()
        if line.startswith('sample:'):
            flush(); cur={'frames':[]}; infile=None
        elif line.startswith('context_switch:') or line.startswith('meta_info'):
            flush(); cur=None
        elif cur is not None:
            if s.startswith('event_type:'): cur['ev']=s.split(': ',1)[1]
            elif s.startswith('thread_name:'): cur['thread_name']=s.split(': ',1)[1]
            elif s.startswith('file:'): infile=s.split(': ',1)[1]
            elif s.startswith('symbol:'):
                cur['frames'].append((infile or '?', s.split(': ',1)[1])); infile=None
flush()
out={'total':total}
print('TOTAL',total)
print('--threads'); 
for k,v in th.most_common(25): print(f'{100*v/total:6.2f}% {k}')
print('--files')
for k,v in fi.most_common(15): print(f'{100*v/total:6.2f}% {k}')
print('--self symbols')
for k,v in sy.most_common(70): print(f'{100*v/total:6.2f}% {k[0][:22]:22} {k[1]}')
print('--inclusive (libe.so)')
n=0
for k,v in incl.most_common(400):
    if k[0]=='libe.so' and n<70: n+=1; print(f'{100*v/total:6.2f}% {k[1]}')
json.dump({t:{f'{k[0]}|{k[1]}':v for k,v in c.most_common(40)} for t,c in thsy.items()},open(sys.argv[2],'w'),indent=0)
