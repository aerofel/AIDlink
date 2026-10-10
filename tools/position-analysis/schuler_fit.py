import json,math,re,sys
import numpy as np
D=sys.argv[1]; day0=1791590400
rows=json.load(open(D+'/rows.json'))
T=[];N=[];E=[];w=[]
def add(t,al,lf,trk,ww):
    k=math.radians(trk); T.append(t); N.append(al*math.cos(k)+lf*math.sin(k)); E.append(al*math.sin(k)-lf*math.cos(k)); w.append(ww)
for r in rows:
    if r[0]>=day0+12*3600: add(r[0],r[1],r[2],r[5],1)
add(day0+14*3600+36*60,2.49,1.10,297,5); add(day0+14*3600+53*60,1.44,0.39,295,5); add(day0+14*3600+58*60,0.57,0.25,295,5)
for l in open(D+'/drift.log'):
    m=re.match(r'(\d+):(\d+) MEAN along ([-+\d.]+)\s+left ([-+\d.]+)',l)
    if m:
        t=day0+int(m.group(1))*3600+int(m.group(2))*60; add(t,float(m.group(3)),float(m.group(4)),296 if t<day0+15*3600+7*60 else 290.5,5)
T=np.array(T);t0=T.min();tt=(T-t0)/60;w=np.array(w)
om=2*math.pi/84.4; X=lambda x:np.stack([np.ones_like(x),x,np.sin(om*x),np.cos(om*x)],1)
C=[np.linalg.lstsq(X(tt)*w[:,None]**.5,np.array(y)*w**.5,rcond=None)[0] for y in (N,E)]
print('fit: Schuler amp N %.2f E %.2f NM, linear drift N %+.2f E %+.2f NM/h'%(math.hypot(*C[0][2:]),math.hypot(*C[1][2:]),C[0][1]*60,C[1][1]*60))
for m in range(int(sys.argv[2]),int(sys.argv[3]),10):
    x=np.array([(day0+m*60-t0)/60.]); n=(X(x)@C[0])[0]; e=(X(x)@C[1])[0]
    print('%02d:%02d  model error N %+.2f E %+.2f = %.2f NM'%(m//60,m%60,n,e,math.hypot(n,e)))
