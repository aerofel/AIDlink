// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Pure web helpers — no ESP-IDF deps, host-unit-testable.
#include "web.h"
#include <string.h>

char *web_html_esc(char *dst, size_t dstsz, const char *src) {
    size_t o = 0;
    for (const char *p = src ? src : ""; *p && o + 1 < dstsz; p++) {
        const char *rep = NULL;
        switch (*p) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (o + rl + 1 > dstsz) break;
            memcpy(dst + o, rep, rl); o += rl;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = 0;
    return dst;
}

bool web_cookie_val(const char *cookie_hdr, const char *name, char *out, size_t outsz) {
    if (!cookie_hdr || !name || outsz == 0) return false;
    size_t nlen = strlen(name);
    const char *p = cookie_hdr;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;       // skip separators/spaces
        if (!*p) break;
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            size_t o = 0;
            while (*v && *v != ';' && o + 1 < outsz) out[o++] = *v++;
            out[o] = 0;
            return true;
        }
        while (*p && *p != ';') p++;              // advance to next cookie
    }
    return false;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *web_url_decode(char *dst, size_t dstsz, const char *src) {
    size_t o = 0;
    for (const char *p = src ? src : ""; *p && o + 1 < dstsz; p++) {
        if (*p == '+') {
            dst[o++] = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            int hi = hexv(p[1]), lo = hexv(p[2]);
            if (hi >= 0 && lo >= 0) { dst[o++] = (char)((hi << 4) | lo); p += 2; }
            else dst[o++] = *p;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = 0;
    return dst;
}

bool web_form_field(const char *body, const char *key, char *out, size_t outsz) {
    if (!body || !key || outsz == 0) return false;
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            // copy raw value into a temp, then URL-decode
            char raw[512];
            size_t rl = end - (p + klen + 1);
            if (rl >= sizeof raw) rl = sizeof raw - 1;
            memcpy(raw, p + klen + 1, rl); raw[rl] = 0;
            web_url_decode(out, outsz, raw);
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}
