import sys,os,re,glob
g=sys.argv[1]
pat=re.compile(r'(MESA_SHADER_\w+) shader: (\d+) inst, (\d+) bundles, (\d+) registers, (\d+) threads, (\d+) quadwords, (\d+) loops, (\d+):(\d+) spills:fills')
def load(d):
    r={};bad=set()
    for f in glob.glob(d+'/*.txt'):
        t=open(f,errors='replace').read(); n=os.path.basename(f)
        if 'rc=0' not in t or 'ERROR' in t or 'Assert' in t: bad.add(n)
        r[n]=[tuple(map(int,m.groups()[1:])) for m in pat.finditer(t)]
    return r,bad
b,bb=load('base-'+g); a,ab=load('a-'+g)
print(g,'files',len(b),len(a),'bad base',len(bb),'bad new',len(ab),'newly bad',sorted(ab-bb)[:5],'fixed',len(bb-ab))
tot=lambda r,i:sum(s[i] for v in r.values() for s in v)
nb=sum(len(v) for v in b.values()); na=sum(len(v) for v in a.values())
print(' shaders',nb,na)
for i,nm in ((0,'inst'),(1,'bundles'),(4,'quadwords'),(7,'fills')):
    print(' total',nm,tot(b,i),'->',tot(a,i),'%+.2f%%'%(100*(tot(a,i)-tot(b,i))/max(tot(b,i),1)))
ch=[(n) for n in b if b[n]!=a.get(n)]
print(' files changed',len(ch))
d=[]
for n in ch:
    if len(b[n])==len(a[n]):
        for x,y in zip(b[n],a[n]):
            if x!=y: d.append((y[4]-x[4],n,x[4],y[4]))
d.sort()
print(' quadword shrink:',sum(1 for x in d if x[0]<0),'grow:',sum(1 for x in d if x[0]>0))
for x in d[:3]+d[-5:]: print('  ',x)
