// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Web config portal on :80 (esp_http_server). The served pages reproduce the v9
// Arduino portal byte-for-byte: same CSS, layout, fields, status strip, clients
// table, traffic log, and inline polling JS. Streamed as HTTP chunks (empty
// strings are skipped — a zero-length chunk would terminate the response).
#include "web.h"
#include "auth.h"
#include "netcore.h"
#include "pos.h"
#include "poller.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web";
static aidlink_cfg_t *CFG;
static httpd_handle_t s_http;

// Firmware build string shown in the UI (analogous to v9's FW_BUILD).
static const char *fw_build(void) {
    static char b[96];
    if (!b[0]) snprintf(b, sizeof b, "%s %s %s", esp_app_get_description()->version, __DATE__, __TIME__);
    return b;
}

// ---- chunk helpers (skip empty strings: a 0-length chunk ends the response) ----
static void chunk(httpd_req_t *r, const char *s) { if (s && s[0]) httpd_resp_sendstr_chunk(r, s); }
static void chunkf(httpd_req_t *r, const char *fmt, ...) {
    char buf[640]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (buf[0]) httpd_resp_sendstr_chunk(r, buf);
}

// v9 esc(): escapes only " < > &  (NOT ') — reproduce exactly for byte parity.
static char *esc(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const char *p = src ? src : ""; *p && o + 1 < cap; p++) {
        const char *rep = NULL;
        switch (*p) { case '"': rep = "&quot;"; break; case '<': rep = "&lt;"; break;
                      case '>': rep = "&gt;"; break; case '&': rep = "&amp;"; break; default: break; }
        if (rep) { size_t rl = strlen(rep); if (o + rl + 1 > cap) break; memcpy(dst + o, rep, rl); o += rl; }
        else dst[o++] = *p;
    }
    dst[o] = 0; return dst;
}
static void esc_chunk(httpd_req_t *r, const char *s) { char e[256]; esc(e, sizeof e, s); chunk(r, e); }

// v9 jsonEsc(): JSON-escape control/special chars, pass UTF-8 (>=0x20) through.
static char *json_esc(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p && o + 6 < cap; p++) {
        if (*p == '"') { memcpy(dst + o, "\\\"", 2); o += 2; }
        else if (*p == '\\') { memcpy(dst + o, "\\\\", 2); o += 2; }
        else if (*p == '\n') { memcpy(dst + o, "\\n", 2); o += 2; }
        else if (*p == '\r') { memcpy(dst + o, "\\r", 2); o += 2; }
        else if (*p == '\t') { memcpy(dst + o, "\\t", 2); o += 2; }
        else if (*p < 0x20) { o += snprintf(dst + o, cap - o, "\\u%04x", *p); }
        else dst[o++] = *p;
    }
    dst[o] = 0; return dst;
}

// ---- v9 field helpers (byte-identical output) ----
static void ff_text(httpd_req_t *r, const char *lbl, const char *nm, const char *val, const char *type, bool full) {
    chunkf(r, "<div class='f%s'><label>%s</label><input type='%s' name='%s' value='", full ? " full" : "", lbl, type, nm);
    esc_chunk(r, val);
    chunk(r, "'></div>");
}
static void ff_num(httpd_req_t *r, const char *lbl, const char *nm, double v, const char *step) {
    char b[40]; snprintf(b, sizeof b, "%g", v);
    chunkf(r, "<div class='f'><label>%s</label><input type='number' step='%s' name='%s' value='%s'></div>", lbl, step, nm, b);
}
static void ff_tog(httpd_req_t *r, const char *lbl, const char *nm, bool v) {
    chunkf(r, "<div class='f'><label>&nbsp;</label><label class='tog'><input type='checkbox' name='%s' %s><span>%s</span></label></div>",
           nm, v ? "checked" : "", lbl);
}

static bool gate(httpd_req_t *r) {
    char cookie[200] = {0};
    httpd_req_get_hdr_value_str(r, "Cookie", cookie, sizeof cookie);
    if (auth_ok(CFG, cookie[0] ? cookie : NULL)) return true;
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "/login");
    httpd_resp_send(r, NULL, 0);
    return false;
}

