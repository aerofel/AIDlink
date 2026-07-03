// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
// ============================================================================
//  aidlink — ESP32 2.4 GHz Wi-Fi bridge (AP + STA + NAT) and an experimental
//            ARINC 834 ADBP position feed for bench / interoperability testing,
//            with an on-device web configuration portal.
//
//  EXPERIMENTAL — NOT CERTIFIED, NOT FOR OPERATIONAL USE. The position output has
//  no integrity guarantee; never use it for navigation. Independent project, not
//  affiliated with any aircraft, avionics, EFB, airline, or connectivity vendor.
// ============================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <math.h>
#include <time.h>
#define FW_BUILD ("v6 " __DATE__ " " __TIME__)
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "dhcpserver/dhcpserver.h"
#include "defs.h"

// ----------------------------- CONFIG (persisted in NVS) --------------------
static Config cfg;
static Preferences prefs;

static void loadConfig(){
  prefs.begin("aidlink", true);
  cfg.staSsid = prefs.getString("staSsid","");
  cfg.staPass = prefs.getString("staPass","88888888");
  cfg.staDhcp = prefs.getBool  ("staDhcp",true);
  cfg.staIp   = prefs.getString("staIp","");
  cfg.staGw   = prefs.getString("staGw","");
  cfg.staMask = prefs.getString("staMask","255.255.255.0");
  cfg.staDns  = prefs.getString("staDns","");
  cfg.apSsid  = prefs.getString("apSsid","AIDlink");
  cfg.apPass  = prefs.getString("apPass","88888888");
  cfg.apHidden= prefs.getBool  ("apHidden",false);
  cfg.apIp    = prefs.getString("apIp","172.20.1.1");
  cfg.apMask  = prefs.getString("apMask","255.255.255.192");
  cfg.apLease = prefs.getString("apLease","172.20.1.2");
  cfg.apChannel    = prefs.getInt("apChannel",0);      // 0 = follow STA
  cfg.apMaxClients = prefs.getInt("apMaxClients",8);
  cfg.apDhcpCount  = prefs.getInt("apDhcpCount",60);
  cfg.apLeaseMin   = prefs.getInt("apLeaseMin",120);
  cfg.apClientDns  = prefs.getString("apClientDns","");
  cfg.srcType = prefs.getInt   ("srcType",0);    // 0=Viasat, 1=Panasonic, 2=custom
  cfg.vsUrl   = prefs.getString("vsUrl","http://192.168.4.2:8080/flight/info");   // custom/test URL default (example only)
  cfg.pollMs  = prefs.getUInt  ("pollMs",10000);
  cfg.staleMs = prefs.getUInt  ("staleMs",15000);
  cfg.simEnable=prefs.getBool  ("simEnable",false);   // emulator override off by default; use the live source
  cfg.simLat  = prefs.getDouble("simLat",1.359);
  cfg.simLon  = prefs.getDouble("simLon",103.99);
  cfg.simTrk  = prefs.getDouble("simTrk",120.0);
  cfg.simGs   = prefs.getDouble("simGs",470.0);
  cfg.simAlt  = prefs.getDouble("simAlt",39000.0);
  cfg.adbpPort= prefs.getInt   ("adbpPort",24000);
  cfg.dsPort  = prefs.getInt   ("dsPort",51000);
  cfg.naptEnable=prefs.getBool ("napt",true);
  cfg.devName = prefs.getString("devName","AIDlink");
  cfg.acTail  = prefs.getString("acTail","TEST01");
  cfg.acType  = prefs.getString("acType","TEST");
  cfg.apiVer  = prefs.getString("apiVer","3.1");   // AID API version
  cfg.frameLen        = prefs.getInt ("frameLen",1);       // 1=method-element length (excl. XML prolog)
  cfg.frameDelim      = prefs.getInt ("frameDelim",0);     // the review next test: NONE (length frames it; no trailing CRLF)
  cfg.framePrologEach = prefs.getBool("framePrologEach",true);
  cfg.cfgVer          = prefs.getInt ("cfgVer",0);
  cfg.logEnable       = prefs.getBool("logEnable",false);   // traffic logging off by default; enable for debugging
  cfg.authEnable      = prefs.getBool ("authEnable",true);  // settings login on by default (admin/password)
  cfg.authUser        = prefs.getString("authUser","admin");
  cfg.authHash        = prefs.getString("authHash","");
  cfg.authSalt        = prefs.getString("authSalt","");
  prefs.end();
}
static void saveConfig(){
  prefs.begin("aidlink", false);
  prefs.putString("staSsid",cfg.staSsid); prefs.putString("staPass",cfg.staPass);
  prefs.putBool("staDhcp",cfg.staDhcp); prefs.putString("staIp",cfg.staIp); prefs.putString("staGw",cfg.staGw);
  prefs.putString("staMask",cfg.staMask); prefs.putString("staDns",cfg.staDns);
  prefs.putString("apSsid",cfg.apSsid);   prefs.putString("apPass",cfg.apPass);
  prefs.putBool("apHidden",cfg.apHidden);
  prefs.putString("apIp",cfg.apIp); prefs.putString("apMask",cfg.apMask); prefs.putString("apLease",cfg.apLease);
  prefs.putInt("apChannel",cfg.apChannel); prefs.putInt("apMaxClients",cfg.apMaxClients);
  prefs.putInt("apDhcpCount",cfg.apDhcpCount); prefs.putInt("apLeaseMin",cfg.apLeaseMin);
  prefs.putString("apClientDns",cfg.apClientDns);
  prefs.putInt("srcType",cfg.srcType); prefs.putString("vsUrl",cfg.vsUrl);
  prefs.putUInt("pollMs",cfg.pollMs); prefs.putUInt("staleMs",cfg.staleMs);
  prefs.putBool("simEnable",cfg.simEnable);
  prefs.putDouble("simLat",cfg.simLat); prefs.putDouble("simLon",cfg.simLon);
  prefs.putDouble("simTrk",cfg.simTrk); prefs.putDouble("simGs",cfg.simGs); prefs.putDouble("simAlt",cfg.simAlt);
  prefs.putInt("adbpPort",cfg.adbpPort); prefs.putInt("dsPort",cfg.dsPort); prefs.putBool("napt",cfg.naptEnable);
  prefs.putString("devName",cfg.devName);
  prefs.putInt("frameLen",cfg.frameLen); prefs.putInt("frameDelim",cfg.frameDelim); prefs.putBool("framePrologEach",cfg.framePrologEach);
  prefs.putInt("cfgVer",cfg.cfgVer); prefs.putBool("logEnable",cfg.logEnable);
  prefs.putBool("authEnable",cfg.authEnable); prefs.putString("authUser",cfg.authUser);
  prefs.putString("authHash",cfg.authHash); prefs.putString("authSalt",cfg.authSalt);
  prefs.putString("acTail",cfg.acTail); prefs.putString("acType",cfg.acType); prefs.putString("apiVer",cfg.apiVer);
  prefs.end();
}

// ----------------------------- STATE ----------------------------------------
static PosState g;
static SemaphoreHandle_t gMux;
static const int MAX_SUBS=6; static Sub subs[MAX_SUBS];
static WiFiClient pushCli[MAX_SUBS];   // ONE persistent data-stream connection per subscription

// connected-client tracking (MAC->IP learned from DHCP-assign event)
struct CliRec { bool used=false; uint8_t mac[6]={0}; uint32_t ip=0; uint32_t seen=0; };
static const int MAX_CLI=10; static CliRec cli[MAX_CLI];
static void noteClient(const uint8_t* mac, uint32_t ip){
  int free=-1;
  for(int i=0;i<MAX_CLI;i++){ if(cli[i].used && memcmp(cli[i].mac,mac,6)==0){ cli[i].ip=ip; cli[i].seen=millis(); return; } if(!cli[i].used&&free<0) free=i; }
  if(free<0){ free=0; uint32_t oldest=cli[0].seen; for(int i=1;i<MAX_CLI;i++) if(cli[i].seen<oldest){oldest=cli[i].seen;free=i;} }
  cli[free].used=true; memcpy(cli[free].mac,mac,6); cli[free].ip=ip; cli[free].seen=millis();
}
static uint32_t lookupClientIp(const uint8_t* mac){ for(int i=0;i<MAX_CLI;i++) if(cli[i].used&&memcmp(cli[i].mac,mac,6)==0) return cli[i].ip; return 0; }
static void onWiFiEvent(WiFiEvent_t e, WiFiEventInfo_t info){
  if(e==ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED)
    noteClient(info.wifi_ap_staipassigned.mac, info.wifi_ap_staipassigned.ip.addr);
}

static WiFiServer adbp(24000);
static WebServer  web(80);
static double SIM_LAT, SIM_LON;   // working sim position

// forward declarations (suppress Arduino auto-prototype ordering issue with PosState)
static void snapshot(PosState& s);
static bool paramXml(const String& name, const PosState& s, char* out, size_t cap);
static String esc(const String& s);

// live poll status (for the web UI Feed indicator)
static bool gPollOk=false; static uint32_t gPollAtMs=0; static char gPollMsg[48]="idle";

// traffic log ring buffer (viewable at /log) — for diagnosing EFB clients
static SemaphoreHandle_t logMux;
static const int LOG_N=90; static String logBuf[LOG_N]; static int logHead=0;
static void logln(const String& s){
  if(!cfg.logEnable) return;   // master traffic-log switch (settings page)
  String line="["+String(millis()/1000)+"s] "+s;
  if(logMux) xSemaphoreTake(logMux,portMAX_DELAY);
  logBuf[logHead]=line; logHead=(logHead+1)%LOG_N;
  if(logMux) xSemaphoreGive(logMux);
  Serial.println(line);
}
// verbose log: serial-ONLY (captured to the host file) so it never floods the small /log ring buffer
static void vlog(const String& s){ if(!cfg.logEnable)return; Serial.print("["); Serial.print(millis()/1000); Serial.print("s] "); Serial.println(s); }
static void vlogChunked(const String& tag,const String& data){   // dump a long blob across lines, serial-only
  if(!cfg.logEnable)return; String d=data; d.replace("\r"," "); d.replace("\n"," ");
  vlog(tag+" ("+String(data.length())+"B):");
  for(int o=0,p=1;o<(int)d.length();o+=480,p++) vlog("    {"+String(p)+"} "+d.substring(o,o+480));
}
// Dump the whole ring buffer to USB serial, framed by markers (read on-demand by the host, no reboot).
static void dumpLog(){
  Serial.println(String("---LOG START ")+FW_BUILD+" up="+String(millis()/1000)+"s---");
  if(logMux) xSemaphoreTake(logMux,portMAX_DELAY);
  for(int i=0;i<LOG_N;i++){ int idx=(logHead+i)%LOG_N; if(logBuf[idx].length()) Serial.println(logBuf[idx]); }
  if(logMux) xSemaphoreGive(logMux);
  Serial.println("---LOG END---");
}
// Serial console commands: 'L' dump log, 'C' clear log.
static void serialCmd(){
  while(Serial.available()){ int c=Serial.read();
    if(c=='L'||c=='l') dumpLog();
    else if(c=='C'||c=='c'){ if(logMux) xSemaphoreTake(logMux,portMAX_DELAY);
      for(int i=0;i<LOG_N;i++) logBuf[i]=""; logHead=0; if(logMux) xSemaphoreGive(logMux); Serial.println("---LOG CLEARED---"); } }
}

// ----------------------------- UTIL -----------------------------------------
static long days_from_civil(int y,unsigned m,unsigned d){
  y-=m<=2; long era=(y>=0?y:y-399)/400; unsigned yoe=(unsigned)(y-era*400);
  unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1; unsigned doe=yoe*365+yoe/4-yoe/100+doy;
  return era*146097L+(long)doe-719468L;
}
static uint64_t isoToEpochMs(const char* s){ int Y,Mo,D,h,mi,se;
  if(sscanf(s,"%d-%d-%dT%d:%d:%d",&Y,&Mo,&D,&h,&mi,&se)==6){
    long days=days_from_civil(Y,(unsigned)Mo,(unsigned)D);
    return ((uint64_t)days*86400ULL+(uint64_t)h*3600+mi*60+se)*1000ULL; } return 0; }
