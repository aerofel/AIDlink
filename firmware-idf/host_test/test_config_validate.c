// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
//   clang -Imain -o /tmp/t host_test/test_config_validate.c main/config_util.c && /tmp/t
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config.h"

static aidlink_cfg_t good(void) {
    aidlink_cfg_t c; memset(&c, 0, sizeof c);
    strcpy(c.ap_ssid, "AIDFEEL"); strcpy(c.ap_pass, "88888888");
    strcpy(c.ap_ip, "172.20.1.1"); strcpy(c.ap_mask, "255.255.255.192");
    strcpy(c.ap_lease, "172.20.1.2"); c.ap_dhcp_count = 60;
    c.adbp_port = 24000; c.ds_port = 51000;
    c.stale_ms = 30000; c.dr_hold_ms = 300000;
    strcpy(c.dev_name, "aidlink"); c.auth_enable = true; strcpy(c.auth_user, "admin");
    return c;
}

#define BAD(expr_setup, needle) do { aidlink_cfg_t c = good(); expr_setup; char e[512]; \
    int n = cfg_validate(&c, e, sizeof e); \
    if (!(n >= 1 && strstr(e, needle))) { printf("FAIL %s: n=%d err='%s'\n", #expr_setup, n, e); return 1; } } while (0)

int main(void) {
    char e[512];
    aidlink_cfg_t c = good();
    assert(cfg_validate(&c, e, sizeof e) == 0 && e[0] == 0);   // the shipped defaults pass

    BAD(strcpy(c.ap_pass, "1234567"), "passphrase");             // would open the AP
    BAD(c.ap_pass[0] = 0, "passphrase");
    BAD(c.ap_ssid[0] = 0, "SSID");
    BAD(strcpy(c.ap_ip, "172.20.1"), "AP IP");
    BAD(strcpy(c.ap_mask, "255.0.255.0"), "contiguous");
    BAD(strcpy(c.ap_mask, "255.255.255.254"), "contiguous");      // /31: no hosts
    BAD(strcpy(c.ap_ip, "172.20.1.0"), "network or broadcast");
    BAD(strcpy(c.ap_ip, "172.20.1.63"), "network or broadcast");
    BAD(strcpy(c.ap_lease, "172.20.2.2"), "outside");
    BAD(c.ap_dhcp_count = 62, "does not fit");                    // .2+62-1 = .63 = broadcast
    BAD((strcpy(c.ap_ip, "172.20.1.10"), 0), "inside the DHCP pool");
    BAD(strcpy(c.ap_client_dns, "8.8.8"), "Client DNS");
    BAD(c.adbp_port = 0, "ADBP port");                            // empty field -> atoi 0
    BAD(c.adbp_port = 80, "ADBP port");
    BAD(c.ds_port = 0, "publish port");
    BAD(c.stale_ms = 1000, "Stale");
    BAD(c.stale_ms = 3600000, "Stale");
    BAD(c.dr_hold_ms = 3600000, "hold");
    BAD(c.dev_name[0] = 0, "Device name");
    BAD(c.auth_user[0] = 0, "Login user");

    // several problems are all reported, not just the first
    c = good(); c.ap_pass[0] = 0; c.adbp_port = 0; c.stale_ms = 0;
    assert(cfg_validate(&c, e, sizeof e) == 3);
    // a /24 with a larger pool is fine
    c = good(); strcpy(c.ap_mask, "255.255.255.0"); c.ap_dhcp_count = 200;
    assert(cfg_validate(&c, e, sizeof e) == 0);
    // login disabled: empty user allowed
    c = good(); c.auth_enable = false; c.auth_user[0] = 0;
    assert(cfg_validate(&c, e, sizeof e) == 0);

    puts("test_config_validate: all OK");
    return 0;
}