// v9 CSS blob (lines 582-628), verbatim.
static const char *CSS =
":root{--bg:#070b14;--bg2:#0d1322;--card:#111a2e;--line:#1e2a44;--txt:#eaf2ff;--mut:#8aa0c0;--cy:#22d3ee;--cy2:#38bdf8;--am:#fbbf24;--gr:#34d399;--rd:#f87171}\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{background:radial-gradient(1200px 600px at 80% -10%,rgba(34,211,238,.12),transparent),linear-gradient(180deg,#070b14,#0a1020);color:var(--txt);font-family:-apple-system,'SF Pro Display',Segoe UI,system-ui,sans-serif;line-height:1.5;min-height:100vh}\n"
".wrap{max-width:920px;margin:0 auto;padding:clamp(1rem,3vw,2.2rem)}\n"
".hero{display:flex;align-items:center;gap:1rem;padding:1.3rem 1.5rem;border:1px solid var(--line);border-radius:18px;background:linear-gradient(135deg,rgba(34,211,238,.10),rgba(56,189,248,.03));margin-bottom:1.3rem;position:relative;overflow:hidden}\n"
".hero::after{content:\"\";position:absolute;right:-40px;top:-40px;width:180px;height:180px;background:radial-gradient(circle,rgba(34,211,238,.25),transparent 70%);filter:blur(8px)}\n"
".logo{font-weight:800;font-size:1.6rem;letter-spacing:.14em;background:linear-gradient(135deg,#fff,#22d3ee);-webkit-background-clip:text;background-clip:text;-webkit-text-fill-color:transparent}\n"
".hero .sub{color:var(--mut);font-size:.82rem;letter-spacing:.05em}\n"
".badge{margin-left:auto;font-size:.7rem;font-weight:700;text-transform:uppercase;letter-spacing:.1em;padding:.3rem .7rem;border-radius:100px;border:1px solid rgba(34,211,238,.4);color:var(--cy);background:rgba(34,211,238,.08);z-index:1}\n"
".status{display:grid;grid-template-columns:repeat(auto-fit,minmax(135px,1fr));gap:.6rem;margin-bottom:1.3rem}\n"
".s{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:.7rem .9rem}\n"
".s .k{font-size:.62rem;text-transform:uppercase;letter-spacing:.12em;color:var(--mut)}\n"
".s .v{font-family:'SF Mono',ui-monospace,monospace;font-size:1.05rem;font-weight:700;margin-top:.15rem}\n"
".dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:.35rem;vertical-align:middle}\n"
".card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:1.2rem 1.3rem;margin-bottom:1.1rem}\n"
".card h2{font-size:.82rem;text-transform:uppercase;letter-spacing:.12em;color:var(--cy);margin-bottom:.9rem;display:flex;align-items:center;gap:.5rem;border-bottom:1px solid var(--line);padding-bottom:.6rem}\n"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:.8rem 1.1rem}\n"
"@media(max-width:620px){.grid{grid-template-columns:1fr}}\n"
".f{display:flex;flex-direction:column;gap:.3rem}\n"
".f.full{grid-column:1/-1}\n"
"label{font-size:.72rem;color:var(--mut);letter-spacing:.04em}\n"
"input[type=text],input[type=password],input[type=number]{background:#070d1a;border:1px solid var(--line);border-radius:9px;color:var(--txt);padding:.6rem .7rem;font-family:'SF Mono',ui-monospace,monospace;font-size:.9rem;width:100%;transition:.15s}\n"
"input:focus{outline:none;border-color:var(--cy);box-shadow:0 0 0 3px rgba(34,211,238,.15)}\n"
"input:disabled{opacity:.5;cursor:not-allowed;background:#0a1120;border-style:dashed}\n"
"select{appearance:none;-webkit-appearance:none;background:#070d1a url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%2322d3ee' stroke-width='3' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M6 9l6 6 6-6'/%3E%3C/svg%3E\") no-repeat right .8rem center;border:1px solid var(--line);border-radius:9px;color:var(--txt);padding:.6rem 2.1rem .6rem .7rem;font-family:'SF Mono',ui-monospace,monospace;font-size:.9rem;width:100%;cursor:pointer;transition:.15s}\n"
"select:hover{border-color:var(--cy2)}\n"
"select:focus{outline:none;border-color:var(--cy);box-shadow:0 0 0 3px rgba(34,211,238,.15)}\n"
"select option{background:#0d1322;color:var(--txt)}\n"
".tog{display:flex;align-items:center;gap:.6rem;background:#070d1a;border:1px solid var(--line);border-radius:9px;padding:.55rem .7rem;cursor:pointer}\n"
".tog input{appearance:none;width:38px;height:22px;background:var(--line);border-radius:100px;position:relative;cursor:pointer;transition:.2s;flex:0 0 auto}\n"
".tog input:checked{background:var(--cy)}\n"
".tog input::after{content:\"\";position:absolute;width:16px;height:16px;border-radius:50%;background:#fff;top:3px;left:3px;transition:.2s}\n"
".tog input:checked::after{left:19px}\n"
".tog span{font-size:.82rem}\n"
".bar{display:flex;gap:.7rem;align-items:center;position:sticky;bottom:0;padding:1rem 0}\n"
"button{background:linear-gradient(135deg,var(--cy),var(--cy2));color:#04121c;border:none;font-weight:800;letter-spacing:.04em;padding:.8rem 1.6rem;border-radius:11px;cursor:pointer;font-size:.95rem;box-shadow:0 6px 20px rgba(34,211,238,.25)}\n"
"button.ghost{background:transparent;color:var(--mut);border:1px solid var(--line);box-shadow:none}\n"
".note{color:var(--mut);font-size:.74rem;margin-top:.3rem}\n"
".warn{font-size:.72rem;color:var(--am);background:rgba(251,191,36,.07);border:1px solid rgba(251,191,36,.25);border-radius:10px;padding:.6rem .8rem;margin-bottom:1.1rem}\n"
"footer{color:#4a5b78;font-size:.72rem;text-align:center;padding:1.4rem 0}\n"
".ctbl{width:100%;border-collapse:collapse;font-size:.85rem}\n"
".ctbl th{text-align:left;color:var(--mut);font-size:.66rem;text-transform:uppercase;letter-spacing:.1em;padding:.45rem .5rem;border-bottom:1px solid var(--line)}\n"
".ctbl td{padding:.5rem;border-bottom:1px solid rgba(30,42,68,.5);font-family:'SF Mono',ui-monospace,monospace}\n"
".ctbl tr:hover td{background:rgba(34,211,238,.05)}\n"
".rssi{display:inline-block;min-width:42px}\n"
".empty{color:var(--mut);font-style:italic;padding:.6rem .5rem}\n"
"code{background:#070d1a;border:1px solid var(--line);border-radius:5px;padding:.05rem .35rem;font-size:.82em}\n";

static void send_hero(httpd_req_t *r, const char *sub_extra) {
    chunk(r, "<div class='hero'><div><div class='logo'><span style='-webkit-text-fill-color:var(--cy);color:var(--cy)'>AID</span><span style='-webkit-text-fill-color:var(--gr);color:var(--gr)'>link</span></div><div class='sub'>");
    chunk(r, sub_extra);
    chunk(r, "</div></div>");
}

