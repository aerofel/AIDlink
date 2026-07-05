// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Web config portal on :80 (esp_http_server). Streams the settings page as
// chunks; parses /save; salted-SHA-256 auth with a session cookie. Mirrors the
// v9 Arduino portal's routes, fields, and JSON/XML API shapes.
#include "web.h"
#include "auth.h"
#include "netcore.h"
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

// ---- small chunk helpers ----
static void chunk(httpd_req_t *r, const char *s) { httpd_resp_sendstr_chunk(r, s); }
static void chunkf(httpd_req_t *r, const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    httpd_resp_sendstr_chunk(r, buf);
}
static void esc_chunk(httpd_req_t *r, const char *s) {
    char e[256]; web_html_esc(e, sizeof e, s); httpd_resp_sendstr_chunk(r, e);
}

// text/number/toggle form fields
static void ff_text(httpd_req_t *r, const char *lbl, const char *nm, const char *val, const char *type, bool full) {
    chunkf(r, "<div class='f%s'><label>%s</label><input type='%s' name='%s' value='",
           full ? " full" : "", lbl, type, nm);
    esc_chunk(r, val);
    chunk(r, "'></div>");
}
static void ff_num(httpd_req_t *r, const char *lbl, const char *nm, double v, const char *step) {
    chunkf(r, "<div class='f'><label>%s</label><input type='number' step='%s' name='%s' value='%g'></div>", lbl, step, nm, v);
}
static void ff_tog(httpd_req_t *r, const char *lbl, const char *nm, bool v) {
    chunkf(r, "<div class='f tog'><label><input type='checkbox' name='%s'%s> %s</label></div>",
           nm, v ? " checked" : "", lbl);
}

static bool gate(httpd_req_t *r) {   // returns true if allowed; else redirects to /login
    char cookie[160] = {0};
    httpd_req_get_hdr_value_str(r, "Cookie", cookie, sizeof cookie);
    if (auth_ok(CFG, cookie[0] ? cookie : NULL)) return true;
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "/login");
    httpd_resp_send(r, NULL, 0);
    return false;
}

// ---------- login ----------
static void send_login_form(httpd_req_t *r, bool err) {
    httpd_resp_set_status(r, err ? "401 Unauthorized" : "200 OK");
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    chunk(r, "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
             "<title>AIDlink sign in</title><style>body{background:#070b14;color:#eaf2ff;font-family:system-ui;"
             "display:flex;min-height:100vh;align-items:center;justify-content:center}form{background:#0e1526;"
             "padding:28px;border-radius:14px;border:1px solid #1c2942;min-width:280px}h2{color:#22d3ee;margin:0 0 16px}"
             "input{width:100%;box-sizing:border-box;margin:6px 0;padding:10px;border-radius:8px;border:1px solid #24344f;"
             "background:#0a1120;color:#eaf2ff}button{width:100%;margin-top:12px;padding:11px;border:0;border-radius:8px;"
             "background:#22d3ee;color:#04121a;font-weight:700}.e{color:#f87171;font-size:13px}label{font-size:13px}</style>"
             "<form method=POST action=/login><h2>AIDlink</h2>");
    if (err) chunk(r, "<div class=e>Invalid credentials</div>");
    chunk(r, "<input name=u placeholder=User autocomplete=username>"
             "<input name=p type=password placeholder=Password autocomplete=current-password>"
             "<label><input type=checkbox name=remember> Stay signed in</label>"
             "<button>Sign in</button></form>");
    httpd_resp_sendstr_chunk(r, NULL);
}

