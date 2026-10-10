import json,subprocess,time,re,sys
Q='<?xml version="1.0" encoding="UTF-8"?><method name="getParameters"><parameters><parameter name="GPSLAT"/><parameter name="GPSLONG"/><parameter name="GPSTTRKA"/><parameter name="GPSGS"/></parameters></method>'
out=open(sys.argv[1],'w')
t_end=time.time()+float(sys.argv[2])
while time.time()<t_end:
    t0=time.time()
    try:
        r=subprocess.run(['curl','-s','-m','3','--interface','en12','https://wifi.inflight.viasat.com/ac/flight/info'],capture_output=True,text=True,timeout=5)
        j=json.loads(r.stdout); t1=time.time()
        f=(j['current_time']['value'],float(j['latitude']['value']),float(j['longitude']['value']),j['latitude'].get('updated_at'))
    except Exception as e:
        f=None; t1=time.time()
    try:
        r=subprocess.run(['nc','-s','172.20.1.2','-w','2','172.20.1.1','24000'],input=Q,capture_output=True,text=True,timeout=4)
        a=[float(x) for x in re.findall(r'value="([-0-9.]+)"',r.stdout)]
    except Exception: a=None
    t2=time.time()
    out.write(json.dumps({'tf':(t0+t1)/2,'f':f,'ta':t2,'a':a})+'\n'); out.flush()
    time.sleep(max(0,1-(time.time()-t0)))
