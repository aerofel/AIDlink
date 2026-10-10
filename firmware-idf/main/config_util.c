// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Pure config helpers — no ESP-IDF dependencies, host-unit-testable.
#include "config.h"
#include <stdio.h>
#include <string.h>

uint32_t cfg_netmask_from_prefix(uint8_t prefix) {
    if (prefix == 0) return 0u;
    if (prefix >= 32) return 0xFFFFFFFFu;
    return 0xFFFFFFFFu << (32 - prefix);
}

// Dotted quad -> host-order u32. False on anything else.
static bool ip_parse(const char *s, uint32_t *out) {
    unsigned a, b, c, d; char tail;
    if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

static int problem(char *err, size_t cap, int n, const char *msg) {
    size_t o = strlen(err);
    if (o + strlen(msg) + 3 < cap) snprintf(err + o, cap - o, "%s%s", o ? "; " : "", msg);
    return n + 1;
}

int cfg_validate(const aidlink_cfg_t *c, char *err, size_t cap) {
    int n = 0;
    if (cap) err[0] = 0;
    if (!c->ap_ssid[0]) n = problem(err, cap, n, "AP SSID is empty");
    size_t pl = strlen(c->ap_pass);
    if (pl < 8 || pl > 63) n = problem(err, cap, n, "AP passphrase must be 8-63 characters (a shorter one opens the AP to everyone)");

    uint32_t ip, mask, lease;
    bool ok_ip = ip_parse(c->ap_ip, &ip), ok_mask = ip_parse(c->ap_mask, &mask),
         ok_lease = ip_parse(c->ap_lease, &lease);
    if (!ok_ip)    n = problem(err, cap, n, "AP IP is not a valid address");
    if (!ok_mask)  n = problem(err, cap, n, "AP netmask is not a valid mask");
    if (!ok_lease) n = problem(err, cap, n, "DHCP pool start is not a valid address");
    if (ok_ip && ok_mask && ok_lease) {
        uint32_t host = ~mask;
        bool contiguous = ((host + 1) & host) == 0;     // 0..01..1
        if (!contiguous || host < 3 || host > 0xFFFF) {
            n = problem(err, cap, n, "AP netmask must be contiguous, /16 to /30");
        } else {
            uint32_t net = ip & mask, bcast = net | host;
            int count = c->ap_dhcp_count;
            if (ip == net || ip == bcast)
                n = problem(err, cap, n, "AP IP is the subnet's network or broadcast address");
            if ((lease & mask) != net)
                n = problem(err, cap, n, "DHCP pool start is outside the AP subnet");
            else if (lease == net || count < 1 || lease + (uint32_t)count - 1 >= bcast)
                n = problem(err, cap, n, "DHCP pool does not fit inside the AP subnet");
            else if (ip >= lease && ip <= lease + (uint32_t)count - 1)
                n = problem(err, cap, n, "AP IP lies inside the DHCP pool");
        }
    }
    if (c->ap_client_dns[0]) {
        uint32_t d;
        if (!ip_parse(c->ap_client_dns, &d)) n = problem(err, cap, n, "Client DNS is not a valid address");
    }
    // (the 1-65535 text range is checked where the form is parsed: these are u16)
    if (c->adbp_port == 0 || c->adbp_port == 53 || c->adbp_port == 80 ||
        c->adbp_port == 67 || c->adbp_port == 5353)
        n = problem(err, cap, n, "ADBP port must be 1-65535 and not 53/67/80/5353");
    if (c->ds_port == 0) n = problem(err, cap, n, "EFB publish port must be 1-65535");
    if (c->stale_ms < 5000 || c->stale_ms > 60000)
        n = problem(err, cap, n, "Stale timeout must be 5-60 s");
    if (c->dr_hold_ms > 600000)
        n = problem(err, cap, n, "Dead-reckoning hold must be 0-600 s");
    if (!c->dev_name[0]) n = problem(err, cap, n, "Device name is empty");
    if (c->auth_enable && !c->auth_user[0]) n = problem(err, cap, n, "Login user is empty");
    return n;
}
