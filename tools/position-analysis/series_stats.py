import json,math,sys
import numpy as np
R=[json.loads(l) for l in open(sys.argv[1])]
lat0=R[0]['f'][1]; lon0=R[0]['f'][2]; c=math.cos(math.radians(lat0))
def enu(la,lo): return ((lo-lon0)*60*c,(la-lat0)*60)  # NM east,north
F=[(r['tf'],*enu(r['f'][1],r['f'][2]),r['f'][0]) for r in R if r['f']]
A=[(r['ta'],*enu(r['a'][0],r['a'][1]),r['a'][2],r['a'][3]) for r in R if r['a'] and len(r['a'])>=4]
print('feed samples',len(F),'adbp samples',len(A),'failed feed',sum(1 for r in R if not r['f']))
# distinct feed updates
d=[F[0]]; 
for x in F[1:]:
    if (x[1],x[2])!=(d[-1][1],d[-1][2]): d.append(x)
print('distinct feed positions',len(d),'repeats',len(F)-len(d), 'distinct current_time',len(set(x[3] for x in F)))
t=np.array([x[0] for x in F]); E=np.array([x[1] for x in F]); N=np.array([x[2] for x in F])
pe=np.polyfit(t-t[0],E,1); pn=np.polyfit(t-t[0],N,1)
gs=math.hypot(pe[0],pn[0])*3600; trk=math.degrees(math.atan2(pe[0],pn[0]))%360
print('feed linear fit GS %.0f kt track %.1f'%(gs,trk))
u=np.array([pe[0],pn[0]])/math.hypot(pe[0],pn[0]); n=np.array([-u[1],u[0]])  # n = left normal
def resid(tt,e,nn):
    pe_=np.polyval(pe,tt-t[0]); pn_=np.polyval(pn,tt-t[0])
    v=np.stack([e-pe_,nn-pn_],1)
    return v@u, v@n
al,xt=resid(t,E,N)
print('FEED  along-track resid NM: mean %+.3f sd %.3f min %+.3f max %+.3f'%(al.mean(),al.std(),al.min(),al.max()))
print('FEED  cross-track(+left) NM: mean %+.3f sd %.3f min %+.3f max %+.3f'%(xt.mean(),xt.std(),xt.min(),xt.max()))
ta=np.array([x[0] for x in A]); Ea=np.array([x[1] for x in A]); Na=np.array([x[2] for x in A])
al2,xt2=resid(ta,Ea,Na)
print('ADBP  along-track resid NM: mean %+.3f sd %.3f min %+.3f max %+.3f'%(al2.mean(),al2.std(),al2.min(),al2.max()))
print('ADBP  cross-track(+left) NM: mean %+.3f sd %.3f min %+.3f max %+.3f'%(xt2.mean(),xt2.std(),xt2.min(),xt2.max()))
trks=np.array([(x[3])%360 for x in A]); g=np.array([x[4] for x in A])
print('ADBP track deg: mean %.1f sd %.1f min %.1f max %.1f'%(trks.mean(),trks.std(),trks.min(),trks.max()))
print('ADBP GS kt: mean %.0f sd %.0f min %.0f max %.0f'%(g.mean(),g.std(),g.min(),g.max()))
# per-update implied speed in feed
sp=[]
for a,b in zip(d,d[1:]):
    dt=b[0]-a[0]; ds=math.hypot(b[1]-a[1],b[2]-a[2]); sp.append(ds/dt*3600 if dt>0 else 0)
print('feed step speeds kt (sample-time based):',' '.join('%d'%s for s in sp[:60]))
iv=[b[0]-a[0] for a,b in zip(d,d[1:])]
print('feed update intervals s: median %.1f max %.1f'%(np.median(iv),max(iv)))