// ---------- root config page (byte-identical to v9 handleRoot) ----------
static esp_err_t h_root(httpd_req_t *r) {
    if (!gate(r)) return ESP_OK;
    const aidlink_cfg_t *c = CFG;
    char e1[128];
    httpd_resp_set_type(r, "text/html; charset=utf-8");

    chunk(r, "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
    chunk(r, "<title>"); esc_chunk(r, c->dev_name); chunk(r, " — Config</title><style>");
    chunk(r, CSS);
    chunk(r, "</style></head><body><div class='wrap'>");

    // hero
    chunk(r, "<div class='hero'><div><div class='logo'><span style='-webkit-text-fill-color:var(--cy);color:var(--cy)'>AID</span><span style='-webkit-text-fill-color:var(--gr);color:var(--gr)'>link</span></div><div class='sub'>ARINC 834 ownship bridge · ");
    esc_chunk(r, c->dev_name);
    chunk(r, " · <b>"); chunk(r, fw_build()); chunk(r, "</b>");
    if (c->auth_enable && c->auth_hash[0]) chunk(r, " · <a href='/logout' style='color:var(--cy)'>log out</a>");
    chunk(r, "</div></div><span class='badge'>"); chunk(r, c->ap_ip); chunk(r, "</span></div>");

    // status strip (8 tiles)
    chunk(r, "<div class='status' id='st'><div class='s'><div class='k'>Uplink</div><div class='v' id='sta'>…</div></div>"
             "<div class='s'><div class='k'>AP clients</div><div class='v' id='cli'>…</div></div>"
             "<div class='s'><div class='k'>Position</div><div class='v' id='pos'>…</div></div>"
             "<div class='s'><div class='k'>Track / GS</div><div class='v' id='trk'>…</div></div>"
             "<div class='s'><div class='k'>Source</div><div class='v' id='src'>…</div></div>"
             "<div class='s'><div class='k'>Uplink IP</div><div class='v' id='staip' style='font-size:.85rem'>…</div></div>"
             "<div class='s'><div class='k'>Feed</div><div class='v' id='feed' style='font-size:.85rem'>…</div></div>"
             "<div class='s'><div class='k'>Firmware</div><div class='v' style='font-size:.8rem' title='version · date & time flashed'>");
    esc_chunk(r, fw_build());
    chunk(r, "</div></div></div>");

    // clients table
    chunk(r, "<div class='card'><h2>🖧 Connected clients <span id='clic' class='text-muted' style='font-weight:400;font-size:.7rem'></span></h2>"
             "<div style='overflow-x:auto'><table class='ctbl' id='clit'><thead><tr><th>#</th><th>MAC</th><th>IP</th><th>Signal</th></tr></thead>"
             "<tbody><tr><td colspan='4' class='empty'>scanning…</td></tr></tbody></table></div></div>");

    chunk(r, "<div class='warn'>⚠ Experimental, non-certified. Ownship position only — never use as a primary/backup navigation source. Saving reboots the device to apply.</div>");
    chunk(r, "<form method='POST' action='/save'>");

    // ① Uplink Wi-Fi
    bool dh = c->sta_dhcp;
    uint8_t sip[4]; bool up = netcore_sta_up(sip);
    char ipv[16], gwv[16], mkv[16], dnv[16];
    if (dh && up) snprintf(ipv, sizeof ipv, "%u.%u.%u.%u", sip[0], sip[1], sip[2], sip[3]);
    else strlcpy(ipv, c->sta_ip, sizeof ipv);
    strlcpy(gwv, c->sta_gw, sizeof gwv); strlcpy(mkv, c->sta_mask, sizeof mkv); strlcpy(dnv, c->sta_dns, sizeof dnv);
    const char *dis = dh ? " disabled" : "";
    chunk(r, "<div class='card'><h2>① Uplink Wi-Fi (we connect to)</h2><div class='grid'>");
    chunk(r, "<div class='f full'><label>Scan &amp; select a network</label><div style='display:flex;gap:.5rem'>"
             "<select id='scanList' style='flex:1' onchange=\"if(this.value){var s=document.querySelector('input[name=staSsid]');s.value=this.value;var p=document.querySelector('input[name=staPass]');if(p){p.value='';p.focus();}}\"><option value=''>— press Scan —</option></select>"
             "<button type='button' class='ghost' onclick='doScan(this)'>Scan</button></div></div>");
    ff_text(r, "SSID", "staSsid", c->sta_ssid, "text", false);
    ff_text(r, "Password", "staPass", c->sta_pass, "password", false);
    ff_tog(r, "Use DHCP", "staDhcp", c->sta_dhcp);
    chunkf(r, "<div class='f'><label>Static IP</label><input id='staIp' type='text' name='staIp' value='%s'%s></div>", esc(e1, sizeof e1, ipv), dis);
    chunkf(r, "<div class='f'><label>Gateway</label><input id='staGw' type='text' name='staGw' value='%s'%s></div>", esc(e1, sizeof e1, gwv), dis);
    chunkf(r, "<div class='f'><label>Netmask</label><input id='staMask' type='text' name='staMask' value='%s'%s></div>", esc(e1, sizeof e1, mkv), dis);
    chunkf(r, "<div class='f'><label>DNS</label><input id='staDns' type='text' name='staDns' value='%s'%s></div>", esc(e1, sizeof e1, dnv), dis);
    chunk(r, "</div><div class='note'>With <b>Use DHCP</b> on, these show the live DHCP-assigned values (read-only). Uncheck to edit them as a static config. Aircraft: <b>OpenCabinWiFi</b> (open — blank password), DHCP on.</div></div>");

    // ② Cockpit AP
    chunk(r, "<div class='card'><h2>② Cockpit AP (we feed / EFB joins)</h2><div class='grid'>");
    ff_text(r, "SSID", "apSsid", c->ap_ssid, "text", false);
    ff_text(r, "Password (≥8)", "apPass", c->ap_pass, "password", false);
    ff_tog(r, "Hidden SSID", "apHidden", c->ap_hidden);
    chunk(r, "</div><div class='note'>For lab compatibility testing, set the AP SSID (and Hidden flag) that your EFB client expects.</div></div>");

    // 🆔 Aircraft identity
    chunk(r, "<div class='card'><h2>🆔 Aircraft identity</h2><div class='grid'>");
    ff_text(r, "Aircraft tail / registration", "acTail", c->ac_tail, "text", false);
    ff_text(r, "Aircraft type", "acType", c->ac_type, "text", false);
    ff_text(r, "Web API version", "apiVer", c->api_ver, "text", false);
    chunk(r, "</div><div class='note'><code>getAPIVersion</code> → ");
    esc_chunk(r, c->api_ver);
    chunk(r, " (AID = 2.0, AID = 3.1). Tail/type are answered over ADBP for params containing TAIL/REG or ACTYPE/MODEL.</div></div>");

    // ③ Network / DHCP / AP radio
    chunk(r, "<div class='card'><h2>③ Network · DHCP · AP radio</h2><div class='grid'>");
    ff_text(r, "AP / AID IP", "apIp", c->ap_ip, "text", false);
    ff_text(r, "Netmask", "apMask", c->ap_mask, "text", false);
    ff_text(r, "DHCP pool start", "apLease", c->ap_lease, "text", false);
    ff_num(r, "DHCP pool size", "apDhcpCount", c->ap_dhcp_count, "1");
    ff_num(r, "Lease time (min)", "apLeaseMin", c->ap_lease_min, "1");
    ff_text(r, "Client DNS (blank=uplink)", "apClientDns", c->ap_client_dns, "text", false);
    ff_num(r, "AP channel (0=follow STA)", "apChannel", c->ap_channel, "1");
    ff_num(r, "Max clients (≤10)", "apMaxClients", c->ap_max_clients, "1");
    ff_num(r, "ADBP TCP port", "adbpPort", c->adbp_port, "1");
    ff_num(r, "EFB DataStreamPort", "dsPort", c->ds_port, "1");
    ff_tog(r, "NAT to uplink", "napt", c->napt_enable);
    ff_text(r, "Device name", "devName", c->dev_name, "text", false);
    chunk(r, "</div><div class='note'>Default AP = 172.20.1.1 /26, ADBP on 24000 · keep the DHCP pool inside .2–.62 for /26.</div></div>");

    // ④ Position source
    chunk(r, "<div class='card'><h2>④ Position source</h2><div class='grid'>");
    chunkf(r, "<div class='f full'><label>Provider</label><select id='srcType' name='srcType' onchange='srcSel()'>"
              "<option value='0'%s>Viasat</option>"
              "<option value='1'%s>Panasonic</option>"
              "<option value='2'%s>Custom / test URL (Viasat format)</option></select></div>",
           c->src_type == 0 ? " selected" : "", c->src_type == 1 ? " selected" : "", c->src_type == 2 ? " selected" : "");
    chunk(r, "<div class='f full'><label>Endpoint URL</label><input id='vsUrl' type='text' name='vsUrl' value='");
    esc_chunk(r, c->vs_url);
    chunk(r, "'></div>");
    chunkf(r, "<div class='f'><label>Poll interval: <b id='pollLbl' style='color:var(--cy)'>%.2f s</b></label>"
              "<input type='range' name='pollMs' min='250' max='20000' step='250' value='%lu' "
              "oninput=\"pollLbl.textContent=(this.value/1000).toFixed(2)+' s'\"></div>",
           c->poll_ms / 1000.0, (unsigned long)c->poll_ms);
    ff_num(r, "Stale → NCD (ms)", "staleMs", c->stale_ms, "500");
    chunk(r, "</div><div class='note'><b>Viasat</b> and <b>Panasonic</b> use their official on-board endpoints automatically. Choose <b>Custom / test URL</b> to point at your own server (must return the Viasat JSON shape: <code>latitude, longitude, altitude, groundSpeed…</code>).</div></div>");

    // ⑤ Emulator
    chunk(r, "<div class='card'><h2>⑤ Emulator — fixed test position</h2><div class='grid'>");
    ff_tog(r, "Enable emulator (override)", "simEnable", c->sim_enable);
    ff_num(r, "Latitude", "simLat", c->sim_lat, "any");
    ff_num(r, "Longitude", "simLon", c->sim_lon, "any");
    ff_num(r, "Track (°T)", "simTrk", c->sim_trk, "any");
    ff_num(r, "Ground speed (kt)", "simGs", c->sim_gs, "any");
    ff_num(r, "Altitude (ft)", "simAlt", c->sim_alt, "any");
    chunk(r, "</div><div class='note'>When <b>enabled</b>, the source feed (the source) is <b>discarded</b> and the EFBs receive a <b>fixed</b> position at the lat/lon above — it does not move. GS/track/alt are sent as set (for display). Uncheck to use the live Source URL.</div></div>");

    // 🔒 Security
    chunk(r, "<div class='card'><h2>🔒 Security (settings login)</h2><div class='grid'>");
    ff_tog(r, "Require login", "authEnable", c->auth_enable);
    ff_text(r, "Username", "authUser", c->auth_user, "text", false);
    chunkf(r, "<div class='f'><label>New password</label><input type='password' name='authPass' autocomplete='new-password' placeholder='%s'></div></div>",
           c->auth_hash[0] ? "leave blank to keep" : "set a password");
    chunk(r, "<div class='note'>Default login <b>admin / password</b> — change it here. When enabled with a password set, the config pages require login. Forgot it? Re-flash or erase NVS to reset.</div></div>");

    chunk(r, "<div class='bar'><button type='submit'>Save &amp; reboot</button><button class='ghost' type='button' onclick='location.reload()'>Reload</button></div></form>");

    // traffic log card
    chunk(r, "<div class='card'><h2>📡 Device traffic log <span class='text-muted' style='font-weight:400;font-size:.7rem'>— ADBP + HTTP from EFB clients (auto-refresh)</span></h2>");
    chunk(r, "<textarea id='logbox' readonly style='width:100%;height:240px;background:#070d1a;border:1px solid var(--line);border-radius:8px;color:#9fe7f5;font-family:SF Mono,ui-monospace,monospace;font-size:.74rem;padding:.6rem;white-space:pre;overflow:auto'></textarea>");
    chunkf(r, "<div style='margin-top:.4rem;display:flex;gap:.5rem;align-items:center;flex-wrap:wrap'>"
              "<label class='tog' style='margin:0'><input type='checkbox' id='logEn' %s onchange=\"fetch('/logtoggle?on='+(this.checked?1:0))\"><span>Live capture</span></label>"
              "<button class='ghost' type='button' onclick='poll()'>Refresh</button>"
              "<button class='ghost' type='button' onclick='copyLog()'>📋 Copy</button>"
              "<a href='/log' target='_blank' style='color:var(--cy);font-size:.8rem'>open raw ↗</a></div></div>",
           c->log_enable ? "checked" : "");
    chunk(r, "<footer><a href='/log' style='color:var(--cy)' target='_blank'>▸ traffic log (ADBP + HTTP probes)</a> · aidlink · ESP32 · ");
    chunk(r, __DATE__);
    chunk(r, "</footer></div>");

    // inline JS (split into pieces; interpolate apMaxClients + vsUrl)
    chunk(r, "<script>async function u(){try{let r=await fetch('/status');let d=await r.json();"
             "sta.innerHTML=(d.sta?\"<span class='dot' style='background:var(--gr)'></span>\":\"<span class='dot' style='background:var(--rd)'></span>\")+(d.ssid||'down');"
             "cli.textContent=d.clients;pos.textContent=d.valid?(d.lat.toFixed(3)+', '+d.lon.toFixed(3)):'no fix';"
             "trk.textContent=d.valid?(Math.round(d.trk)+'° / '+Math.round(d.gs)+'kt'):'—';"
             "src.innerHTML=d.valid?(d.sim?\"<span style='color:var(--am)'>SIM</span>\":\"<span style='color:var(--gr)'>LIVE</span>\"):'—';"
             "document.getElementById('staip').textContent=d.staip||'—';"
             "var fc=d.pollok?'var(--gr)':'var(--rd)';var fa=(d.pollage>=0?d.pollage+'s ago':'');"
             "document.getElementById('feed').innerHTML=\"<span style='color:\"+fc+\"'>\"+(d.pollmsg||'idle')+\"</span> <span style='color:var(--mut);font-size:.8em'>\"+fa+\"</span>\";"
             "}catch(e){}}");
    chunkf(r, "async function uc(){try{let r=await fetch('/clients');let a=await r.json();let tb=document.querySelector('#clit tbody');"
              "document.getElementById('clic').textContent='('+a.length+' / %d)';"
              "if(!a.length){tb.innerHTML=\"<tr><td colspan='4' class='empty'>no clients connected</td></tr>\";return;}"
              "tb.innerHTML=a.map((c,i)=>'<tr><td>'+(i+1)+'</td><td>'+c.mac+'</td><td>'+c.ip+'</td><td><span class=\"rssi\">'+c.rssi+' dBm</span></td></tr>').join('');"
              "}catch(e){}}", c->ap_max_clients);
    chunk(r, "var _dc=document.querySelector(\"input[name='staDhcp']\");"
             "function _td(){var d=_dc.checked;['staIp','staGw','staMask','staDns'].forEach(function(i){var e=document.getElementById(i);if(e)e.disabled=d;});}"
             "if(_dc){_dc.addEventListener('change',_td);_td();}");
    chunk(r, "var _customUrl='"); { char je[160]; esc_chunk(r, c->vs_url); (void)je; } chunk(r, "';");
    chunk(r, "function srcSel(){var t=document.getElementById('srcType').value,u=document.getElementById('vsUrl');"
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
             "let _busy=false;async function poll(){if(_busy)return;_busy=true;"
             "try{await u();await uc();await ul();}catch(e){}_busy=false;}"
             "poll();setInterval(poll,2500);</script></body></html>");

    httpd_resp_sendstr_chunk(r, NULL);
    return ESP_OK;
}

// ---------- login ----------
static void send_login_form(httpd_req_t *r, bool err) {
    httpd_resp_set_status(r, err ? "401 Unauthorized" : "200 OK");
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    chunk(r, "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
    chunk(r, "<title>AIDlink — Sign in</title><style>"); chunk(r, CSS); chunk(r, "</style></head><body><div class='wrap' style='max-width:430px'>");
    send_hero(r, "Settings access");
    chunk(r, "</div>");
    chunk(r, "<div class='card'><h2>🔒 Sign in</h2><form method='POST' action='/login'><div class='grid'>");
    chunk(r, "<div class='f full'><label>Username</label><input type='text' name='u' autocomplete='username' autofocus></div>");
    chunk(r, "<div class='f full'><label>Password</label><input type='password' name='p' autocomplete='current-password'></div>");
    chunk(r, "<div class='f full'><label>&nbsp;</label><label class='tog'><input type='checkbox' name='remember'><span>Stay connected on this device</span></label></div></div>");
    if (err) chunk(r, "<div class='note' style='color:var(--rd)'>Invalid username or password.</div>");
    chunk(r, "<div class='bar'><button type='submit'>Sign in</button></div></form></div></div></body></html>");
    httpd_resp_sendstr_chunk(r, NULL);
}

static esp_err_t h_login_get(httpd_req_t *r) {
    char cookie[200] = {0};
    httpd_req_get_hdr_value_str(r, "Cookie", cookie, sizeof cookie);
    if (auth_ok(CFG, cookie[0] ? cookie : NULL)) {
        httpd_resp_set_status(r, "302 Found");
        httpd_resp_set_hdr(r, "Location", "/");
        httpd_resp_send(r, NULL, 0);
        return ESP_OK;
    }
    send_login_form(r, false);
    return ESP_OK;
}

static esp_err_t h_login_post(httpd_req_t *r) {
    char body[256] = {0};
    int n = r->content_len < (int)sizeof body - 1 ? r->content_len : (int)sizeof body - 1;
    int got = httpd_req_recv(r, body, n);
    if (got > 0) body[got] = 0;
    char u[48] = {0}, p[64] = {0}, rem[8] = {0}, cookie[200];
    web_form_field(body, "u", u, sizeof u);
    web_form_field(body, "p", p, sizeof p);
    bool remember = web_form_field(body, "remember", rem, sizeof rem);
    if (auth_login(CFG, u, p, remember, cookie, sizeof cookie)) {
        httpd_resp_set_status(r, "302 Found");
        httpd_resp_set_hdr(r, "Set-Cookie", cookie);
        httpd_resp_set_hdr(r, "Location", "/");
        httpd_resp_send(r, NULL, 0);
        return ESP_OK;
    }
    vTaskDelay(pdMS_TO_TICKS(700));
    send_login_form(r, true);
    return ESP_OK;
}

static esp_err_t h_logout(httpd_req_t *r) {
    char cookie[120];
    auth_logout(cookie, sizeof cookie);
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Set-Cookie", cookie);
    httpd_resp_set_hdr(r, "Location", "/login");
    httpd_resp_send(r, NULL, 0);
    return ESP_OK;
}

// ---------- save ----------
static bool fld(const char *body, const char *k, char *out, size_t n) { return web_form_field(body, k, out, n); }
// IP setter: accept blank or a syntactically valid dotted-quad (reject garbage).
static void fld_ip(const char *body, const char *k, char *dst, size_t cap) {
    char v[40];
    if (!fld(body, k, v, sizeof v)) return;
    if (v[0] == 0) { dst[0] = 0; return; }
    unsigned a, b, c, d;
    if (sscanf(v, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 && c < 256 && d < 256)
        strlcpy(dst, v, cap);
}

static esp_err_t h_save(httpd_req_t *r) {
    if (!gate(r)) return ESP_OK;
    int len = r->content_len;
    if (len <= 0 || len > 4096) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK; }
    char *body = malloc(len + 1);
    if (!body) { httpd_resp_send_500(r); return ESP_OK; }
    int off = 0, got;
    while (off < len && (got = httpd_req_recv(r, body + off, len - off)) > 0) off += got;
    body[off] = 0;

    aidlink_cfg_t *c = CFG;
    char v[160];
    if (fld(body, "staSsid", v, sizeof v)) strlcpy(c->sta_ssid, v, sizeof c->sta_ssid);
    if (fld(body, "staPass", v, sizeof v)) strlcpy(c->sta_pass, v, sizeof c->sta_pass);
    c->sta_dhcp = fld(body, "staDhcp", v, sizeof v);
    fld_ip(body, "staIp", c->sta_ip, sizeof c->sta_ip);
    fld_ip(body, "staGw", c->sta_gw, sizeof c->sta_gw);
    fld_ip(body, "staMask", c->sta_mask, sizeof c->sta_mask);
    fld_ip(body, "staDns", c->sta_dns, sizeof c->sta_dns);
    if (fld(body, "apSsid", v, sizeof v)) strlcpy(c->ap_ssid, v, sizeof c->ap_ssid);
    if (fld(body, "apPass", v, sizeof v)) strlcpy(c->ap_pass, v, sizeof c->ap_pass);
    c->ap_hidden = fld(body, "apHidden", v, sizeof v);
    fld_ip(body, "apIp", c->ap_ip, sizeof c->ap_ip);
    fld_ip(body, "apMask", c->ap_mask, sizeof c->ap_mask);
    fld_ip(body, "apLease", c->ap_lease, sizeof c->ap_lease);
    if (fld(body, "apChannel", v, sizeof v)) c->ap_channel = atoi(v);
    if (fld(body, "apMaxClients", v, sizeof v)) c->ap_max_clients = atoi(v);
    if (fld(body, "apDhcpCount", v, sizeof v)) c->ap_dhcp_count = atoi(v);
    if (fld(body, "apLeaseMin", v, sizeof v)) c->ap_lease_min = atoi(v);
    fld_ip(body, "apClientDns", c->ap_client_dns, sizeof c->ap_client_dns);
    if (fld(body, "srcType", v, sizeof v)) { c->src_type = atoi(v); if (c->src_type < 0 || c->src_type > 2) c->src_type = 0; }
    if (fld(body, "vsUrl", v, sizeof v)) strlcpy(c->vs_url, v, sizeof c->vs_url);
    if (fld(body, "pollMs", v, sizeof v)) c->poll_ms = strtoul(v, 0, 10);
    if (fld(body, "staleMs", v, sizeof v)) c->stale_ms = strtoul(v, 0, 10);
    c->sim_enable = fld(body, "simEnable", v, sizeof v);
    if (fld(body, "simLat", v, sizeof v)) c->sim_lat = atof(v);
    if (fld(body, "simLon", v, sizeof v)) c->sim_lon = atof(v);
    if (fld(body, "simTrk", v, sizeof v)) c->sim_trk = atof(v);
    if (fld(body, "simGs", v, sizeof v)) c->sim_gs = atof(v);
    if (fld(body, "simAlt", v, sizeof v)) c->sim_alt = atof(v);
    if (fld(body, "adbpPort", v, sizeof v)) c->adbp_port = atoi(v);
    if (fld(body, "dsPort", v, sizeof v)) c->ds_port = atoi(v);
    c->napt_enable = fld(body, "napt", v, sizeof v);
    if (fld(body, "devName", v, sizeof v)) strlcpy(c->dev_name, v, sizeof c->dev_name);
    if (fld(body, "acTail", v, sizeof v)) strlcpy(c->ac_tail, v, sizeof c->ac_tail);
    if (fld(body, "acType", v, sizeof v)) strlcpy(c->ac_type, v, sizeof c->ac_type);
    if (fld(body, "apiVer", v, sizeof v)) strlcpy(c->api_ver, v, sizeof c->api_ver);
    c->log_enable = fld(body, "logEnable", v, sizeof v);
    c->auth_enable = fld(body, "authEnable", v, sizeof v);
    if (fld(body, "authUser", v, sizeof v)) strlcpy(c->auth_user, v, sizeof c->auth_user);
    if (fld(body, "authPass", v, sizeof v) && v[0]) {
        auth_rand_hex(c->auth_salt, 8);
        auth_hash(c->auth_salt, v, c->auth_hash);
        auth_reset_session();
    }
    // clamps (v9)
    if (c->ap_max_clients < 1) c->ap_max_clients = 1;
    if (c->ap_max_clients > 10) c->ap_max_clients = 10;
    if (c->ap_channel < 0 || c->ap_channel > 13) c->ap_channel = 0;
    if (c->ap_dhcp_count < 1) c->ap_dhcp_count = 1;
    if (c->ap_dhcp_count > 62) c->ap_dhcp_count = 62;
    if (c->ap_lease_min < 1) c->ap_lease_min = 1;
    if (c->poll_ms < 250) c->poll_ms = 250;
    if (c->poll_ms > 60000) c->poll_ms = 60000;
    if (c->stale_ms < 1000) c->stale_ms = 1000;
    free(body);

    cfg_save(c);
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_sendstr(r, "<meta charset='utf-8'><meta http-equiv='refresh' content='6;url=/'><body style='background:#070b14;color:#eaf2ff;font-family:system-ui;text-align:center;padding-top:18vh'><h2 style='color:#22d3ee'>Saved ✓</h2><p>Rebooting to apply… returning in a few seconds.</p></body>");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

// ---------- JSON status / clients / scan ----------
static esp_err_t h_status(httpd_req_t *r) {
    if (!gate(r)) return ESP_OK;
    uint8_t sip[4] = {0}; bool up = netcore_sta_up(sip);
    char staip[20]; if (up) snprintf(staip, sizeof staip, "%u.%u.%u.%u", sip[0], sip[1], sip[2], sip[3]); else strlcpy(staip, "—", sizeof staip);
    pos_state_t p; pos_get(&p);
    uint32_t nowms = (uint32_t)(esp_timer_get_time() / 1000);
    bool valid = p.valid && (nowms - p.last_fix_ms < CFG->stale_ms);
    bool pok; uint32_t pat; char pmsg[64]; poller_status(&pok, &pat, pmsg, sizeof pmsg);
    long pollage = pat ? (long)((nowms - pat) / 1000) : -1;
    char pmsg_e[80], ssid_e[80]; esc(pmsg_e, sizeof pmsg_e, pmsg); esc(ssid_e, sizeof ssid_e, CFG->sta_ssid);

    char json[640];
    snprintf(json, sizeof json,
        "{\"sta\":%s,\"ssid\":\"%s\",\"clients\":%d,\"valid\":%s,\"sim\":%s,"
        "\"lat\":%.5f,\"lon\":%.5f,\"trk\":%.1f,\"gs\":%.1f,\"alt\":%.0f,\"staip\":\"%s\","
        "\"pollok\":%s,\"pollage\":%ld,\"pollmsg\":\"%s\"}",
        up ? "true" : "false", ssid_e, netcore_ap_client_count(),
        valid ? "true" : "false", p.simulated ? "true" : "false",
        p.lat, p.lon, p.track_deg, p.gs_kt, p.alt_ft, staip,
        pok ? "true" : "false", pollage, pmsg_e);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, json);
    return ESP_OK;
}

static esp_err_t h_clients(httpd_req_t *r) {
    if (!gate(r)) return ESP_OK;
    httpd_resp_set_type(r, "application/json");
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) { httpd_resp_sendstr(r, "[]"); return ESP_OK; }
    chunk(r, "[");
    for (int i = 0; i < list.num; i++) {
        wifi_sta_info_t *s = &list.sta[i];
        // IP per-MAC isn't tracked (no DHCP-lease map); report "(pending)" like v9's fallback.
        chunkf(r, "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"ip\":\"(pending)\",\"rssi\":%d}",
               i ? "," : "", s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5], s->rssi);
    }
    chunk(r, "]");
    httpd_resp_sendstr_chunk(r, NULL);
    return ESP_OK;
}

