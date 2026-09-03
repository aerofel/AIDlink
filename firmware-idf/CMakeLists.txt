# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AIDlink contributors
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ---------------------------------------------------------------------------
# NAPT ageing timers. esp-lwip's defaults (lwip_napt.h) assume a LAN; they are
# actively wrong for a high-RTT satellite uplink and there is no Kconfig for
# them, so they are overridden here (each is #ifndef-guarded upstream).
#
# UDP was 2 s. That is SHORTER THAN ONE ROUND TRIP on this link (the Viasat
# poll in netcore.c measures 1-10 s), so a mapping could expire before the
# reply arrived. Every UDP flow with a gap of more than 2 s lost its
# translation: QUIC/HTTP3 on UDP 443 (Chrome and Safari default to it) and any
# VPN keepalive (WireGuard's is 25 s) were being torn down continuously, then
# retried, then torn down again. RFC 4787 REQ-5 sets the floor at 2 minutes.
#
# ICMP was also 2 s, which silently caps how slow a ping can be before the
# reply is discarded as unmapped -- misleading exactly when diagnosing a slow
# link. 10 s matches the observed worst-case RTT.
#
# TCP was 30 min of idle retention in a 512-entry table (IP_NAPT_MAX, left
# alone: raising it costs internal SRAM on the non-PSRAM ESP32 board, which
# LEARNING.md already records as starved). Dead entries held slots long enough
# for a browsing session to exhaust the table, after which NEW connections
# fail while ping and established flows keep working. 10 min still comfortably
# outlives a real idle keep-alive while letting the table actually recycle.
idf_build_set_property(COMPILE_DEFINITIONS "IP_NAPT_TIMEOUT_MS_UDP=120000" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "IP_NAPT_TIMEOUT_MS_ICMP=10000" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "IP_NAPT_TIMEOUT_MS_TCP=600000" APPEND)
# ---------------------------------------------------------------------------

project(aidlink)