static double bearingDeg(double la1,double lo1,double la2,double lo2){
  double p1=la1*M_PI/180,p2=la2*M_PI/180,dl=(lo2-lo1)*M_PI/180;
  double y=sin(dl)*cos(p2),x=cos(p1)*sin(p2)-sin(p1)*cos(p2)*cos(dl);
  double b=atan2(y,x)*180/M_PI; if(b<0)b+=360; return b; }
static double norm180(double d){ while(d>180.0)d-=360.0; while(d<-180.0)d+=360.0; return d; }  // the EFB requires track/hdg in -180..180
static double distNmGC(double la1,double lo1,double la2,double lo2){   // great-circle distance (NM)
  double p1=la1*M_PI/180,p2=la2*M_PI/180,dp=(la2-la1)*M_PI/180,dl=(lo2-lo1)*M_PI/180;
  double a=sin(dp/2)*sin(dp/2)+cos(p1)*cos(p2)*sin(dl/2)*sin(dl/2);
  return 3440.065*2*atan2(sqrt(a),sqrt(1-a)); }
static void advanceLL(double la,double lo,double trkDeg,double distNm,double& outLa,double& outLo){ // move along track
  double dRad=distNm/3440.065, b=trkDeg*M_PI/180, p=la*M_PI/180, l=lo*M_PI/180;
  double p2=asin(sin(p)*cos(dRad)+cos(p)*sin(dRad)*cos(b));
  double l2=l+atan2(sin(b)*sin(dRad)*cos(p),cos(dRad)-sin(p)*sin(p2));
  outLa=p2*180/M_PI; outLo=l2*180/M_PI; }
// Find the position of the actual value for `key`. Handles BOTH a flat value
// (  "key": X  or  "key":"X"  ) and the nested object form used by the source feed
// (  "key": {"attr":{...}, "updated_at": <ts>, "value": "X"}  ) — where naively
// taking the first token after the key would wrongly grab `updated_at`.
static int jsonValuePos(const String& j,const char* key){
  String k=String("\"")+key+"\""; int i=j.indexOf(k); if(i<0)return -1; i+=k.length();
  int n=j.length(); while(i<n && (j[i]==' '||j[i]=='\t'||j[i]==':')) i++;
  if(i<n && j[i]=='{'){                                   // nested object -> jump to its "value" member
    int ve=j.indexOf("\"value\"",i); if(ve<0)return -1; i=ve+7;
    while(i<n && (j[i]==' '||j[i]=='\t'||j[i]==':')) i++; }
  return i; }
static bool jsonNum(const String& j,const char* key,double& out){
  int i=jsonValuePos(j,key); if(i<0)return false; int n=j.length();
  while(i<n && j[i]=='"') i++;                            // value may be a quoted number ("-7.98311")
  while(i<n && !((j[i]>='0'&&j[i]<='9')||j[i]=='-'||j[i]=='+'||j[i]=='.')){ if(j[i]=='}'||j[i]==','||j[i]=='"')return false; i++; }
  if(i>=n)return false; int s=i;
  while(i<n){char c=j[i]; if((c>='0'&&c<='9')||c=='-'||c=='+'||c=='.'||c=='e'||c=='E')i++; else break;}
  if(i==s)return false; out=atof(j.substring(s,i).c_str()); return true; }
static bool jsonStr(const String& j,const char* key,char* out,size_t cap){
  int i=jsonValuePos(j,key); if(i<0)return false;
  int q=j.indexOf('"',i); if(q<0)return false; int e=j.indexOf('"',q+1); if(e<0)return false;
  j.substring(q+1,e).toCharArray(out,cap); return true; }

// ----------------------------- POSITION SOURCE POLL -------------------------
// parse  scheme://host[:port]/path  (scheme optional, defaults http)
static bool parseUrl(const String& url, bool& https, String& host, int& port, String& path){
  String u=url; u.trim(); https=false;
  int i=u.indexOf("://");
  if(i>=0){ https = u.substring(0,i).equalsIgnoreCase("https"); u=u.substring(i+3); }
  int slash=u.indexOf('/');
  String hp = slash<0 ? u : u.substring(0,slash);
  path = slash<0 ? "/" : u.substring(slash);
  int colon=hp.indexOf(':');
  if(colon<0){ host=hp; port=https?443:80; }
  else { host=hp.substring(0,colon); port=hp.substring(colon+1).toInt(); if(port<=0)port=https?443:80; }
  return host.length()>0;
}
// Official position-source endpoints (selectable in settings). Both are vendor moving-map feeds.
#define URL_VIASAT "https://wifi.inflight.viasat.com/ac/flight/info"
#define URL_PANA   "http://services.inflightpanasonic.aero/inflight/services/flightdata/v1/flightdata"
static String activeUrl(){ return cfg.srcType==0?String(URL_VIASAT):cfg.srcType==1?String(URL_PANA):cfg.vsUrl; }
// Panasonic lat/lon: 8-digit string, degrees*1000, value >= 80000000 encodes the negative sign.
static double panLL(const char* s){ long v=atol(s); return (v>=80000000L)? -((v-80000000L)/1000.0) : (v/1000.0); }
static bool fetchFlightInfo(String& body){
  bool https; String host,path; int port;
  if(!parseUrl(activeUrl(), https, host, port, path)){ strcpy(gPollMsg,"bad url"); return false; }
  WiFiClientSecure sc; WiFiClient pc; WiFiClient* c;
  if(https){ sc.setInsecure(); sc.setTimeout(4000); c=&sc; } else { pc.setTimeout(4000); c=&pc; }
  if(!c->connect(host.c_str(), port)){ snprintf(gPollMsg,sizeof gPollMsg,"connect fail %s:%d",host.c_str(),port); return false; }
  c->printf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: aidlink\r\nConnection: close\r\n\r\n",
            path.c_str(), host.c_str());
  uint32_t t0=millis(); bool inBody=false; body="";
  // drain the socket even after the server closes it (fast local servers close
  // immediately after the response — don't gate the read on connected())
  while(millis()-t0<5000){
    while(c->available()){ String line=c->readStringUntil('\n'); t0=millis();
      if(!inBody){ if(line.length()<=1) inBody=true; } else body+=line+"\n"; }
    if(!c->connected() && !c->available()) break;
    delay(4);
  }
  c->stop();
  int b=body.indexOf('{'); if(b<0){ strcpy(gPollMsg,"no json in reply"); return false; }
  body=body.substring(b); return true;
}
static void applyFix(double lat,double lon,double alt,double gs,uint64_t utcMs,bool sim){
  uint32_t nowMs=millis();
  xSemaphoreTake(gMux,portMAX_DELAY);
  if(g.valid){ g.prevLat=g.lat; g.prevLon=g.lon; g.havePrev=true; g.prevFixMs=g.lastFixMs; }
  if(g.havePrev){ double d=fabs(lat-g.prevLat)+fabs(lon-g.prevLon);
    if(d>1e-6){ g.trackDeg=bearingDeg(g.prevLat,g.prevLon,lat,lon); g.haveTrack=true; }
    // derive ground speed from successive fixes (so GS is coherent even when the source reports 0).
    // CLAMP to a sane range (don't reject): rejecting left gsDerivedKt at 0 while the position kept
    // moving, so the pushed GS disagreed with the motion. Clamping keeps GS == the speed the
    // dead-reckoner advances at, so reported GS and position motion stay consistent.
    if(g.prevFixMs>0 && nowMs>g.prevFixMs){ double dtH=(nowMs-g.prevFixMs)/3600000.0;
      if(dtH>=(0.5/3600.0)){ double gd=distNmGC(g.prevLat,g.prevLon,lat,lon)/dtH;
        if(gd<0)gd=0; if(gd>1500.0)gd=1500.0; g.gsDerivedKt=gd; } } }
  g.lat=lat; g.lon=lon; g.altFt=alt;
  g.gsKt = (gs>1.0 && gs<=1500.0) ? gs : g.gsDerivedKt;   // prefer (sane) source GS, else derived
  if(g.gsKt<0 || g.gsKt>1500.0) g.gsKt=0;                 // final clamp
  if(utcMs) g.utcMs=utcMs;
  g.lastFixMs=nowMs; g.valid=true; g.simulated=sim; g.fixed=false;
  double vLat=g.lat,vLon=g.lon,vGs=g.gsKt,vGsD=g.gsDerivedKt,vTrk=g.trackDeg; bool vHt=g.haveTrack; uint32_t vPrev=g.prevFixMs;
  xSemaphoreGive(gMux);
  vlog(String("[FIX] ")+(sim?"SIM":"REAL")+" lat="+String(vLat,6)+" lon="+String(vLon,6)+" alt="+String(alt,0)+"ft"
       +" srcGS="+String(gs,1)+" derivedGS="+String(vGsD,1)+" usedGS="+String(vGs,1)+"kt"
       +" trk="+String(vTrk,1)+(vHt?"":"(none)")+" dtFix="+String(vPrev?(nowMs-vPrev):0)+"ms utcMs="+String((unsigned long long)g.utcMs));
}
static void pollerTask(void*){
  for(;;){
    if(cfg.simEnable){   // emulator override ON: discard the source feed entirely (do not poll/apply)
      strcpy(gPollMsg,"disabled (emulator on)"); gPollOk=false;
      vTaskDelay(1000/portTICK_PERIOD_MS); continue;
    }
    if(WiFi.status()==WL_CONNECTED){
      String body; uint32_t tp=millis();
      vlog(String("[POLL] GET ")+activeUrl());
      if(fetchFlightInfo(body)){
        vlogChunked("[POLL] body",body);
        double lat=0,lon=0,alt=0,gs=0,trk=0; bool okpos=false,haveTrk=false; uint64_t utc=0; char b1[24];
        if(cfg.srcType==1){   // ---- Panasonic flightdata format ----
          if(jsonStr(body,"td_id_fltdata_present_position_latitude",b1,sizeof b1)){ lat=panLL(b1);
            if(jsonStr(body,"td_id_fltdata_present_position_longitude",b1,sizeof b1)){ lon=panLL(b1); okpos=true; } }
          jsonNum(body,"td_id_fltdata_altitude",alt);
          jsonNum(body,"td_id_fltdata_ground_speed",gs);
          if(jsonNum(body,"td_id_fltdata_true_heading",trk)) haveTrk=true;
          xSemaphoreTake(gMux,portMAX_DELAY);
          jsonStr(body,"td_id_fltdata_flight_number",g.flight,sizeof g.flight);
          jsonStr(body,"td_id_airframe_tail_number",g.tail,sizeof g.tail);
          jsonStr(body,"td_id_fltdata_departure_id",g.orig,sizeof g.orig);
          jsonStr(body,"td_id_fltdata_destination_id",g.dest,sizeof g.dest);
          g.serviceAvail=true; xSemaphoreGive(gMux);
        } else {              // ---- Viasat format (also used for a custom/test URL) ----
          if(jsonNum(body,"latitude",lat)&&jsonNum(body,"longitude",lon)) okpos=true;
          jsonNum(body,"altitude",alt); jsonNum(body,"groundSpeed",gs);
          char ts[40]; if(jsonStr(body,"current_time",ts,sizeof ts)) utc=isoToEpochMs(ts);
          xSemaphoreTake(gMux,portMAX_DELAY);
          jsonStr(body,"flightNumber",g.flight,sizeof g.flight);
          jsonStr(body,"tail_number",g.tail,sizeof g.tail);
          jsonStr(body,"originCode",g.orig,sizeof g.orig);
          jsonStr(body,"destinationCode",g.dest,sizeof g.dest);
          double sa; g.serviceAvail = jsonNum(body,"service_available",sa)?(sa!=0):true;
          xSemaphoreGive(gMux);
        }
        if(okpos){
          applyFix(lat,lon,alt,gs,utc,false);
          if(haveTrk){ xSemaphoreTake(gMux,portMAX_DELAY); g.trackDeg=trk; g.haveTrack=true; xSemaphoreGive(gMux); }  // Panasonic gives true heading
          gPollOk=true; gPollAtMs=millis(); strcpy(gPollMsg,"ok");
          Serial.printf("[SRC] %.5f %.5f %.0fft %.0fkt\n",lat,lon,alt,gs);
          vlog(String("[POLL] ok in ")+String(millis()-tp)+"ms  src="+(cfg.srcType==1?"Panasonic":cfg.srcType==0?"Viasat":"custom")+" flt="+g.flight+" tail="+g.tail+" "+g.orig+"->"+g.dest);
        } else { gPollOk=false; gPollAtMs=millis(); strcpy(gPollMsg,"no lat/lon in json"); vlog("[POLL] FAIL: no lat/lon in json"); }
      } else { gPollOk=false; gPollAtMs=millis(); Serial.printf("[SRC] poll fail: %s\n",gPollMsg); }
    }
    vTaskDelay(cfg.pollMs/portTICK_PERIOD_MS);
  }
}
static void simStep(uint32_t dtMs){
  if(!cfg.simEnable) return;   // emulator OFF -> use the real (the source) feed
  // emulator ON: emit a FIXED test position (override). Position does NOT move; the source feed is
  // discarded in pollerTask. GS/track come straight from the configured sim fields (for display).
  uint32_t nowMs=millis();
  xSemaphoreTake(gMux,portMAX_DELAY);
  g.lat=cfg.simLat; g.lon=cfg.simLon; g.altFt=cfg.simAlt;
  g.gsKt = (cfg.simGs>=0 && cfg.simGs<=1500.0) ? cfg.simGs : 0;
  g.trackDeg=cfg.simTrk; g.haveTrack=true;
  g.fixed=true; g.valid=true; g.simulated=true;
  g.lastFixMs=nowMs;
  time_t t=time(nullptr); if(t>1700000000) g.utcMs=(uint64_t)t*1000ULL+(millis()%1000);
  xSemaphoreGive(gMux);
}