static esp_err_t h_scan(httpd_req_t *r) {
    if (!gate(r)) return ESP_OK;
    httpd_resp_set_type(r, "application/json; charset=utf-8");
    wifi_scan_config_t sc = { .show_hidden = true };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) { httpd_resp_sendstr(r, "[]"); return ESP_OK; }
    uint16_t n = 0; esp_wifi_scan_get_ap_num(&n);
    if (n > 48) n = 48;
    wifi_ap_record_t *recs = n ? calloc(n, sizeof(*recs)) : NULL;
    if (recs) esp_wifi_scan_get_ap_records(&n, recs);
    chunk(r, "[");
    for (int i = 0; recs && i < n; i++) {
        char je[100]; json_esc(je, sizeof je, (char *)recs[i].ssid);
        chunkf(r, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"enc\":%s}",
               i ? "," : "", je, recs[i].rssi, recs[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
    }
    free(recs);
    chunk(r, "]");
    httpd_resp_sendstr_chunk(r, NULL);
    return ESP_OK;
}

// ---------- traffic log ----------
static void log_emit(const char *line, void *ctx) {
    httpd_req_t *r = ctx;
    httpd_resp_sendstr_chunk(r, line);
    httpd_resp_sendstr_chunk(r, "\n");
}
static esp_err_t h_log(httpd_req_t *r) {
    if (!gate(r)) return ESP_OK;
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    int n = log_foreach(log_emit, r);
    if (!n) httpd_resp_sendstr_chunk(r, "(no ADBP/HTTP client probes captured yet — connect the EFB, then refresh)\n");
    httpd_resp_sendstr_chunk(r, NULL);
    return ESP_OK;
}
static esp_err_t h_logtoggle(httpd_req_t *r) {
    if (!gate(r)) return ESP_OK;
    char q[32] = {0}, on[4] = {0};
    if (httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "on", on, sizeof on) == ESP_OK) {
        CFG->log_enable = atoi(on) != 0;
        log_set_enable(CFG->log_enable);
        cfg_save(CFG);
    }
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_sendstr(r, CFG->log_enable ? "on" : "off");
    return ESP_OK;
}