static esp_err_t h_login_get(httpd_req_t *r) {
    char cookie[160] = {0};
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
    vTaskDelay(pdMS_TO_TICKS(700));   // throttle brute force
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

// ---------- root config page ----------
static const char *PAGE_CSS =
    "*{box-sizing:border-box}body{background:#070b14;color:#eaf2ff;font-family:system-ui;margin:0}"
    ".wrap{max-width:900px;margin:0 auto;padding:18px}"
    ".hero{display:flex;align-items:center;gap:12px;margin-bottom:14px}"
    ".hero h1{font-size:20px;margin:0;color:#22d3ee}.hero .v{color:#64748b;font-size:12px}"
    ".hero a{margin-left:auto;color:#93c5fd;font-size:13px}"
    ".status{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:14px}"
    ".status .t{background:#0e1526;border:1px solid #1c2942;border-radius:10px;padding:8px 12px;font-size:12px;min-width:96px}"
    ".status .t b{display:block;color:#cbd5e1;font-size:14px;margin-top:2px}"
    ".card{background:#0e1526;border:1px solid #1c2942;border-radius:14px;padding:16px;margin-bottom:14px}"
    ".card h3{margin:0 0 12px;font-size:15px;color:#e2e8f0}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}"
    ".f{display:flex;flex-direction:column;gap:4px}.f.full{grid-column:1/-1}.f label{font-size:12px;color:#94a3b8}"
    ".f input,.f select{padding:9px;border-radius:8px;border:1px solid #24344f;background:#0a1120;color:#eaf2ff}"
    ".f.tog{justify-content:center}.f.tog label{color:#cbd5e1;font-size:14px}"
    ".warn{background:#3f1d1d;border:1px solid #7f1d1d;color:#fecaca;border-radius:10px;padding:10px;font-size:13px;margin-bottom:14px}"
    ".bar{display:flex;gap:10px;margin:6px 0 20px}button{padding:11px 18px;border:0;border-radius:9px;font-weight:700;cursor:pointer}"
    ".save{background:#22d3ee;color:#04121a}.ghost{background:#1c2942;color:#cbd5e1}";

static esp_err_t h_root(httpd_req_t *r) {
    if (!gate(r)) return ESP_OK;
    const aidlink_cfg_t *c = CFG;
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    chunk(r, "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>");
    chunk(r, "<title>AIDlink</title><style>"); chunk(r, PAGE_CSS); chunk(r, "</style><div class=wrap>");
    chunkf(r, "<div class=hero><h1>AIDlink</h1><span class=v>%s</span><a href=/logout>log out</a></div>", esp_app_get_description()->version);

    // status strip (network subset; position fills in once M4 poller lands)
    uint8_t sip[4] = {0}; bool up = netcore_sta_up(sip);
    chunk(r, "<div class=status>");
    chunkf(r, "<div class=t>Uplink<b>%s</b></div>", up ? "UP" : "down");
    chunkf(r, "<div class=t>AP clients<b>%d</b></div>", netcore_ap_client_count());
    if (up) chunkf(r, "<div class=t>Uplink IP<b>%u.%u.%u.%u</b></div>", sip[0], sip[1], sip[2], sip[3]);
    chunkf(r, "<div class=t>Firmware<b>%s</b></div>", esp_app_get_description()->version);
    chunk(r, "</div>");

    chunk(r, "<div class=warn>EXPERIMENTAL — not certified, not for operational use.</div>");
    chunk(r, "<form method=POST action=/save>");

    // (1) Uplink Wi-Fi
    chunk(r, "<div class=card><h3>\xE2\x91\xA0 Uplink Wi-Fi</h3><div class=grid>");
    ff_text(r, "SSID", "staSsid", c->sta_ssid, "text", false);
    ff_text(r, "Password", "staPass", c->sta_pass, "password", false);
    ff_tog(r, "Use DHCP", "staDhcp", c->sta_dhcp);
    ff_text(r, "Static IP", "staIp", c->sta_ip, "text", false);
    ff_text(r, "Gateway", "staGw", c->sta_gw, "text", false);
    ff_text(r, "Netmask", "staMask", c->sta_mask, "text", false);
    ff_text(r, "DNS", "staDns", c->sta_dns, "text", false);
    chunk(r, "</div></div>");

    // (2) Cockpit AP
    chunk(r, "<div class=card><h3>\xE2\x91\xA1 Cockpit AP</h3><div class=grid>");
    ff_text(r, "AP SSID", "apSsid", c->ap_ssid, "text", false);
    ff_text(r, "AP Password", "apPass", c->ap_pass, "text", false);
    ff_tog(r, "Hidden SSID", "apHidden", c->ap_hidden);
    chunk(r, "</div></div>");

    // (id) Aircraft identity
    chunk(r, "<div class=card><h3>\xF0\x9F\x86\x94 Aircraft identity</h3><div class=grid>");
    ff_text(r, "Tail", "acTail", c->ac_tail, "text", false);
    ff_text(r, "Type", "acType", c->ac_type, "text", false);
    ff_text(r, "API version", "apiVer", c->api_ver, "text", false);
    chunk(r, "</div></div>");

    // (3) Network / DHCP / radio
    char apip[16]; snprintf(apip, sizeof apip, "%u.%u.%u.%u", c->ap_ip[0], c->ap_ip[1], c->ap_ip[2], c->ap_ip[3]);
    chunk(r, "<div class=card><h3>\xE2\x91\xA2 Network \xC2\xB7 DHCP \xC2\xB7 AP radio</h3><div class=grid>");
    ff_text(r, "AP IP", "apIp", apip, "text", false);
    ff_num(r, "AP prefix (/n)", "apPrefix", c->ap_prefix, "1");
    ff_num(r, "DHCP pool size", "apDhcpCount", c->ap_dhcp_count, "1");
    ff_num(r, "Lease (min)", "apLeaseMin", c->ap_lease_min, "1");
    ff_text(r, "Client DNS (blank=uplink)", "apClientDns", c->ap_client_dns, "text", false);
    ff_num(r, "AP channel (0=auto)", "apChannel", c->ap_channel, "1");
    ff_num(r, "Max clients", "apMaxClients", c->ap_max_clients, "1");
    ff_num(r, "ADBP TCP port", "adbpPort", c->adbp_port, "1");
    ff_num(r, "EFB DataStreamPort", "dsPort", c->ds_port, "1");
    ff_tog(r, "NAT (share uplink)", "napt", c->napt_enable);
    ff_text(r, "Device name (mDNS)", "devName", c->dev_name, "text", false);
    chunk(r, "</div></div>");

    // (4) Position source
    chunk(r, "<div class=card><h3>\xE2\x91\xA3 Position source</h3><div class=grid>");
    chunkf(r, "<div class=f><label>Provider</label><select name=srcType>"
              "<option value=0%s>Viasat</option><option value=1%s>Panasonic</option><option value=2%s>Custom URL</option>"
              "</select></div>",
           c->src_type == 0 ? " selected" : "", c->src_type == 1 ? " selected" : "", c->src_type == 2 ? " selected" : "");
    ff_text(r, "Custom URL", "vsUrl", c->vs_url, "text", true);
    ff_num(r, "Poll (ms)", "pollMs", c->poll_ms, "1");
    ff_num(r, "Stale (ms)", "staleMs", c->stale_ms, "1");
    chunk(r, "</div></div>");

    // (5) Emulator
    chunk(r, "<div class=card><h3>\xE2\x91\xA4 Emulator</h3><div class=grid>");
    ff_tog(r, "Enable emulator", "simEnable", c->sim_enable);
    ff_num(r, "Lat", "simLat", c->sim_lat, "any");
    ff_num(r, "Lon", "simLon", c->sim_lon, "any");
    ff_num(r, "Track", "simTrk", c->sim_trk, "any");
    ff_num(r, "GS (kt)", "simGs", c->sim_gs, "any");
    ff_num(r, "Alt (ft)", "simAlt", c->sim_alt, "any");
    chunk(r, "</div></div>");

    // (6) Security
    chunk(r, "<div class=card><h3>\xF0\x9F\x94\x92 Security</h3><div class=grid>");
    ff_tog(r, "Require login", "authEnable", c->auth_enable);
    ff_text(r, "User", "authUser", c->auth_user, "text", false);
    ff_text(r, "New password (blank=keep)", "authPass", "", "password", false);
    chunk(r, "</div></div>");

    chunk(r, "<div class=bar><button class=save>Save &amp; reboot</button>"
             "<button class=ghost type=button onclick='location.reload()'>Reload</button></div>");
    chunk(r, "</form></div>");
    httpd_resp_sendstr_chunk(r, NULL);
    return ESP_OK;
}

// ---------- save ----------
static bool fld(const char *body, const char *k, char *out, size_t n) { return web_form_field(body, k, out, n); }

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
    if (fld(body, "staIp", v, sizeof v)) strlcpy(c->sta_ip, v, sizeof c->sta_ip);
    if (fld(body, "staGw", v, sizeof v)) strlcpy(c->sta_gw, v, sizeof c->sta_gw);
    if (fld(body, "staMask", v, sizeof v)) strlcpy(c->sta_mask, v, sizeof c->sta_mask);
    if (fld(body, "staDns", v, sizeof v)) strlcpy(c->sta_dns, v, sizeof c->sta_dns);
    if (fld(body, "apSsid", v, sizeof v)) strlcpy(c->ap_ssid, v, sizeof c->ap_ssid);
    if (fld(body, "apPass", v, sizeof v)) strlcpy(c->ap_pass, v, sizeof c->ap_pass);
    c->ap_hidden = fld(body, "apHidden", v, sizeof v);
    if (fld(body, "apIp", v, sizeof v)) { unsigned a,b,cc,d; if (sscanf(v,"%u.%u.%u.%u",&a,&b,&cc,&d)==4){c->ap_ip[0]=a;c->ap_ip[1]=b;c->ap_ip[2]=cc;c->ap_ip[3]=d;} }
    if (fld(body, "apPrefix", v, sizeof v)) c->ap_prefix = atoi(v);
    if (fld(body, "apDhcpCount", v, sizeof v)) c->ap_dhcp_count = atoi(v);
    if (fld(body, "apLeaseMin", v, sizeof v)) c->ap_lease_min = atoi(v);
    if (fld(body, "apClientDns", v, sizeof v)) strlcpy(c->ap_client_dns, v, sizeof c->ap_client_dns);
    if (fld(body, "apChannel", v, sizeof v)) c->ap_channel = atoi(v);
    if (fld(body, "apMaxClients", v, sizeof v)) c->ap_max_clients = atoi(v);
    if (fld(body, "adbpPort", v, sizeof v)) c->adbp_port = atoi(v);
    if (fld(body, "dsPort", v, sizeof v)) c->ds_port = atoi(v);
    c->napt_enable = fld(body, "napt", v, sizeof v);
    if (fld(body, "devName", v, sizeof v)) strlcpy(c->dev_name, v, sizeof c->dev_name);
    if (fld(body, "acTail", v, sizeof v)) strlcpy(c->ac_tail, v, sizeof c->ac_tail);
    if (fld(body, "acType", v, sizeof v)) strlcpy(c->ac_type, v, sizeof c->ac_type);
    if (fld(body, "apiVer", v, sizeof v)) strlcpy(c->api_ver, v, sizeof c->api_ver);
    if (fld(body, "srcType", v, sizeof v)) { int s = atoi(v); c->src_type = s < 0 ? 0 : s > 2 ? 2 : s; }
    if (fld(body, "vsUrl", v, sizeof v)) strlcpy(c->vs_url, v, sizeof c->vs_url);
    if (fld(body, "pollMs", v, sizeof v)) { uint32_t p = strtoul(v,0,10); c->poll_ms = p<250?250:p>60000?60000:p; }
    if (fld(body, "staleMs", v, sizeof v)) { uint32_t p = strtoul(v,0,10); c->stale_ms = p<1000?1000:p; }
    c->sim_enable = fld(body, "simEnable", v, sizeof v);
    if (fld(body, "simLat", v, sizeof v)) c->sim_lat = atof(v);
    if (fld(body, "simLon", v, sizeof v)) c->sim_lon = atof(v);
    if (fld(body, "simTrk", v, sizeof v)) c->sim_trk = atof(v);
    if (fld(body, "simGs", v, sizeof v)) c->sim_gs = atof(v);
    if (fld(body, "simAlt", v, sizeof v)) c->sim_alt = atof(v);
    c->auth_enable = fld(body, "authEnable", v, sizeof v);
    if (fld(body, "authUser", v, sizeof v)) strlcpy(c->auth_user, v, sizeof c->auth_user);
    if (fld(body, "authPass", v, sizeof v) && v[0]) {   // new password -> re-salt + hash, drop sessions
        auth_rand_hex(c->auth_salt, 8);
        auth_hash(c->auth_salt, v, c->auth_hash);
        auth_reset_session();
    }
    // clamps
    if (c->ap_max_clients < 1) c->ap_max_clients = 1;
    if (c->ap_max_clients > 10) c->ap_max_clients = 10;
    if (c->ap_channel > 13) c->ap_channel = 0;
    if (c->ap_dhcp_count < 1) c->ap_dhcp_count = 1;
    if (c->ap_dhcp_count > 62) c->ap_dhcp_count = 62;
    if (c->ap_lease_min < 1) c->ap_lease_min = 1;
    free(body);

    cfg_save(c);
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_sendstr(r, "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content='6;url=/'>"
                          "<body style='background:#070b14;color:#eaf2ff;font-family:system-ui;text-align:center;padding-top:18vh'>"
                          "<h2 style='color:#22d3ee'>Saved \xE2\x9C\x93</h2><p>Rebooting to apply\xE2\x80\xA6</p>");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

// ---------- JSON status / clients / scan ----------
static esp_err_t h_status(httpd_req_t *r) {
    if (!gate(r)) return ESP_OK;
    uint8_t sip[4] = {0}; bool up = netcore_sta_up(sip);
    char ipbuf[16] = "";
    if (up) snprintf(ipbuf, sizeof ipbuf, "%u.%u.%u.%u", sip[0], sip[1], sip[2], sip[3]);
    char json[512];
    // position fields are placeholders until the M4 poller is wired in.
    snprintf(json, sizeof json,
        "{\"sta\":%s,\"ssid\":\"%s\",\"clients\":%d,\"valid\":false,\"sim\":%s,"
        "\"lat\":0,\"lon\":0,\"trk\":0,\"gs\":0,\"alt\":0,\"staip\":\"%s\","
        "\"pollok\":false,\"pollage\":-1,\"pollmsg\":\"poller not yet ported (M4)\"}",
        up ? "true" : "false", CFG->sta_ssid, netcore_ap_client_count(),
        CFG->sim_enable ? "true" : "false", ipbuf);
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
        chunkf(r, "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"ip\":\"\",\"rssi\":%d}",
               i ? "," : "", s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5], s->rssi);
    }
    chunk(r, "]");
    httpd_resp_sendstr_chunk(r, NULL);
    return ESP_OK;
}