// ----------------------------- ADBP -----------------------------------------
// escape a value for an XML attribute (ACID/flight/dest/etc.) so & < > " can't corrupt the frame
static String xmlEsc(const char* v){ String o; for(const char*p=v;*p;++p){ char c=*p;
  if(c=='&')o+="&amp;"; else if(c=='<')o+="&lt;"; else if(c=='>')o+="&gt;"; else if(c=='"')o+="&quot;"; else o+=c; } return o; }
static void snapshot(PosState& s){ xSemaphoreTake(gMux,portMAX_DELAY); s=g; xSemaphoreGive(gMux); }
static uint32_t gLastFix(){ xSemaphoreTake(gMux,portMAX_DELAY); uint32_t v=g.lastFixMs; xSemaphoreGive(gMux); return v; }
// best current UTC in ms: SNTP if synced, else source fix time, else a recent floor (~2026-06)
static uint64_t stampMs(uint64_t srcUtc){
  time_t t=time(nullptr);
  if(t>1700000000) return (uint64_t)t*1000ULL + (millis()%1000);
  if(srcUtc>1000000000000ULL) return srcUtc;
  return 1782000000000ULL + (millis()%1000);
}
static bool paramXml(const String& name,const PosState& s,char* out,size_t cap){
  String u=name; u.toUpperCase();
  bool fresh = s.valid && (millis()-s.lastFixMs<cfg.staleMs);
  uint64_t ts=stampMs(s.utcMs);
  #define NCDP do{ snprintf(out,cap,"<parameter name=\"%s\" validity=\"2\"/>",name.c_str()); return true; }while(0)
  #define STRP(v) do{ String _e=xmlEsc(v); snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"7\" value=\"%s\" time=\"%llu\"/>",name.c_str(),_e.c_str(),(unsigned long long)ts); return true; }while(0)
  // GPS FINE lat/lon: the EFB reconstructs position from COARSE+FINE and REJECTS the fix unless the
  // fine sample's status MATCHES the coarse. Proven by the EFB's own the EFB connectivity log:
  //   "GetPositionGPS: ... Coarse and Fine ... Status Mismatch ... NORMAL vs NO_COMPUTED_DATA".
  // Coarse (GPSLATP/GPSLONGP) already carries full precision, so emit fine as a VALID 0.0 residual
  // -> coarse + 0 = correct position, and both statuses are NORMAL.
  if(u=="GPSLATPF"||u=="GPSLONGPF"||u.indexOf("FINE")>=0){
    snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"0\" value=\"0.000000\" time=\"%llu\"/>",name.c_str(),(unsigned long long)ts); return true; }
  // ===== ADBP (ADBP generic) vocabulary — assert a complete, trustworthy GNSS fix =====
  #define F64P(v) do{ snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"0\" value=\"%s\" time=\"%llu\"/>",name.c_str(),(v),(unsigned long long)ts); return true; }while(0)
  #define S32P(v) do{ snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"5\" value=\"%d\" time=\"%llu\"/>",name.c_str(),(int)(v),(unsigned long long)ts); return true; }while(0)
  #define BOOLP(v) do{ snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"6\" value=\"%d\" time=\"%llu\"/>",name.c_str(),(int)(v),(unsigned long long)ts); return true; }while(0)
  if(u=="GNSS_AVAIL"||u.indexOf("AVAIL")>=0) BOOLP(fresh?1:0);                 // GNSS available flag (key gate)
  if(u=="GNSS_HFOM"||u=="GNSS_VFOM"||u.indexOf("FOM")>=0) F64P("8.0");          // figure of merit (m), small = good
  if(u=="HORIZONTALDILUTIONOFPRECISION"||u=="GNSS_HDOP"||u=="GNSS_VDOP"||u.indexOf("DILUTION")>=0) F64P("0.8");
  if(u.indexOf("INTEGRITYLIMIT")>=0||u=="GPSHIL2") F64P("0.10");                // HPL/HIL within limit
  { time_t tt=ts/1000; struct tm tm; gmtime_r(&tt,&tm);
    if(u=="GNSS_HOURS"||u=="TIMEGMT-HOUR") S32P(tm.tm_hour);
    if(u=="GNSS_MINUTES"||u=="TIMEGMT-MINUTE") S32P(tm.tm_min);
    if(u=="GNSS_SECONDS"||u=="TIMEGMT-SECOND") S32P(tm.tm_sec);
    if(u=="TIMEGMT-"||u=="TIMEGMT"){ char b[16]; snprintf(b,sizeof b,"%02d:%02d:%02d",tm.tm_hour,tm.tm_min,tm.tm_sec);
      snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"9\" value=\"%s\" time=\"%llu\"/>",name.c_str(),b,(unsigned long long)ts); return true; }
    if(u=="DATE-DAY") S32P(tm.tm_mday);
    if(u=="DATE-MONTH") S32P(tm.tm_mon+1);
    if(u=="DATE-YEAR") S32P(tm.tm_year+1900);
    if(u=="DATE-"||u=="DATE"){ char b[16]; snprintf(b,sizeof b,"%04d/%02d/%02d",tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday);
      snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"8\" value=\"%s\" time=\"%llu\"/>",name.c_str(),b,(unsigned long long)ts); return true; }
  }
  if(u.indexOf("FLIGHTNUMBER-")>=0){                                            // split flight number into BCD digits
    int fn=0; for(const char* p=s.flight;*p;++p) if(*p>='0'&&*p<='9') fn=fn*10+(*p-'0');
    if(!s.flight[0]) NCDP;
    if(u.indexOf("ONES")>=0) S32P(fn%10); if(u.indexOf("TENS")>=0) S32P((fn/10)%10);
    if(u.indexOf("HUNDREDS")>=0) S32P((fn/100)%10); if(u.indexOf("THOUSANDS")>=0) S32P((fn/1000)%10);
    NCDP; }
  if(u=="AIRCRAFTTYPE-"||u=="AIRCRAFTTYPE"){ if(cfg.acType.length()) STRP(cfg.acType.c_str()); NCDP; }
  #undef F64P
  #undef S32P
  #undef BOOLP
  // identification strings (TYPE_STRING=7)
  if(u=="ACID"||u.indexOf("TAIL")>=0||u.indexOf("REGIST")>=0||u.indexOf("ACREG")>=0){
    if(s.tail[0]) STRP(s.tail); else STRP(cfg.acTail.c_str()); }   // prefer live tail, else configured
  // City Pair (AID "FROMTO"): departure+arrival combined — the basic param list has no separate DEPARTURE
  if(u=="FROMTO"||u.indexOf("CITYPAIR")>=0||u.indexOf("CITY_PAIR")>=0){
    if(s.orig[0]&&s.dest[0]){ char cp[20]; snprintf(cp,sizeof cp,"%s%s",s.orig,s.dest); STRP(cp); } else NCDP; }
  if(u=="FLT"||u.indexOf("FLTNUM")>=0||u.indexOf("FLIGHT")>=0){ if(s.flight[0]) STRP(s.flight); else NCDP; }
  if(u=="DEST"||u=="ADES"||u=="ARR"||u.indexOf("DESTIN")>=0||u.indexOf("ARRIVAL")>=0){ if(s.dest[0]) STRP(s.dest); else NCDP; }
  if(u=="ORIG"||u=="ADEP"||u=="DEP"||u.indexOf("ORIGIN")>=0||u.indexOf("DEPART")>=0){ if(s.orig[0]) STRP(s.orig); else NCDP; }
  // ACTYP is TYPE_S32B (numeric code), not a string -> NCD for now (per spec correction)
  if(u=="ACTYP") NCDP;
  // discretes (TYPE_BOOL=6): airborne, brake off, doors closed
  if(u=="WOW"||u.indexOf("WHEEL")>=0||u.indexOf("WONW")>=0||u=="PBRKON"||u.indexOf("DOOR")>=0){
    snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"6\" value=\"0\" time=\"%llu\"/>",name.c_str(),(unsigned long long)ts); return true; }
  // GPS quality/integrity — assert good so the EFB trusts the position (TYPE_F64B=0)
  if(u=="GPSHDOP"||u=="GPSVDOP"){
    snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"0\" value=\"0.8\" time=\"%llu\"/>",name.c_str(),(unsigned long long)ts); return true; }
  if(u=="GPSHIL"){
    snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"0\" value=\"0.10\" time=\"%llu\"/>",name.c_str(),(unsigned long long)ts); return true; }
  // date/time (TYPE_DATE=8 YYYY/MM/DD, TYPE_TIME=9 HH:MM:SS)
  if(u=="SYSTEMTIME"){ time_t t=ts/1000; struct tm tm; gmtime_r(&t,&tm); char b[16]; snprintf(b,sizeof b,"%02d:%02d:%02d",tm.tm_hour,tm.tm_min,tm.tm_sec);
    snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"9\" value=\"%s\" time=\"%llu\"/>",name.c_str(),b,(unsigned long long)ts); return true; }
  if(u=="SYSTEMDATE"){ time_t t=ts/1000; struct tm tm; gmtime_r(&t,&tm); char b[16]; snprintf(b,sizeof b,"%04d/%02d/%02d",tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday);
    snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"8\" value=\"%s\" time=\"%llu\"/>",name.c_str(),b,(unsigned long long)ts); return true; }
  // true heading (the review: the EFB subscribes THDG; emit as TYPE_F32B=3, use GPS track as proxy)
  if(u=="THDG"||u.indexOf("HDG")>=0||u.indexOf("HEADING")>=0){
    if(fresh&&s.haveTrack) snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"3\" value=\"%.2f\" time=\"%llu\"/>",name.c_str(),norm180(s.trackDeg),(unsigned long long)ts);
    else NCDP; return true; }
  // numeric position (TYPE_F64B=0). GS is the (clamped) state GS -> it always agrees with the motion the
  // dead-reckoner produces; no magic fallback value that could disagree with the position.
  bool haveVal=fresh; char vb[40];
  double gsVal = (s.gsKt>=0 && s.gsKt<=1500.0) ? s.gsKt : 0.0;
  double altVal = (s.altFt>1.0) ? s.altFt : 31000.0;                    // stable altitude if source has none
  if(u.indexOf("LAT")>=0) snprintf(vb,sizeof vb,"%.6f",s.lat);
  else if(u.indexOf("LON")>=0) snprintf(vb,sizeof vb,"%.6f",s.lon);
  else if(u=="GPSTTRKA"||u.indexOf("TRK")>=0||u.indexOf("TRACK")>=0){ snprintf(vb,sizeof vb,"%.2f",norm180(s.trackDeg)); haveVal=fresh&&s.haveTrack; }
  else if(u=="GS"||u.indexOf("GROUND")>=0) snprintf(vb,sizeof vb,"%.1f",gsVal);
  else if(u=="BARO_CORRECTION_ALTITUDE1"||(u.indexOf("ALT")>=0 && u.indexOf("CORRECTION")<0)) snprintf(vb,sizeof vb,"%.0f",altVal);
  else NCDP;
  if(haveVal) snprintf(out,cap,"<parameter name=\"%s\" validity=\"1\" type=\"0\" value=\"%s\" time=\"%llu\"/>",name.c_str(),vb,(unsigned long long)ts);
  else NCDP;
  return true;
  #undef STRP
  #undef NCDP
}
static int parseParams(const String& m,String names[],int maxN){
  // match ONLY <parameter name="..."> — must not catch the <method name="..."> tag
  int n=0,i=0; const char* tag="<parameter name=\""; int tl=strlen(tag);
  while(n<maxN){ i=m.indexOf(tag,i); if(i<0)break; i+=tl; int e=m.indexOf('"',i); if(e<0)break;
    names[n++]=m.substring(i,e); i=e+1; } return n; }
