// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#pragma once
#include <Arduino.h>
#include <IPAddress.h>

// ADBP (ARINC 834-7) value type codes
enum AdbpType { ADBP_F64 = 0, ADBP_S32 = 5, ADBP_STR = 7 };

// Persisted configuration
struct Config {
  String staSsid, staPass;                 // uplink we connect to
  bool   staDhcp; String staIp, staGw, staMask, staDns;  // static uplink (if !staDhcp)
  String apSsid,  apPass;  bool apHidden;  // cockpit AP we feed
  String apIp, apMask, apLease;            // AID identity (default 172.20.1.1/26)
  int    apChannel, apMaxClients, apDhcpCount, apLeaseMin; String apClientDns; // AP/DHCP tuning
  int    srcType;          // position source: 0=Viasat, 1=Panasonic, 2=custom/test URL (Viasat format)
  String vsUrl;            uint32_t pollMs, staleMs;   // custom/test source URL (used when srcType=2)
  bool   simEnable; double simLat, simLon, simTrk, simGs, simAlt;
  int    adbpPort;  int dsPort;  bool naptEnable;  String devName;   // dsPort = EFB DataStreamPort
  int    frameLen, frameDelim; bool framePrologEach;   // ADBP stream framing experiment (len:0 full/1 method/2 omit; delim:0 none/1 CRLF/2 LF/3 NUL)
  int    cfgVer;                                        // config-migration version (forces corrected defaults past stale NVS)
  bool   logEnable;                                     // master switch for live device traffic logging
  bool   authEnable; String authUser, authHash, authSalt;   // settings-page login (salted SHA-256, no plaintext)
  String acTail, acType, apiVer;           // Aircraft identity (tail, aircraft type, Web API version)
};

// Live ownship state
struct PosState {
  bool valid=false, simulated=false, haveTrack=false, fixed=false;   // fixed=emulator override, do not dead-reckon
  double lat=0,lon=0,altFt=0,gsKt=0,trackDeg=0, prevLat=0,prevLon=0; bool havePrev=false;
  uint64_t utcMs=0; uint32_t lastFixMs=0, prevFixMs=0; double gsDerivedKt=0; bool serviceAvail=false;
  char flight[16]={0},tail[12]={0},orig[8]={0},dest[8]={0};
};

// ADBP subscription record (push to client callback port)
struct Sub {
  bool active=false; IPAddress ip; uint16_t port=0; bool onEvent=false;
  uint32_t periodMs=5000,lastPushMs=0,lastFixSeen=0; char params[1400]={0};
  uint16_t workPort=0; uint8_t tryIdx=0; bool dumped=false;   // discovered/working push port; one-shot msg dump
  uint32_t connMs=0, pushCount=0; bool wasConnected=false;    // stream-lifetime + msg-count instrumentation
};
