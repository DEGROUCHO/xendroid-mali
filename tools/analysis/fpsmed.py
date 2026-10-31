import re,sys,statistics
for p in sys.argv[1:]:
    s=[(float(a),int(b)) for a,b in re.findall(r'SAMPLE ([0-9.]+) PID \d+ FRAME (\d+)',open(p,errors='replace').read())]
    f=[(s[i+1][1]-s[i][1])/(s[i+1][0]-s[i][0]) for i in range(len(s)-1) if s[i+1][1]>s[i][1] and s[i][1]>0]
    g=f[20:-12] if len(f)>40 else f
    print(f"{p.split('/')[-1]:40} samples={len(g)} median={statistics.median(g):5.1f} mean={statistics.mean(g):5.1f} p10={sorted(g)[len(g)//10]:5.1f}")