static String paramsBlock(const String names[],int n,bool& miss){
  PosState s; snapshot(s);
  // dead-reckon position from the last fix to NOW so motion is smooth at the 1 Hz push rate
  // (the source only updates every ~10 s; without this the EFB sees teleporting kinematics).
  if(s.valid && !s.fixed && s.haveTrack && s.gsKt>1.0 && s.gsKt<=1500.0){
    double dtSec=(millis()-s.lastFixMs)/1000.0, cap=cfg.staleMs/1000.0;
    if(dtSec<0)dtSec=0; if(dtSec>cap)dtSec=cap;
    double dnm=s.gsKt*dtSec/3600.0;
    if(dnm>0){ double la,lo; advanceLL(s.lat,s.lon,s.trackDeg,dnm,la,lo); s.lat=la; s.lon=lo; }
  }
  String body="<parameters>"; body.reserve(2700); char pb[200]; miss=false;
  for(int i=0;i<n;i++){ if(!paramXml(names[i],s,pb,sizeof pb)) miss=true; body+=pb; }
  return body+"</parameters>"; }
static String wrapResp(const char* m,int ec,const String& pb){
  return String("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<response method=\"")+m+"\" errorcode=\""+ec+"\">"+pb+"</response>"; }
static String wrapPush(const char* m,const String& pb,bool withProlog){
  // Framing is configurable (debug): cfg.frameLen 0=full(prolog+method) 1=method-element 2=omit length attr.
  String prolog = withProlog ? "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" : "";
  if(cfg.frameLen==2) return prolog+"<method name=\""+m+"\">"+pb+"</method>";   // no length attribute
  int len=0; String msg; msg.reserve(pb.length()+128); String methodEl; methodEl.reserve(pb.length()+96);
  while(true){
    methodEl=String("<method name=\"")+m+"\" length=\""+len+"\">"+pb+"</method>";
    msg=prolog+methodEl;
    int nx=(cfg.frameLen==1)?methodEl.length():msg.length();   // 1=method-only count, 0=full (incl prolog)
    if(nx==len) break; len=nx;
  }
  return msg; }
static long tagNum(const String& m,const char* t){ String o=String("<")+t+">"; int i=m.indexOf(o); if(i<0)return -1; i+=o.length();
  int e=m.indexOf('<',i); if(e<0)return -1; return m.substring(i,e).toInt(); }
static void handleAdbp(WiFiClient& cl){
  String msg; uint32_t t0=millis(); uint32_t tStart=t0;
  // read until </method>; cap total wait at 250ms and bail fast if the peer sends nothing
  while(millis()-t0<250){ while(cl.available()){ msg+=(char)cl.read(); t0=millis(); } if(msg.indexOf("</method>")>=0)break;
    if(!msg.length() && millis()-tStart>60) break; delay(2); }
  { String pv=msg; pv.replace("\r"," "); pv.replace("\n"," ");
    if(pv.length()>1500) pv=pv.substring(0,1500)+"...";
    logln("ADBP "+cl.remoteIP().toString()+":"+String(cl.remotePort())+" "+String(msg.length())+"B: "+(msg.length()?pv:String("(connected, no data)"))); }
  vlogChunked("ADBP RX full",msg);
  if(!msg.length()){ cl.stop(); return; }
  String names[64]; int n=parseParams(msg,names,64);
  String resp; const char* txName="(none)";
  if(msg.indexOf("\"getAvionicParameters\"")>=0){ bool m; String pb=paramsBlock(names,n,m);
    txName=m?"getAvionicParametersResponse(err8)":"getAvionicParametersResponse"; resp=wrapResp("getAvionicParametersResponse",m?8:0,pb); }
  else if(msg.indexOf("\"subscribeAvionicParameters")>=0){
    bool onEvent=msg.indexOf("OnEvent")>=0; long pport=tagNum(msg,"publishport"); long per=tagNum(msg,"refreshperiod"); if(per<100)per=5000;
    bool m; String pb=paramsBlock(names,n,m);
    const char* rm=onEvent?"subscribeAvionicParametersOnEventResponse":"subscribeAvionicParametersResponse";
    // PER-CLIENT single subscription: drop only THIS iPad's previous sub(s) (kills stale-schema
    // interleaving from the same client) but leave OTHER iPads' subscriptions intact -> concurrent clients.
    for(int k=0;k<MAX_SUBS;k++) if(subs[k].active && subs[k].ip==cl.remoteIP()){ subs[k].active=false; subs[k].wasConnected=false; pushCli[k].stop(); }
    if(pport>0){ int idx=-1; for(int k=0;k<MAX_SUBS;k++) if(!subs[k].active){ idx=k; break; }
      if(idx>=0){ subs[idx]={}; subs[idx].active=true; subs[idx].ip=cl.remoteIP(); subs[idx].port=(uint16_t)pport;
        subs[idx].onEvent=onEvent; subs[idx].periodMs=per;
        String j; for(int i=0;i<n;i++){ j+=names[i]; j+=' '; } j.toCharArray(subs[idx].params,sizeof subs[idx].params);
        int act=0; for(int k=0;k<MAX_SUBS;k++) if(subs[k].active)act++;
        logln("ADBP RX subscribe "+cl.remoteIP().toString()+":"+String(pport)+" slot"+String(idx)+" "+(onEvent?"onEvent":"cont")+" period="+String(per)+"ms "+String(n)+"p ("+String(act)+" client(s) active)"); }
      else logln("ADBP subscribe REJECTED: no free slot (max "+String(MAX_SUBS)+" clients)"); }
    txName=rm; resp=wrapResp(rm,0,pb);   // always accept (per-param validity carries NF, no top-level reject)
  }
  else if(msg.indexOf("\"unSubscribe\"")>=0){ long p=tagNum(msg,"publishport");
    // match IP+port: both iPads use publishport 51001, so unsub from one must NOT drop the other
    for(int i=0;i<MAX_SUBS;i++) if(subs[i].active && subs[i].ip==cl.remoteIP() && subs[i].port==p){
      logln("UNSUB "+cl.remoteIP().toString()+":"+String(p)+" after stream "+String((millis()-subs[i].connMs)/1000.0,1)+"s / "+String(subs[i].pushCount)+" msgs sent");
      subs[i].active=false; subs[i].wasConnected=false; pushCli[i].stop(); }
    txName="unSubscribeResponse"; resp=wrapResp("unSubscribeResponse",0,"<parameters></parameters>"); }
  else { txName="UnknownMethod(err2)"; resp=wrapResp("UnknownMethod",2,"<parameters></parameters>"); }
  cl.print(resp); cl.flush(); delay(5); cl.stop();
  logln(String("ADBP TX ")+txName+" "+String(resp.length())+"B -> closed cmd socket 24000");
  vlogChunked("ADBP TX full",resp);
}
static void pushSubs(){
  uint32_t curFix=gLastFix();   // one locked read of the latest fix time for this cycle
  for(int i=0;i<MAX_SUBS;i++){ if(!subs[i].active)continue; uint32_t now=millis(); bool due=false;
    if(subs[i].onEvent){ if(curFix!=subs[i].lastFixSeen) due=true; }
    else if(now-subs[i].lastPushMs>=subs[i].periodMs) due=true;
    if(!due)continue;
    // (re)establish the ONE persistent data-stream connection (AID keeps it open
    // and streams every periodic message over it — opening a new socket per push
    // is what the EFB rejected after the first).
    // detect the client tearing down the data-stream socket (vs. an explicit unSubscribe)
    if(subs[i].wasConnected && !pushCli[i].connected()){
      logln("PUSH stream CLOSED BY CLIENT "+subs[i].ip.toString()+":"+String(subs[i].workPort)+
            " after "+String((now-subs[i].connMs)/1000.0,1)+"s / "+String(subs[i].pushCount)+" msgs"
            "  <- the EFB dropped us (content rejected)");
      subs[i].wasConnected=false; subs[i].workPort=0;
    }
    if(!pushCli[i].connected()){
      pushCli[i].stop();
      // connect straight to the client's ADVERTISED publishport (no 51000 probe — that just wasted a
      // 250ms blocking attempt every cycle and starved the other iPad's pushes).
      uint16_t port = subs[i].port ? subs[i].port : (uint16_t)cfg.dsPort;
      if(pushCli[i].connect(subs[i].ip,port,200)){ subs[i].workPort=port; subs[i].dumped=false; pushCli[i].setNoDelay(true);
        subs[i].connMs=now; subs[i].pushCount=0; subs[i].wasConnected=true;
        logln("PUSH stream connected "+subs[i].ip.toString()+":"+String(port)); }
      else { subs[i].workPort=0;
        logln("PUSH connect fail "+subs[i].ip.toString()+":"+String(port)+" (client not listening)");
        subs[i].lastPushMs=now; continue; }   // retry no more than once per period (1s), 200ms timeout
    }
    // anything the EFB sends back on the data socket is a strong clue (ack / nak / error / keep-alive)
    if(pushCli[i].available()){ String rx; while(pushCli[i].available()&&rx.length()<400) rx+=(char)pushCli[i].read();
      rx.replace("\r"," "); rx.replace("\n"," "); logln("PUSH socket RX ("+String(rx.length())+"B) <<< "+rx); }
    String list=subs[i].params,nm[64]; int n=0,si=0;
    while(n<64){ int sp=list.indexOf(' ',si); if(sp<0)break; String t=list.substring(si,sp); si=sp+1; if(t.length())nm[n++]=t; }
    bool m; String pb=paramsBlock(nm,n,m);
    const char* method=subs[i].onEvent?"onEventAvionicParameters":"publishAvionicParameters";
    bool withProlog = cfg.framePrologEach || subs[i].pushCount==0;   // optionally prolog only on first frame
    String msg=wrapPush(method,pb,withProlog);
    if(!subs[i].dumped){ String d=msg; d.replace("\r"," ");   // full first push, split across log lines so nothing is lost
      const char* dn=cfg.frameDelim==1?"CRLF":cfg.frameDelim==2?"LF":cfg.frameDelim==3?"NUL":"none";
      logln("PUSH MSG ("+String(msg.length())+"B, "+String(n)+" params) frameLen="+String(cfg.frameLen)+" delim="+dn+" prolog="+(withProlog?"yes":"no")+":");
      for(int o=0,part=1;o<(int)d.length();o+=480,part++) logln("  ["+String(part)+"] "+d.substring(o,o+480));
      subs[i].dumped=true; }
    size_t w=pushCli[i].print(msg);
    if(cfg.frameDelim==1) pushCli[i].print("\r\n"); else if(cfg.frameDelim==2) pushCli[i].print("\n");
    else if(cfg.frameDelim==3) pushCli[i].write((uint8_t)0);   // NUL delimiter
    pushCli[i].flush();
    if(w==0){ logln("PUSH write fail -> reconnect next cycle"); pushCli[i].stop(); subs[i].workPort=0; subs[i].wasConnected=false; }
    else { subs[i].pushCount++; PosState ps; snapshot(ps); bool fr=ps.valid&&(millis()-ps.lastFixMs<cfg.staleMs);
      logln(String("PUSH ")+subs[i].ip.toString()+":"+subs[i].workPort+" #"+String(subs[i].pushCount)+" "+String(n)+"p "+String(w)+"B len="+String(cfg.frameLen)+" ok  pos="+
            (fr?(String(ps.lat,6)+","+String(ps.lon,6)+" gs="+String(ps.gsKt,1)+" trk="+String(ps.trackDeg,1)+" alt="+String(ps.altFt,0)+(ps.simulated?" SIM":" LIVE")):String("NCD - no fresh feed!")));
      vlogChunked(String("PUSH #")+subs[i].pushCount+" full frame (delim="+(cfg.frameDelim==1?"CRLF":cfg.frameDelim==2?"LF":cfg.frameDelim==3?"NUL":"none")+")",msg); }
    subs[i].lastPushMs=now; subs[i].lastFixSeen=curFix;
  }
}