static esp_err_t h_scan(httpd_req_t *r) {
    if (!gate(r)) return ESP_OK;
    httpd_resp_set_type(r, "application/json");
    wifi_scan_config_t sc = { .show_hidden = true };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) { httpd_resp_sendstr(r, "[]"); return ESP_OK; }
    uint16_t n = 0; esp_wifi_scan_get_ap_num(&n);
    if (n > 48) n = 48;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (recs) esp_wifi_scan_get_ap_records(&n, recs);
    chunk(r, "[");
    for (int i = 0; recs && i < n; i++) {
        char ssid[100]; web_html_esc(ssid, sizeof ssid, (char *)recs[i].ssid);   // reuse escaper (also JSON-safe for our chars)
        chunkf(r, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"enc\":%s}",
               i ? "," : "", (char *)recs[i].ssid, recs[i].rssi,
               recs[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
        (void)ssid;
    }
    free(recs);
    chunk(r, "]");
    httpd_resp_sendstr_chunk(r, NULL);
    return ESP_OK;
}

// ---------- AID Web API (XML, public) ----------
static void api_resp(httpd_req_t *r, const char *cmd, const char *inner) {
    char buf[512];
    time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm);
    char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    snprintf(buf, sizeof buf,
        "<?xml version=\"1.0\"?><Response commandName=\"%s\" returnCode=\"0\" timestamp=\"%s\">%s</Response>",
        cmd, ts, inner);
    httpd_resp_set_type(r, "text/xml");
    httpd_resp_sendstr(r, buf);
}
static esp_err_t h_api_ver(httpd_req_t *r) { char x[80]; snprintf(x,sizeof x,"<APIVersion APIVersion=\"%s\"/>", CFG->api_ver); api_resp(r,"getAPIVersion",x); return ESP_OK; }
static esp_err_t h_api_wifi(httpd_req_t *r) { api_resp(r,"getWiFiAPStatus","<WiFiAP WiFiAPStatus=\"ACTIVE\"/>"); return ESP_OK; }
static esp_err_t h_api_aoip(httpd_req_t *r) { api_resp(r,"getAoIPStatus","<AoIPStatus AoIPAvailability=\"DISABLED\" ATSUStatus=\"NORMAL\" IpLinkStatus=\"DISCONNECTED\" VPNUsed=\"AoIP-VPN\" AoIPServerFQDN=\"\" AoIPServerIP=\"\"><ListOfAuthorizedChannels/></AoIPStatus>"); return ESP_OK; }
static esp_err_t h_api_acars(httpd_req_t *r) { api_resp(r,"getAcarsStatus","<AcarsStatus ATSUStatus=\"NORMAL\" AcarsForEFBAvailability=\"DISABLED\" AcarsLinkStatus=\"DISCONNECTED\"/>"); return ESP_OK; }
static esp_err_t h_api_reboot(httpd_req_t *r) { api_resp(r,"cmdReboot",""); return ESP_OK; }

static esp_err_t h_404(httpd_req_t *r, httpd_err_code_t e) {
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
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 20;
    hc.stack_size = 8192;
    hc.lru_purge_enable = true;
    if (httpd_start(&s_http, &hc) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }
    reg("/", HTTP_GET, h_root);
    reg("/status", HTTP_GET, h_status);
    reg("/clients", HTTP_GET, h_clients);
    reg("/scan", HTTP_GET, h_scan);
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
    ESP_LOGI(TAG, "[WEB] http://%u.%u.%u.%u/", cfg->ap_ip[0], cfg->ap_ip[1], cfg->ap_ip[2], cfg->ap_ip[3]);
}
