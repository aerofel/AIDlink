import json,sys,re,math,time,collections
j=json.load(open(sys.argv[1])); T=j['timestamp']
tr=[(T+p[0],p[1],p[2]) for p in j['trace'] if p[1] is not None]
day0=1791590400
pts=[]
for l in open(sys.argv[2]):
    m=re.match(r'^\[\d+ (\d+):(\d+):(\d+)Z\] \[poll\] feed position (\S+) -> (\S+).*\((-?[\d.]+),(-?[\d.]+)\)',l)
    if m:
        h,mi,s=map(int,m.group(1,2,3)); pts.append((day0+h*3600+mi*60+s,float(m.group(6)),float(m.group(7))))
import bisect
ts=[p[0] for p in tr]
def interp(t):
    i=bisect.bisect_right(ts,t)
    if i==0 or i>=len(tr): return None
    a,b=tr[i-1],tr[i]
    if b[0]-a[0]>180: return None
    f=(t-a[0])/(b[0]-a[0]); c=math.cos(math.radians(a[1]))
    trk=math.degrees(math.atan2((b[2]-a[2])*c,b[1]-a[1]))
    return a[1]+f*(b[1]-a[1]),a[2]+f*(b[2]-a[2]),trk
rows=[]
for t,la,lo in pts:
    r=interp(t)
    if not r: continue
    bl,bo,trk=r; c=math.cos(math.radians(bl)); tk=math.radians(trk)
    dn=(la-bl)*60; de=(lo-bo)*60*c
    if math.hypot(dn,de)>30: continue
    rows.append((t,dn*math.cos(tk)+de*math.sin(tk),-(de*math.cos(tk)-dn*math.sin(tk)),bl,bo,trk))
print('matched',len(rows),'of',len(pts),'feed points')
B=collections.OrderedDict()
for r in rows: B.setdefault(int(r[0]//300)*300,[]).append(r)
for k,v in B.items():
    n=len(v); al=sum(x[1] for x in v)/n; lf=sum(x[2] for x in v)/n
    print('%s n=%3d along %+.2f left %+.2f total %.2f NM  trk %3.0f  at %.2f,%.2f'%(time.strftime('%H:%M',time.gmtime(k)),n,al,lf,math.hypot(al,lf),v[0][5]%360,v[0][3],v[0][4]))
json.dump(rows,open(sys.argv[3],'w'))