// ----------------------------- WEB UI ---------------------------------------
static const char* CSS = R"CSS(
:root{--bg:#070b14;--bg2:#0d1322;--card:#111a2e;--line:#1e2a44;--txt:#eaf2ff;--mut:#8aa0c0;--cy:#22d3ee;--cy2:#38bdf8;--am:#fbbf24;--gr:#34d399;--rd:#f87171}
*{box-sizing:border-box;margin:0;padding:0}
body{background:radial-gradient(1200px 600px at 80% -10%,rgba(34,211,238,.12),transparent),linear-gradient(180deg,#070b14,#0a1020);color:var(--txt);font-family:-apple-system,'SF Pro Display',Segoe UI,system-ui,sans-serif;line-height:1.5;min-height:100vh}
.wrap{max-width:920px;margin:0 auto;padding:clamp(1rem,3vw,2.2rem)}
.hero{display:flex;align-items:center;gap:1rem;padding:1.3rem 1.5rem;border:1px solid var(--line);border-radius:18px;background:linear-gradient(135deg,rgba(34,211,238,.10),rgba(56,189,248,.03));margin-bottom:1.3rem;position:relative;overflow:hidden}
.hero::after{content:"";position:absolute;right:-40px;top:-40px;width:180px;height:180px;background:radial-gradient(circle,rgba(34,211,238,.25),transparent 70%);filter:blur(8px)}
.logo{font-weight:800;font-size:1.6rem;letter-spacing:.14em;background:linear-gradient(135deg,#fff,#22d3ee);-webkit-background-clip:text;background-clip:text;-webkit-text-fill-color:transparent}
.hero .sub{color:var(--mut);font-size:.82rem;letter-spacing:.05em}
.badge{margin-left:auto;font-size:.7rem;font-weight:700;text-transform:uppercase;letter-spacing:.1em;padding:.3rem .7rem;border-radius:100px;border:1px solid rgba(34,211,238,.4);color:var(--cy);background:rgba(34,211,238,.08);z-index:1}
.status{display:grid;grid-template-columns:repeat(auto-fit,minmax(135px,1fr));gap:.6rem;margin-bottom:1.3rem}
.s{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:.7rem .9rem}
.s .k{font-size:.62rem;text-transform:uppercase;letter-spacing:.12em;color:var(--mut)}
.s .v{font-family:'SF Mono',ui-monospace,monospace;font-size:1.05rem;font-weight:700;margin-top:.15rem}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:.35rem;vertical-align:middle}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:1.2rem 1.3rem;margin-bottom:1.1rem}
.card h2{font-size:.82rem;text-transform:uppercase;letter-spacing:.12em;color:var(--cy);margin-bottom:.9rem;display:flex;align-items:center;gap:.5rem;border-bottom:1px solid var(--line);padding-bottom:.6rem}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:.8rem 1.1rem}
@media(max-width:620px){.grid{grid-template-columns:1fr}}
.f{display:flex;flex-direction:column;gap:.3rem}
.f.full{grid-column:1/-1}
label{font-size:.72rem;color:var(--mut);letter-spacing:.04em}
input[type=text],input[type=password],input[type=number]{background:#070d1a;border:1px solid var(--line);border-radius:9px;color:var(--txt);padding:.6rem .7rem;font-family:'SF Mono',ui-monospace,monospace;font-size:.9rem;width:100%;transition:.15s}
input:focus{outline:none;border-color:var(--cy);box-shadow:0 0 0 3px rgba(34,211,238,.15)}
input:disabled{opacity:.5;cursor:not-allowed;background:#0a1120;border-style:dashed}
select{appearance:none;-webkit-appearance:none;background:#070d1a url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%2322d3ee' stroke-width='3' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M6 9l6 6 6-6'/%3E%3C/svg%3E") no-repeat right .8rem center;border:1px solid var(--line);border-radius:9px;color:var(--txt);padding:.6rem 2.1rem .6rem .7rem;font-family:'SF Mono',ui-monospace,monospace;font-size:.9rem;width:100%;cursor:pointer;transition:.15s}
select:hover{border-color:var(--cy2)}
select:focus{outline:none;border-color:var(--cy);box-shadow:0 0 0 3px rgba(34,211,238,.15)}
select option{background:#0d1322;color:var(--txt)}
.tog{display:flex;align-items:center;gap:.6rem;background:#070d1a;border:1px solid var(--line);border-radius:9px;padding:.55rem .7rem;cursor:pointer}
.tog input{appearance:none;width:38px;height:22px;background:var(--line);border-radius:100px;position:relative;cursor:pointer;transition:.2s;flex:0 0 auto}
.tog input:checked{background:var(--cy)}
.tog input::after{content:"";position:absolute;width:16px;height:16px;border-radius:50%;background:#fff;top:3px;left:3px;transition:.2s}
.tog input:checked::after{left:19px}
.tog span{font-size:.82rem}
.bar{display:flex;gap:.7rem;align-items:center;position:sticky;bottom:0;padding:1rem 0}
button{background:linear-gradient(135deg,var(--cy),var(--cy2));color:#04121c;border:none;font-weight:800;letter-spacing:.04em;padding:.8rem 1.6rem;border-radius:11px;cursor:pointer;font-size:.95rem;box-shadow:0 6px 20px rgba(34,211,238,.25)}
button.ghost{background:transparent;color:var(--mut);border:1px solid var(--line);box-shadow:none}
.note{color:var(--mut);font-size:.74rem;margin-top:.3rem}
.warn{font-size:.72rem;color:var(--am);background:rgba(251,191,36,.07);border:1px solid rgba(251,191,36,.25);border-radius:10px;padding:.6rem .8rem;margin-bottom:1.1rem}
footer{color:#4a5b78;font-size:.72rem;text-align:center;padding:1.4rem 0}
.ctbl{width:100%;border-collapse:collapse;font-size:.85rem}
.ctbl th{text-align:left;color:var(--mut);font-size:.66rem;text-transform:uppercase;letter-spacing:.1em;padding:.45rem .5rem;border-bottom:1px solid var(--line)}
.ctbl td{padding:.5rem;border-bottom:1px solid rgba(30,42,68,.5);font-family:'SF Mono',ui-monospace,monospace}
.ctbl tr:hover td{background:rgba(34,211,238,.05)}
.rssi{display:inline-block;min-width:42px}
.empty{color:var(--mut);font-style:italic;padding:.6rem .5rem}
code{background:#070d1a;border:1px solid var(--line);border-radius:5px;padding:.05rem .35rem;font-size:.82em}
)CSS";

static String esc(const String& s){ String o; for(char c:s){ if(c=='"')o+="&quot;"; else if(c=='<')o+="&lt;"; else if(c=='>')o+="&gt;"; else if(c=='&')o+="&amp;"; else o+=c; } return o; }
static const char* selo(bool b){ return b?" selected":""; }
static String fText(const char*lbl,const char*nm,const String&v,const char*type="text",bool full=false){
  return String("<div class='f")+(full?" full":"")+"'><label>"+lbl+"</label><input type='"+type+"' name='"+nm+"' value='"+esc(v)+"'></div>"; }
static String fNum(const char*lbl,const char*nm,double v,const char*step="any"){
  char b[40]; snprintf(b,sizeof b,"%g",v); return String("<div class='f'><label>")+lbl+"</label><input type='number' step='"+step+"' name='"+nm+"' value='"+b+"'></div>"; }
static String fTog(const char*lbl,const char*nm,bool v){
  return String("<div class='f'><label>&nbsp;</label><label class='tog'><input type='checkbox' name='")+nm+"' "+(v?"checked":"")+"><span>"+lbl+"</span></label></div>"; }

// ----------------------------- AUTH -----------------------------------------
static String sha256hex(const String& in){
  uint8_t h[32]; mbedtls_sha256((const uint8_t*)in.c_str(), in.length(), h, 0);
  char o[65]; for(int i=0;i<32;i++) sprintf(o+i*2,"%02x",h[i]); o[64]=0; return String(o); }
static String randHex(int n){ String s; for(int i=0;i<n;i++){ char b[3]; sprintf(b,"%02x",(uint8_t)(esp_random()&0xff)); s+=b; } return s; }
static String gSessTok=""; static uint32_t gSessSeen=0; static bool gSessRemember=false;
static const uint32_t SESS_IDLE=30UL*60UL*1000UL;   // 30 min idle for non-remembered sessions
static String cookieVal(const char* key){
  if(!web.hasHeader("Cookie")) return ""; String c=web.header("Cookie");
  String k=String(key)+"="; int i=c.indexOf(k); if(i<0)return ""; i+=k.length();
  int e=c.indexOf(';',i); if(e<0)e=c.length(); String v=c.substring(i,e); v.trim(); return v; }
static bool authed(){
  if(!cfg.authEnable || cfg.authHash.length()==0) return true;   // auth off / not configured -> open
  String t=cookieVal("AIDSESS");
  if(t.length() && gSessTok.length() && t==gSessTok && (gSessRemember || millis()-gSessSeen<SESS_IDLE)){
    gSessSeen=millis(); return true; }
  return false; }
static bool requireAuth(){ if(authed()) return true; web.sendHeader("Location","/login"); web.send(302,"text/plain","login required"); return false; }

static void handleRoot(){
  if(!requireAuth())return;
  String h; h.reserve(14000);   // page is ~10-12 KB; reserve once to avoid repeated realloc/fragmentation
  h="<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  h+="<title>"+esc(cfg.devName)+" — Config</title><style>"+String(CSS)+"</style></head><body><div class='wrap'>";
  h+="<div class='hero'><div><div class='logo'><span style='-webkit-text-fill-color:var(--cy);color:var(--cy)'>AID</span><span style='-webkit-text-fill-color:var(--gr);color:var(--gr)'>link</span></div><div class='sub'>ARINC 834 ownship bridge · "+esc(cfg.devName)+" · <b>"+FW_BUILD+"</b>"+((cfg.authEnable&&cfg.authHash.length())?" · <a href='/logout' style='color:var(--cy)'>log out</a>":"")+"</div></div><span class='badge'>"+WiFi.softAPIP().toString()+"</span></div>";
  h+="<div class='status' id='st'><div class='s'><div class='k'>Uplink</div><div class='v' id='sta'>…</div></div>"
     "<div class='s'><div class='k'>AP clients</div><div class='v' id='cli'>…</div></div>"
     "<div class='s'><div class='k'>Position</div><div class='v' id='pos'>…</div></div>"
     "<div class='s'><div class='k'>Track / GS</div><div class='v' id='trk'>…</div></div>"
     "<div class='s'><div class='k'>Source</div><div class='v' id='src'>…</div></div>"
     "<div class='s'><div class='k'>Uplink IP</div><div class='v' id='staip' style='font-size:.85rem'>…</div></div>"
     "<div class='s'><div class='k'>Feed</div><div class='v' id='feed' style='font-size:.85rem'>…</div></div>"
     "<div class='s'><div class='k'>Firmware</div><div class='v' style='font-size:.8rem' title='version · date & time flashed'>"+esc(FW_BUILD)+"</div></div></div>";
  h+="<div class='card'><h2>🖧 Connected clients <span id='clic' class='text-muted' style='font-weight:400;font-size:.7rem'></span></h2>"
     "<div style='overflow-x:auto'><table class='ctbl' id='clit'><thead><tr><th>#</th><th>MAC</th><th>IP</th><th>Signal</th></tr></thead>"
     "<tbody><tr><td colspan='4' class='empty'>scanning…</td></tr></tbody></table></div></div>";

  h+="<div class='warn'>⚠ Experimental, non-certified. Ownship position only — never use as a primary/backup navigation source. Saving reboots the device to apply.</div>";
  h+="<form method='POST' action='/save'>";

  bool dh=cfg.staDhcp, up=(WiFi.status()==WL_CONNECTED);
  String ipv = dh ? (up?WiFi.localIP().toString():cfg.staIp)    : cfg.staIp;
  String gwv = dh ? (up?WiFi.gatewayIP().toString():cfg.staGw)  : cfg.staGw;
  String mkv = dh ? (up?WiFi.subnetMask().toString():cfg.staMask): cfg.staMask;
  String dnv = dh ? (up?WiFi.dnsIP().toString():cfg.staDns)     : cfg.staDns;
  String dis = dh ? " disabled" : "";
  h+="<div class='card'><h2>① Uplink Wi-Fi (we connect to)</h2><div class='grid'>";
  h+="<div class='f full'><label>Scan &amp; select a network</label><div style='display:flex;gap:.5rem'>"
     "<select id='scanList' style='flex:1' onchange=\"if(this.value){var s=document.querySelector('input[name=staSsid]');s.value=this.value;var p=document.querySelector('input[name=staPass]');if(p){p.value='';p.focus();}}\"><option value=''>— press Scan —</option></select>"
     "<button type='button' class='ghost' onclick='doScan(this)'>Scan</button></div></div>";
  h+=fText("SSID","staSsid",cfg.staSsid)+fText("Password","staPass",cfg.staPass,"password")+fTog("Use DHCP","staDhcp",cfg.staDhcp);
  h+="<div class='f'><label>Static IP</label><input id='staIp' type='text' name='staIp' value='"+esc(ipv)+"'"+dis+"></div>";
  h+="<div class='f'><label>Gateway</label><input id='staGw' type='text' name='staGw' value='"+esc(gwv)+"'"+dis+"></div>";
  h+="<div class='f'><label>Netmask</label><input id='staMask' type='text' name='staMask' value='"+esc(mkv)+"'"+dis+"></div>";
  h+="<div class='f'><label>DNS</label><input id='staDns' type='text' name='staDns' value='"+esc(dnv)+"'"+dis+"></div>";
  h+="</div><div class='note'>With <b>Use DHCP</b> on, these show the live DHCP-assigned values (read-only). Uncheck to edit them as a static config. Aircraft: <b>OpenCabinWiFi</b> (open — blank password), DHCP on.</div></div>";

  h+="<div class='card'><h2>② Cockpit AP (we feed / EFB joins)</h2><div class='grid'>";
  h+=fText("SSID","apSsid",cfg.apSsid)+fText("Password (≥8)","apPass",cfg.apPass,"password")+fTog("Hidden SSID","apHidden",cfg.apHidden)+"</div>";
  h+="<div class='note'>For lab compatibility testing, set the AP SSID (and Hidden flag) that your EFB client expects.</div></div>";

  h+="<div class='card'><h2>🆔 Aircraft identity</h2><div class='grid'>";
  h+=fText("Aircraft tail / registration","acTail",cfg.acTail)+fText("Aircraft type","acType",cfg.acType)+fText("Web API version","apiVer",cfg.apiVer)+"</div>";
  h+="<div class='note'><code>getAPIVersion</code> → "+esc(cfg.apiVer)+" (AID = 2.0, AID = 3.1). Tail/type are answered over ADBP for params containing TAIL/REG or ACTYPE/MODEL.</div></div>";

  h+="<div class='card'><h2>③ Network · DHCP · AP radio</h2><div class='grid'>";
  h+=fText("AP / AID IP","apIp",cfg.apIp)+fText("Netmask","apMask",cfg.apMask)
    +fText("DHCP pool start","apLease",cfg.apLease)+fNum("DHCP pool size","apDhcpCount",cfg.apDhcpCount,"1")
    +fNum("Lease time (min)","apLeaseMin",cfg.apLeaseMin,"1")+fText("Client DNS (blank=uplink)","apClientDns",cfg.apClientDns)
    +fNum("AP channel (0=follow STA)","apChannel",cfg.apChannel,"1")+fNum("Max clients (≤10)","apMaxClients",cfg.apMaxClients,"1")
    +fNum("ADBP TCP port","adbpPort",cfg.adbpPort,"1")+fNum("EFB DataStreamPort","dsPort",cfg.dsPort,"1")
    +fTog("NAT to uplink","napt",cfg.naptEnable)+fText("Device name","devName",cfg.devName)+"</div>";
  h+="<div class='note'>Default AP = 172.20.1.1 /26, ADBP on 24000 · keep the DHCP pool inside .2–.62 for /26.</div></div>";

  h+="<div class='card'><h2>④ Position source</h2><div class='grid'>";
  h+=String("<div class='f full'><label>Provider</label><select id='srcType' name='srcType' onchange='srcSel()'>")
    +"<option value='0'"+selo(cfg.srcType==0)+">Viasat</option>"
    +"<option value='1'"+selo(cfg.srcType==1)+">Panasonic</option>"
    +"<option value='2'"+selo(cfg.srcType==2)+">Custom / test URL (Viasat format)</option></select></div>";
  h+="<div class='f full'><label>Endpoint URL</label><input id='vsUrl' type='text' name='vsUrl' value='"+esc(cfg.vsUrl)+"'></div>";
  h+="<div class='f'><label>Poll interval: <b id='pollLbl' style='color:var(--cy)'>"+String(cfg.pollMs/1000.0,2)+" s</b></label>"
     "<input type='range' name='pollMs' min='250' max='20000' step='250' value='"+String(cfg.pollMs)+"' "
     "oninput=\"pollLbl.textContent=(this.value/1000).toFixed(2)+' s'\"></div>";
  h+=fNum("Stale → NCD (ms)","staleMs",cfg.staleMs,"500")+"</div>";
  h+="<div class='note'><b>Viasat</b> and <b>Panasonic</b> use their official on-board endpoints automatically. Choose <b>Custom / test URL</b> to point at your own server (must return the Viasat JSON shape: <code>latitude, longitude, altitude, groundSpeed…</code>).</div></div>";

  h+="<div class='card'><h2>⑤ Emulator — fixed test position</h2><div class='grid'>";
  h+=fTog("Enable emulator (override)","simEnable",cfg.simEnable)+fNum("Latitude","simLat",cfg.simLat)+fNum("Longitude","simLon",cfg.simLon)+fNum("Track (°T)","simTrk",cfg.simTrk)+fNum("Ground speed (kt)","simGs",cfg.simGs)+fNum("Altitude (ft)","simAlt",cfg.simAlt)+"</div>";
  h+="<div class='note'>When <b>enabled</b>, the source feed (the source) is <b>discarded</b> and the EFBs receive a <b>fixed</b> position at the lat/lon above — it does not move. GS/track/alt are sent as set (for display). Uncheck to use the live Source URL.</div></div>";

  h+="<div class='card'><h2>🔒 Security (settings login)</h2><div class='grid'>";
  h+=fTog("Require login","authEnable",cfg.authEnable)+fText("Username","authUser",cfg.authUser);
  h+="<div class='f'><label>New password</label><input type='password' name='authPass' autocomplete='new-password' placeholder='"+String(cfg.authHash.length()?"leave blank to keep":"set a password")+"'></div></div>";
  h+="<div class='note'>Default login <b>admin / password</b> — change it here. When enabled with a password set, the config pages require login. Forgot it? Re-flash or erase NVS to reset.</div></div>";

  h+="<div class='bar'><button type='submit'>Save &amp; reboot</button><button class='ghost' type='button' onclick='location.reload()'>Reload</button></div></form>";

  h+="<div class='card'><h2>📡 Device traffic log <span class='text-muted' style='font-weight:400;font-size:.7rem'>— ADBP + HTTP from EFB clients (auto-refresh)</span></h2>";
  h+="<textarea id='logbox' readonly style='width:100%;height:240px;background:#070d1a;border:1px solid var(--line);border-radius:8px;color:#9fe7f5;font-family:SF Mono,ui-monospace,monospace;font-size:.74rem;padding:.6rem;white-space:pre;overflow:auto'></textarea>";
  h+="<div style='margin-top:.4rem;display:flex;gap:.5rem;align-items:center;flex-wrap:wrap'>"
     "<label class='tog' style='margin:0'><input type='checkbox' id='logEn' "+String(cfg.logEnable?"checked":"")+" onchange=\"fetch('/logtoggle?on='+(this.checked?1:0))\"><span>Live capture</span></label>"
     "<button class='ghost' type='button' onclick='poll()'>Refresh</button>"
     "<button class='ghost' type='button' onclick='copyLog()'>📋 Copy</button>"
     "<a href='/log' target='_blank' style='color:var(--cy);font-size:.8rem'>open raw ↗</a></div></div>";
  h+="<footer><a href='/log' style='color:var(--cy)' target='_blank'>▸ traffic log (ADBP + HTTP probes)</a> · aidlink · ESP32 · "+String(__DATE__)+"</footer></div>";
  h+="<script>async function u(){try{let r=await fetch('/status');let d=await r.json();"
     "sta.innerHTML=(d.sta?\"<span class='dot' style='background:var(--gr)'></span>\":\"<span class='dot' style='background:var(--rd)'></span>\")+(d.ssid||'down');"
     "cli.textContent=d.clients;pos.textContent=d.valid?(d.lat.toFixed(3)+', '+d.lon.toFixed(3)):'no fix';"
     "trk.textContent=d.valid?(Math.round(d.trk)+'° / '+Math.round(d.gs)+'kt'):'—';"
     "src.innerHTML=d.valid?(d.sim?\"<span style='color:var(--am)'>SIM</span>\":\"<span style='color:var(--gr)'>LIVE</span>\"):'—';"
     "document.getElementById('staip').textContent=d.staip||'—';"
     "var fc=d.pollok?'var(--gr)':'var(--rd)';var fa=(d.pollage>=0?d.pollage+'s ago':'');"
     "document.getElementById('feed').innerHTML=\"<span style='color:\"+fc+\"'>\"+(d.pollmsg||'idle')+\"</span> <span style='color:var(--mut);font-size:.8em'>\"+fa+\"</span>\";"
     "}catch(e){}}"
     "async function uc(){try{let r=await fetch('/clients');let a=await r.json();let tb=document.querySelector('#clit tbody');"
     "document.getElementById('clic').textContent='('+a.length+' / "+String(cfg.apMaxClients)+")';"
     "if(!a.length){tb.innerHTML=\"<tr><td colspan='4' class='empty'>no clients connected</td></tr>\";return;}"
     "tb.innerHTML=a.map((c,i)=>'<tr><td>'+(i+1)+'</td><td>'+c.mac+'</td><td>'+c.ip+'</td><td><span class=\"rssi\">'+c.rssi+' dBm</span></td></tr>').join('');"
     "}catch(e){}}"
     "var _dc=document.querySelector(\"input[name='staDhcp']\");"
     "function _td(){var d=_dc.checked;['staIp','staGw','staMask','staDns'].forEach(function(i){var e=document.getElementById(i);if(e)e.disabled=d;});}"
     "if(_dc){_dc.addEventListener('change',_td);_td();}"
     "var _customUrl='"+esc(cfg.vsUrl)+"';"
     "function srcSel(){var t=document.getElementById('srcType').value,u=document.getElementById('vsUrl');"
     "if(t=='0'){u.value='https://wifi.inflight.viasat.com/ac/flight/info';u.disabled=true;}"
     "else if(t=='1'){u.value='http://services.inflightpanasonic.aero/inflight/services/flightdata/v1/flightdata';u.disabled=true;}"
     "else{u.disabled=false;if(u.value.indexOf('viasat.com')>=0||u.value.indexOf('inflightpanasonic')>=0)u.value=_customUrl;u.focus();}}"
     "srcSel();"
     "async function ul(){try{let r=await fetch('/log');let t=await r.text();var b=document.getElementById('logbox');"
     "if(b){var bot=b.scrollTop+b.clientHeight>=b.scrollHeight-24;b.value=t;if(bot)b.scrollTop=b.scrollHeight;}}catch(e){}}"
     "function copyLog(){var b=document.getElementById('logbox');b.focus();b.select();b.setSelectionRange(0,999999);var d=false;"
     "try{d=document.execCommand('copy');}catch(e){}if(!d&&navigator.clipboard){navigator.clipboard.writeText(b.value);d=true;}"
     "window.getSelection&&window.getSelection().removeAllRanges();alert(d?'Log copied to clipboard':'Could not auto-copy — select the text manually');}"
     "async function doScan(btn){btn.disabled=true;var t=btn.textContent;btn.textContent='Scanning…';"
     "try{let r=await fetch('/scan');let a=await r.json();a.sort((x,y)=>y.rssi-x.rssi);"
     "var sel=document.getElementById('scanList');"
     "sel.innerHTML='<option value=\"\">— '+a.length+' found —</option>'+a.map(function(n){"
     "var nm=(n.ssid||'(hidden)');var o=document.createElement('option');o.value=n.ssid;o.textContent=nm+'  '+n.rssi+'dBm'+(n.enc?' 🔒':' 🔓');return o.outerHTML;}).join('');"
     "}catch(e){alert('Scan failed');}btn.disabled=false;btn.textContent=t;}"
     "let _busy=false;async function poll(){if(_busy)return;_busy=true;"   // serialize: ESP32 WebServer is one-connection-at-a-time
     "try{await u();await uc();await ul();}catch(e){}_busy=false;}"
     "poll();setInterval(poll,2500);</script></body></html>";
  web.send(200,"text/html; charset=utf-8",h);
}
static void handleStatus(){
  if(!requireAuth())return;
  PosState s; snapshot(s);
  String j="{"; j+="\"sta\":"+String(WiFi.status()==WL_CONNECTED?"true":"false");
  j+=",\"ssid\":\""+esc(cfg.staSsid)+"\""; j+=",\"clients\":"+String(WiFi.softAPgetStationNum());
  j+=",\"valid\":"+String(s.valid&&(millis()-s.lastFixMs<cfg.staleMs)?"true":"false");
  j+=",\"sim\":"+String(s.simulated?"true":"false");
  char b[120]; snprintf(b,sizeof b,",\"lat\":%.5f,\"lon\":%.5f,\"trk\":%.1f,\"gs\":%.1f,\"alt\":%.0f",s.lat,s.lon,s.trackDeg,s.gsKt,s.altFt); j+=b;
  j+=",\"staip\":\""+(WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():String("—"))+"\"";
  j+=",\"pollok\":"+String(gPollOk?"true":"false");
  j+=",\"pollage\":"+String(gPollAtMs?(long)((millis()-gPollAtMs)/1000):-1);
  j+=",\"pollmsg\":\""+esc(String(gPollMsg))+"\"";
  j+="}";
  web.send(200,"application/json",j);
}
static void handleClients(){
  if(!requireAuth())return;
  wifi_sta_list_t sl; String j="[";
  if(esp_wifi_ap_get_sta_list(&sl)==ESP_OK){
    for(int i=0;i<sl.num;i++){
      const uint8_t* m=sl.sta[i].mac; char mac[18];
      snprintf(mac,sizeof mac,"%02X:%02X:%02X:%02X:%02X:%02X",m[0],m[1],m[2],m[3],m[4],m[5]);
      uint32_t ip=lookupClientIp(m); IPAddress ipa(ip);
      if(i) j+=",";
      j+="{\"mac\":\""+String(mac)+"\",\"ip\":\""+(ip?ipa.toString():String("(pending)"))+"\",\"rssi\":"+String((int)sl.sta[i].rssi)+"}";
    }
  }
  j+="]"; web.send(200,"application/json",j);
}
// ---- AID Web API (HTTP :80) — the EFB detects AID via getAPIVersion ----
static String isoNow(){
  uint64_t ms; xSemaphoreTake(gMux,portMAX_DELAY); ms=g.utcMs; xSemaphoreGive(gMux);
  if(ms<1000000000000ULL) ms=1750000000000ULL+millis();   // fallback when no real UTC yet
  time_t t=ms/1000; struct tm tmv; gmtime_r(&t,&tmv); char b[28];
  strftime(b,sizeof b,"%Y-%m-%dT%H:%M:%SZ",&tmv); return String(b);
}
static void apiResp(const char* cmd,const String& inner){
  String x="<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Response commandName=\"";
  x+=cmd; x+="\" returnCode=\"0\" timestamp=\""+isoNow()+"\">"+inner+"</Response>";
  logln(String("WEBAPI ")+cmd+" -> 200 ("+web.client().remoteIP().toString()+")");
  web.send(200,"text/xml",x);
}
static void apiVersion(){ apiResp("getAPIVersion","<APIVersion APIVersion=\""+cfg.apiVer+"\"/>"); }
static void apiWifi(){ apiResp("getWiFiAPStatus","<WiFiAP WiFiAPStatus=\"ACTIVE\"/>"); }
static void apiAoip(){ apiResp("getAoIPStatus","<AoIPStatus AoIPAvailability=\"DISABLED\" ATSUStatus=\"NORMAL\" IpLinkStatus=\"DISCONNECTED\" VPNUsed=\"AoIP-VPN\" AoIPServerFQDN=\"\" AoIPServerIP=\"\"><ListOfAuthorizedChannels/></AoIPStatus>"); }
static void apiAcars(){ apiResp("getAcarsStatus","<AcarsStatus ATSUStatus=\"NORMAL\" AcarsForEFBAvailability=\"DISABLED\" AcarsLinkStatus=\"DISCONNECTED\"/>"); }
static void apiReboot(){ apiResp("cmdReboot",""); }   // acknowledge but do NOT actually reboot

static void handleLog(){
  if(!requireAuth())return;
  // Stream line-by-line (chunked) — never allocate one giant String, which fragmented the heap
  // and caused an abort() once the largest free block dropped below the buffer size.
  web.setContentLength(CONTENT_LENGTH_UNKNOWN);
  web.send(200,"text/plain; charset=utf-8","");
  int sent=0;
  for(int i=0;i<LOG_N;i++){
    String line; if(logMux) xSemaphoreTake(logMux,portMAX_DELAY);
    int idx=(logHead+i)%LOG_N; if(logBuf[idx].length()) line=logBuf[idx]+"\n";
    if(logMux) xSemaphoreGive(logMux);
    if(line.length()){ web.sendContent(line); sent++; }
  }
  if(!sent) web.sendContent("(no ADBP/HTTP client probes captured yet — connect the EFB, then refresh)\n");
  web.sendContent("");   // terminate chunked response
}
// JSON string escape — escapes the JSON-special and control chars but passes UTF-8 bytes (>=0x20) through
// verbatim, so SSIDs with accented/CJK/emoji characters survive intact.
static String jsonEsc(const String& s){ String o;
  for(size_t i=0;i<s.length();i++){ uint8_t c=(uint8_t)s[i];
    if(c=='"') o+="\\\""; else if(c=='\\') o+="\\\\"; else if(c=='\n') o+="\\n"; else if(c=='\r') o+="\\r";
    else if(c=='\t') o+="\\t"; else if(c<0x20){ char b[8]; snprintf(b,sizeof b,"\\u%04x",c); o+=b; }
    else o+=(char)c; }
  return o; }
static void handleScan(){ if(!requireAuth())return;   // scan uplink Wi-Fi networks; returns JSON [{ssid,rssi,enc}], UTF-8 safe
  int n=WiFi.scanNetworks(false,true);   // synchronous, include hidden
  String j="["; j.reserve(n>0?n*48:8);
  for(int i=0;i<n && i<48;i++){ if(j.length()>1) j+=",";
    j+="{\"ssid\":\""+jsonEsc(WiFi.SSID(i))+"\",\"rssi\":"+String(WiFi.RSSI(i))
      +",\"enc\":"+String(WiFi.encryptionType(i)==WIFI_AUTH_OPEN?"false":"true")+"}"; }
  j+="]"; WiFi.scanDelete();
  web.send(200,"application/json; charset=utf-8",j);
}
static void handleLogToggle(){ if(!requireAuth())return;   // live enable/disable of traffic logging (no reboot), persisted
  if(web.hasArg("on")) cfg.logEnable = web.arg("on").toInt()!=0;
  prefs.begin("aidlink",false); prefs.putBool("logEnable",cfg.logEnable); prefs.end();
  Serial.printf("[LOG] live capture %s\n",cfg.logEnable?"ENABLED":"DISABLED");
  web.send(200,"text/plain; charset=utf-8",cfg.logEnable?"on":"off");
}
static void serveLoginForm(bool err){
  String h; h.reserve(4000);
  h="<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  h+="<title>AIDlink — Sign in</title><style>"+String(CSS)+"</style></head><body><div class='wrap' style='max-width:430px'>";
  h+="<div class='hero'><div><div class='logo'><span style='-webkit-text-fill-color:var(--cy);color:var(--cy)'>AID</span><span style='-webkit-text-fill-color:var(--gr);color:var(--gr)'>link</span></div><div class='sub'>Settings access</div></div></div>";
  h+="<div class='card'><h2>🔒 Sign in</h2><form method='POST' action='/login'><div class='grid'>";
  h+="<div class='f full'><label>Username</label><input type='text' name='u' autocomplete='username' autofocus></div>";
  h+="<div class='f full'><label>Password</label><input type='password' name='p' autocomplete='current-password'></div>";
  h+="<div class='f full'><label>&nbsp;</label><label class='tog'><input type='checkbox' name='remember'><span>Stay connected on this device</span></label></div></div>";
  if(err) h+="<div class='note' style='color:var(--rd)'>Invalid username or password.</div>";
  h+="<div class='bar'><button type='submit'>Sign in</button></div></form></div></div></body></html>";
  web.send(err?401:200,"text/html; charset=utf-8",h);
}
static void handleLogin(){
  if(web.method()==HTTP_POST){
    bool ok = cfg.authEnable && cfg.authHash.length()
              && web.arg("u")==cfg.authUser && sha256hex(cfg.authSalt+":"+web.arg("p"))==cfg.authHash;
    if(ok){
      gSessTok=randHex(16); gSessSeen=millis(); gSessRemember=web.hasArg("remember");
      prefs.begin("aidlink",false);
      if(gSessRemember) prefs.putString("authTok",gSessTok); else prefs.remove("authTok");
      prefs.end();
      String c="AIDSESS="+gSessTok+"; Path=/; HttpOnly; SameSite=Strict";
      if(gSessRemember) c+="; Max-Age=2592000";   // ~30 days persistent cookie
      web.sendHeader("Set-Cookie",c); web.sendHeader("Location","/"); web.send(302,"text/plain","ok"); return;
    }
    delay(700); serveLoginForm(true); return;   // throttle failed attempts
  }
  if(authed()){ web.sendHeader("Location","/"); web.send(302,"text/plain","already in"); return; }
  serveLoginForm(false);
}
static void handleLogout(){
  gSessTok=""; gSessRemember=false;
  prefs.begin("aidlink",false); prefs.remove("authTok"); prefs.end();
  web.sendHeader("Set-Cookie","AIDSESS=; Path=/; HttpOnly; Max-Age=0");
  web.sendHeader("Location","/login"); web.send(302,"text/plain","bye");
}
static void handleNotFound(){
  String a; for(int i=0;i<web.args();i++){ a+=(a.length()?"&":"")+web.argName(i)+"="+web.arg(i); }
  String ua=web.hasHeader("User-Agent")?web.header("User-Agent"):"";
  const char* m=web.method()==HTTP_GET?"GET":web.method()==HTTP_POST?"POST":web.method()==HTTP_PUT?"PUT":"?";
  logln(String("HTTP ")+m+" "+web.uri()+(a.length()?("?"+a):"")+(ua.length()?("  UA="+ua):""));
  web.send(404,"text/plain","not found");
}
static void handleSave(){
  if(!requireAuth())return;
  auto S=[&](const char*k,String&d){ if(web.hasArg(k)) d=web.arg(k); };
  auto B=[&](const char*k,bool&d){ d=web.hasArg(k); };
  auto I=[&](const char*k,int&d){ if(web.hasArg(k)) d=web.arg(k).toInt(); };
  auto U=[&](const char*k,uint32_t&d){ if(web.hasArg(k)) d=(uint32_t)web.arg(k).toInt(); };
  auto D=[&](const char*k,double&d){ if(web.hasArg(k)) d=web.arg(k).toFloat(); };
  // IP setter: accept blank (means "use default/upstream") or a syntactically valid dotted-quad; reject
  // garbage so a bad field can't become 0.0.0.0 and break the AP/uplink.
  auto IP=[&](const char*k,String&d){ if(web.hasArg(k)){ String v=web.arg(k); v.trim(); IPAddress t; if(v.length()==0||t.fromString(v)) d=v; } };
  S("staSsid",cfg.staSsid); S("staPass",cfg.staPass); S("apSsid",cfg.apSsid); S("apPass",cfg.apPass);
  B("staDhcp",cfg.staDhcp);                                   // uplink DHCP toggle (was not persisted)
  IP("staIp",cfg.staIp); IP("staGw",cfg.staGw); IP("staMask",cfg.staMask); IP("staDns",cfg.staDns);  // static uplink (inputs disabled when DHCP -> not submitted -> unchanged)
  B("apHidden",cfg.apHidden); IP("apIp",cfg.apIp); IP("apMask",cfg.apMask); IP("apLease",cfg.apLease);
  I("apChannel",cfg.apChannel); I("apMaxClients",cfg.apMaxClients);     // AP/DHCP fields (were not persisted)
  I("apDhcpCount",cfg.apDhcpCount); I("apLeaseMin",cfg.apLeaseMin); IP("apClientDns",cfg.apClientDns);
  I("srcType",cfg.srcType); if(cfg.srcType<0||cfg.srcType>2)cfg.srcType=0;
  S("vsUrl",cfg.vsUrl); U("pollMs",cfg.pollMs); U("staleMs",cfg.staleMs);
  B("simEnable",cfg.simEnable); D("simLat",cfg.simLat); D("simLon",cfg.simLon); D("simTrk",cfg.simTrk); D("simGs",cfg.simGs); D("simAlt",cfg.simAlt);
  I("adbpPort",cfg.adbpPort); I("dsPort",cfg.dsPort); B("napt",cfg.naptEnable); S("devName",cfg.devName);
  S("acTail",cfg.acTail); S("acType",cfg.acType); S("apiVer",cfg.apiVer);
  B("logEnable",cfg.logEnable);
  B("authEnable",cfg.authEnable); S("authUser",cfg.authUser);
  if(web.hasArg("authPass") && web.arg("authPass").length()){     // change password (blank = keep current)
    cfg.authSalt=randHex(8); cfg.authHash=sha256hex(cfg.authSalt+":"+web.arg("authPass"));
    gSessTok=""; prefs.begin("aidlink",false); prefs.remove("authTok"); prefs.end();   // invalidate sessions on change
  }
  // clamp numeric fields to safe ranges
  if(cfg.apMaxClients<1)cfg.apMaxClients=1; if(cfg.apMaxClients>10)cfg.apMaxClients=10;
  if(cfg.apChannel<0||cfg.apChannel>13)cfg.apChannel=0;
  if(cfg.apDhcpCount<1)cfg.apDhcpCount=1; if(cfg.apDhcpCount>62)cfg.apDhcpCount=62;
  if(cfg.apLeaseMin<1)cfg.apLeaseMin=1;
  if(cfg.pollMs<250)cfg.pollMs=250; if(cfg.pollMs>60000)cfg.pollMs=60000;
  if(cfg.staleMs<1000)cfg.staleMs=1000;
  saveConfig();
  web.send(200,"text/html; charset=utf-8","<meta charset='utf-8'><meta http-equiv='refresh' content='6;url=/'><body style='background:#070b14;color:#eaf2ff;font-family:system-ui;text-align:center;padding-top:18vh'><h2 style='color:#22d3ee'>Saved ✓</h2><p>Rebooting to apply… returning in a few seconds.</p></body>");
  delay(400); ESP.restart();
}

// ----------------------------- WIFI / SETUP ---------------------------------
static void applyDhcpPool(IPAddress start,int count,int leaseMin){
  esp_netif_t* ap = WiFi.AP.netif(); if(!ap) return;
  if(count<1)count=1; if(count>62)count=62;
  int last = start[3]+count-1; if(last>254)last=254;
  IPAddress end(start[0],start[1],start[2],last);
  esp_netif_dhcps_stop(ap);
  dhcps_lease_t l; l.enable=true; l.start_ip.addr=(uint32_t)start; l.end_ip.addr=(uint32_t)end;
  esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &l, sizeof(l));
  uint32_t lt=(uint32_t)(leaseMin<1?1:leaseMin);
  esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME, &lt, sizeof(lt));
  esp_netif_dhcps_start(ap);
  Serial.printf("[DHCP] pool %s-%s lease=%lumin\n",start.toString().c_str(),end.toString().c_str(),(unsigned long)lt);
}
static void startAP(){
  IPAddress ip,mask,lease,dns; ip.fromString(cfg.apIp); mask.fromString(cfg.apMask); lease.fromString(cfg.apLease);
  if(cfg.apClientDns.length()) dns.fromString(cfg.apClientDns);
  else { dns=WiFi.dnsIP(); if((uint32_t)dns==0) dns=ip; }
  WiFi.softAPConfig(ip,ip,mask,lease,dns);
  int ch=cfg.apChannel>0?cfg.apChannel:(WiFi.channel()?WiFi.channel():1);
  int mx=cfg.apMaxClients; if(mx<1)mx=1; if(mx>10)mx=10;
  WiFi.softAP(cfg.apSsid.c_str(), cfg.apPass.c_str(), ch, cfg.apHidden?1:0, mx);
  delay(200);
  applyDhcpPool(lease, cfg.apDhcpCount, cfg.apLeaseMin);
  bool napt=cfg.naptEnable ? WiFi.AP.enableNAPT(true) : (WiFi.AP.enableNAPT(false),false);
  Serial.printf("[AP] %s%s IP=%s ch=%d max=%d NAPT=%s DNS->%s\n",cfg.apSsid.c_str(),cfg.apHidden?"(hidden)":"",
                WiFi.softAPIP().toString().c_str(),ch,mx,napt?"ON":"off",dns.toString().c_str());
}
void setup(){
  Serial.begin(115200); delay(300);
  Serial.println(String("\n[aidlink] boot ")+FW_BUILD);   // always printed (even if traffic logging is disabled) for flash verification
  loadConfig(); SIM_LAT=cfg.simLat; SIM_LON=cfg.simLon;
  // one-time config migration: force corrected defaults past any stale saved NVS values
  if(cfg.cfgVer < 7){
    cfg.acTail="TEST01"; cfg.apiVer="3.1"; cfg.frameLen=1; cfg.frameDelim=0; cfg.framePrologEach=true;
    if(cfg.cfgVer < 4) cfg.apSsid="AIDlink";   // one-time AP-name rename; user can change it afterwards
    if(cfg.cfgVer < 5){ cfg.logEnable=false; cfg.simEnable=false; }   // one-time: traffic logging + emulator off
    if(cfg.authHash.length()==0){ cfg.authEnable=true; cfg.authUser="admin";   // seed default login admin/password
      cfg.authSalt=randHex(8); cfg.authHash=sha256hex(cfg.authSalt+":password"); }
    cfg.cfgVer=7; saveConfig();
  }
  { prefs.begin("aidlink",true); gSessTok=prefs.getString("authTok",""); prefs.end();   // restore a "stay connected" session across reboots
    gSessRemember=gSessTok.length()>0; gSessSeen=millis(); }
  gMux=xSemaphoreCreateMutex(); logMux=xSemaphoreCreateMutex();
  logln(String("==== BOOT aidlink ")+FW_BUILD+" ====");
  logln(String("[CFG] apiVer=")+cfg.apiVer+" emulator="+(cfg.simEnable?"ON(fixed pos)":"off")+" logCapture="+(cfg.logEnable?"on":"off"));
  WiFi.persistent(false); WiFi.mode(WIFI_AP_STA); WiFi.setSleep(false);
  WiFi.onEvent(onWiFiEvent);
  WiFi.setHostname(cfg.devName.c_str());
  if(!cfg.staDhcp && cfg.staIp.length()){
    IPAddress ip,gw,mask,dns; ip.fromString(cfg.staIp); gw.fromString(cfg.staGw); mask.fromString(cfg.staMask);
    if(cfg.staDns.length()) dns.fromString(cfg.staDns); else dns=gw;
    WiFi.config(ip,gw,mask,dns);
    Serial.printf("[STA] static %s gw %s\n",cfg.staIp.c_str(),cfg.staGw.c_str());
  }
  WiFi.begin(cfg.staSsid.c_str(), cfg.staPass.length()?cfg.staPass.c_str():nullptr);
  Serial.printf("[STA] connecting to %s",cfg.staSsid.c_str());
  uint32_t t0=millis(); while(WiFi.status()!=WL_CONNECTED && millis()-t0<30000){ delay(500); Serial.print("."); } Serial.println();
  if(WiFi.status()==WL_CONNECTED){ Serial.printf("[STA] up IP=%s ch=%d\n",WiFi.localIP().toString().c_str(),WiFi.channel());
    configTime(0,0,"pool.ntp.org","time.google.com","time.cloudflare.com"); }  // SNTP -> current UTC for timestamps
  else Serial.println("[STA] not connected (serving sim position)");
  startAP();
  adbp=WiFiServer(cfg.adbpPort); adbp.begin(); adbp.setNoDelay(true);
  { static const char* hdrs[]={"User-Agent","Cookie"}; web.collectHeaders(hdrs,2); }
  web.on("/",handleRoot); web.on("/status",handleStatus); web.on("/clients",handleClients);
  web.on("/login",handleLogin); web.on("/login",HTTP_POST,handleLogin); web.on("/logout",handleLogout);
  web.on("/log",handleLog); web.on("/logtoggle",handleLogToggle); web.on("/scan",handleScan); web.on("/save",HTTP_POST,handleSave);
  web.on("/getAPIVersion",apiVersion); web.on("/getWiFiAPStatus",apiWifi);
  web.on("/getAoIPStatus",apiAoip); web.on("/getAcarsStatus",apiAcars); web.on("/cmdReboot",apiReboot);
  web.onNotFound(handleNotFound); web.begin();
  // mDNS: reachable as <devName>.local (e.g. aidlink.local) from any client on the AP
  { String host=cfg.devName; host.toLowerCase();
    String h2; for(unsigned i=0;i<host.length();i++){ char c=host[i]; h2+=((c>='a'&&c<='z')||(c>='0'&&c<='9'))?c:'-'; }
    while(h2.indexOf("--")>=0) h2.replace("--","-");
    if(MDNS.begin(h2.c_str())){ MDNS.addService("http","tcp",80); MDNS.addService("aidlink-adbp","tcp",cfg.adbpPort);
      Serial.printf("[mDNS] http://%s.local/  (AP %s)\n",h2.c_str(),WiFi.softAPIP().toString().c_str()); }
    else Serial.println("[mDNS] start failed"); }
  Serial.printf("[ADBP] ADBP server on %s:%d   [WEB] http://%s/\n",
                WiFi.softAPIP().toString().c_str(),cfg.adbpPort,WiFi.softAPIP().toString().c_str());
  xTaskCreatePinnedToCore(pollerTask,"poller",8192,nullptr,1,nullptr,0);
}
void loop(){
  serialCmd();   // host can request a full log dump over USB without rebooting the board
  static wl_status_t last=WL_IDLE_STATUS; static uint32_t lastRetry=0,lastSim=0;
  wl_status_t s=WiFi.status();
  if(s!=last){ if(s==WL_CONNECTED) startAP(); last=s; }
  if(s!=WL_CONNECTED && millis()-lastRetry>10000){ lastRetry=millis(); WiFi.begin(cfg.staSsid.c_str(), cfg.staPass.length()?cfg.staPass.c_str():nullptr); }
  uint32_t now=millis(); if(now-lastSim>=1000){ simStep(now-lastSim); lastSim=now; }
  web.handleClient();
  WiFiClient cl=adbp.available(); if(cl) handleAdbp(cl);
  pushSubs();
  web.handleClient();                  // 2nd service after the (now short) ADBP/push work -> keep UI snappy
  static uint32_t lastHeap=0; if(now-lastHeap>15000){ lastHeap=now;
    logln("HEAP free="+String(ESP.getFreeHeap())+"B  minFree="+String(ESP.getMinFreeHeap())+"B  largestBlock="+String(ESP.getMaxAllocHeap())+"B"); }
  delay(5);
}
