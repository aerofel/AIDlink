import json,subprocess,time,math,sys
def get(u):
    r=subprocess.run(['curl','-s','-m','6','--interface','en12',u],capture_output=True,text=True,timeout=8); return json.loads(r.stdout)
rows=[]
for i in range(int(sys.argv[1])):
    t0=time.time()
    try:
        b=get('https://api.adsb.lol/v2/reg/F-ONEO')['ac'][0]; tb=time.time()
        f=get('https://wifi.inflight.viasat.com/ac/flight/info'); tf=time.time()
    except Exception as e:
        print('err',e); time.sleep(5); continue
    fl,fo=float(f['latitude']['value']),float(f['longitude']['value'])
    # ADS-B position time = request midpoint - seen_pos; project to feed fetch time along track/gs
    tpos=(t0+tb)/2-b['seen_pos']; dt=tf-tpos
    trk=math.radians(b['track']); d=b['gs']*dt/3600
    c=math.cos(math.radians(b['lat']))
    bl=b['lat']+d*math.cos(trk)/60; bo=b['lon']+d*math.sin(trk)/60/c
    dn=(fl-bl)*60; de=(fo-bo)*60*c
    along=dn*math.cos(trk)+de*math.sin(trk); left=-(de*math.cos(trk)-dn*math.sin(trk))
    print('%s feed-minus-ADSB: along %+.2f NM  left %+.2f NM  (nic %s nacp %s sil %s, adsb age %.1fs)'%(time.strftime('%H:%M:%S',time.gmtime()),along,left,b.get('nic'),b.get('nac_p'),b.get('sil'),dt))
    rows.append((along,left))
    time.sleep(max(0,6-(time.time()-t0)))
if rows:
    import statistics as s
    print('MEAN along %+.2f  left %+.2f  (n=%d)'%(s.mean(r[0] for r in rows),s.mean(r[1] for r in rows),len(rows)))
