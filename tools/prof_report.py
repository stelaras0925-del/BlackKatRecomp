import subprocess,collections,sys,bisect
samples,mapsf=sys.argv[1],sys.argv[2]
maps=[]
for l in open(mapsf):
    p=l.split(None,5)
    a,b=[int(x,16) for x in p[0].split('-')]
    maps.append((a,b,int(p[2],16),p[5].strip() if len(p)>5 else '?'))
rows=[l.split() for l in open(samples)]
addrs=set()
for r in rows:
    for x in [r[2]]+r[3:]: addrs.add(int(x,16))
loc={}
byfile=collections.defaultdict(list)
for a in addrs:
    for lo,hi,o,path in maps:
        if lo<=a<hi: byfile[path].append((a,a-lo+o)); break
    else: loc[a]='?'
for path,lst in byfile.items():
    if path.endswith('blackkat'):
        r=subprocess.run(['addr2line','-f','-C','-e',path]+[hex(o) for _,o in lst],capture_output=True,text=True).stdout.split('\n')
        for i,(a,o) in enumerate(lst): loc[a]=r[2*i].split('(')[0][:70]
    elif path.startswith('/'):
        out=subprocess.run(['nm','-D','--defined-only','-C',path],capture_output=True,text=True).stdout
        syms=sorted((int(q[0],16),q[2]) for q in (s.split(None,2) for s in out.splitlines()) if len(q)==3 and q[1] in 'TtWi')
        ks=[k for k,_ in syms]
        for a,o in lst:
            i=bisect.bisect_right(ks,o)-1
            loc[a]=path.split('/')[-1]+':'+(syms[i][1][:50] if i>=0 else '?')
    else:
        for a,o in lst: loc[a]=path
tid=collections.Counter(r[0] for r in rows); print(tid.most_common(4))
hot=tid.most_common(1)[0][0]
incl=collections.Counter(); chains=collections.Counter(); self_=collections.Counter()
for r in rows:
    if r[0]!=hot: continue
    fr=[loc[int(r[2],16)]]+[loc[int(x,16)] for x in r[4:]]  # r[3] = backtrace's own frame (handler)
    self_[fr[0]]+=1
    for f in set(fr): incl[f]+=1
    chains[' < '.join(fr[:5])]+=1
n=sum(1 for r in rows if r[0]==hot)
print('hot tid',hot,n)
print('--- inclusive'); 
for k,v in incl.most_common(45): print(v,k)
print('--- chains')
for k,v in chains.most_common(25): print(v,k)
