// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
#include "auth.h"
#include "web.h"
#include <string.h>
#include <stdio.h>
#include "mbedtls/sha256.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"

#define COOKIE_NAME "AIDSESS"

static const char *TAG = "auth";
static char     s_tok[33];        // 32 hex + NUL; "" = no session
static bool     s_remember;

void auth_hash(const char *salt, const char *password, char out64[65]) {
    char in[160];
    snprintf(in, sizeof in, "%s:%s", salt ? salt : "", password ? password : "");
    uint8_t d[32];
    mbedtls_sha256((const unsigned char *)in, strlen(in), d, 0 /* SHA-256 */);
    for (int i = 0; i < 32; i++) snprintf(out64 + i * 2, 3, "%02x", d[i]);
    out64[64] = 0;
}

void auth_rand_hex(char *out, int nbytes) {
    for (int i = 0; i < nbytes; i++) snprintf(out + i * 2, 3, "%02x", (unsigned)(esp_random() & 0xFF));
    out[nbytes * 2] = 0;
}

void auth_init(void) {
    nvs_handle_t h;
    if (nvs_open("aidlink", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof s_tok;
        if (nvs_get_str(h, "auth_tok", s_tok, &len) == ESP_OK && s_tok[0]) {
            s_remember = true;
        }
        nvs_close(h);
    }
}

static void persist_tok(const char *tok) {
    nvs_handle_t h;
    if (nvs_open("aidlink", NVS_READWRITE, &h) != ESP_OK) return;
    if (tok && tok[0]) nvs_set_str(h, "auth_tok", tok);
    else nvs_erase_key(h, "auth_tok");
    nvs_commit(h);
    nvs_close(h);
}

bool auth_ok(const aidlink_cfg_t *c, const char *cookie_hdr) {
    if (!c->auth_enable || c->auth_hash[0] == 0) return true;   // auth off/unconfigured -> open
    if (s_tok[0] == 0) return false;
    char tok[33];
    if (!cookie_hdr || !web_cookie_val(cookie_hdr, COOKIE_NAME, tok, sizeof tok)) return false;
    if (strcmp(tok, s_tok) != 0) return false;
    // No server-side idle timeout: the session is valid until logout / password
    // change / a new login. The previous 30-min idle (plus a fragile session
    // cookie iOS Safari drops on app-switch) forced constant re-logins.
    return true;
}

bool auth_login(const aidlink_cfg_t *c, const char *user, const char *pass,
                bool remember, char *set_cookie, size_t sccap) {
    if (!c->auth_enable || c->auth_hash[0] == 0) return false;
    if (!user || strcmp(user, c->auth_user) != 0) return false;
    char h[65];
    auth_hash(c->auth_salt, pass ? pass : "", h);
    if (strcmp(h, c->auth_hash) != 0) return false;

    auth_rand_hex(s_tok, 16);
    s_remember = remember;
    persist_tok(s_tok);   // always persist so the session survives a reboot

    // Always give the cookie a Max-Age (a bare session cookie gets dropped by iOS
    // Safari when you switch to the EFB app). "Stay connected" just extends it.
    long max_age = remember ? 2592000L : 604800L;   // 30 days vs 7 days
    snprintf(set_cookie, sccap,
             COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%ld", s_tok, max_age);
    ESP_LOGI(TAG, "login ok (remember=%d)", remember);
    return true;
}

void auth_logout(char *set_cookie, size_t sccap) {
    s_tok[0] = 0; s_remember = false;
    persist_tok("");
    snprintf(set_cookie, sccap, COOKIE_NAME "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
}

void auth_reset_session(void) {
    s_tok[0] = 0; s_remember = false;
    persist_tok("");
}