// ---------- AID Web API (XML, public) ----------
static void api_resp(httpd_req_t *r, const char *cmd, const char *inner) {
    char buf[512];
    time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm);
    char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    snprintf(buf, sizeof buf,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Response commandName=\"%s\" returnCode=\"0\" timestamp=\"%s\">%s</Response>",
        cmd, ts, inner);
    logln("WEBAPI %s -> 200", cmd);
    httpd_resp_set_type(r, "text/xml");
    httpd_resp_sendstr(r, buf);
}
static esp_err_t h_api_ver(httpd_req_t *r) { char x[80]; snprintf(x,sizeof x,"<APIVersion APIVersion=\"%s\"/>", CFG->api_ver); api_resp(r,"getAPIVersion",x); return ESP_OK; }
static esp_err_t h_api_wifi(httpd_req_t *r) { api_resp(r,"getWiFiAPStatus","<WiFiAP WiFiAPStatus=\"ACTIVE\"/>"); return ESP_OK; }
static esp_err_t h_api_aoip(httpd_req_t *r) { api_resp(r,"getAoIPStatus","<AoIPStatus AoIPAvailability=\"DISABLED\" ATSUStatus=\"NORMAL\" IpLinkStatus=\"DISCONNECTED\" VPNUsed=\"AoIP-VPN\" AoIPServerFQDN=\"\" AoIPServerIP=\"\"><ListOfAuthorizedChannels/></AoIPStatus>"); return ESP_OK; }
static esp_err_t h_api_acars(httpd_req_t *r) { api_resp(r,"getAcarsStatus","<AcarsStatus ATSUStatus=\"NORMAL\" AcarsForEFBAvailability=\"DISABLED\" AcarsLinkStatus=\"DISCONNECTED\"/>"); return ESP_OK; }
static esp_err_t h_api_reboot(httpd_req_t *r) { api_resp(r,"cmdReboot",""); return ESP_OK; }

