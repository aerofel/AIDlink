// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Custom lwIP hooks, compiled into the lwip component only (see
// ESP_IDF_LWIP_HOOK_FILENAME in main/CMakeLists.txt).
//
// DHCP server reply counter (2026-11-02): a phone looping join -> leave
// (reason 8) -> rejoin and ending on 169.254.x.x left no trace in the flash
// log, because the IDF DHCP server logs nothing. Counting the replies it
// builds (OFFER / ACK / NAK) tells "the server never heard the DISCOVER" apart
// from "we answered but the phone never got it". Runs in the tcpip thread:
// netcore.c's implementation only bumps counters.
#pragma once

// msg_type is the RFC 2132 option-53 value: 2 OFFER, 5 ACK, 6 NAK.
void aid_dhcps_reply_hook(int msg_type);

#define LWIP_HOOK_DHCPS_POST_APPEND_OPTS(netif, dhcps, state, pp_opts) \
    aid_dhcps_reply_hook(state);