static esp_err_t h_404(httpd_req_t *r, httpd_err_code_t e) {
    logln("HTTP %s", r->uri);
    httpd_resp_set_status(r, "404 Not Found");
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_sendstr(r, "not found");
    return ESP_OK;
}

static void reg(const char *uri, httpd_method_t m, esp_err_t (*fn)(httpd_req_t *)) {
    httpd_uri_t u = { .uri = uri, .method = m, .handler = fn };
    httpd_register_uri_handler(s_http, &u);
}

void web_start(aidlink_cfg_t *cfg) {
    CFG = cfg;
    log_set_enable(cfg->log_enable);
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 24;
    hc.stack_size = 8192;
    hc.lru_purge_enable = true;
    if (httpd_start(&s_http, &hc) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }
    reg("/", HTTP_GET, h_root);
    reg("/status", HTTP_GET, h_status);
    reg("/clients", HTTP_GET, h_clients);
    reg("/scan", HTTP_GET, h_scan);
    reg("/log", HTTP_GET, h_log);
    reg("/logtoggle", HTTP_GET, h_logtoggle);
    reg("/save", HTTP_POST, h_save);
    reg("/login", HTTP_GET, h_login_get);
    reg("/login", HTTP_POST, h_login_post);
    reg("/logout", HTTP_GET, h_logout);
    reg("/getAPIVersion", HTTP_GET, h_api_ver);
    reg("/getWiFiAPStatus", HTTP_GET, h_api_wifi);
    reg("/getAoIPStatus", HTTP_GET, h_api_aoip);
    reg("/getAcarsStatus", HTTP_GET, h_api_acars);
    reg("/cmdReboot", HTTP_GET, h_api_reboot);
    httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, h_404);
    ESP_LOGI(TAG, "[WEB] http://%s/", cfg->ap_ip);
}
