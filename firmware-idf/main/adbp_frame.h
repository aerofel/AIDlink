// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Pure ADBP frame/XML builders — no sockets, no ESP-IDF; host-unit-testable.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "pos.h"

#define ADBP_MAXNAME 40
#define ADBP_MAXPARAMS 64

// XML-escape src into dst (bounded). Returns dst.
char *adbp_xml_esc(char *dst, size_t cap, const char *src);

// Advance p->lat/lon along p->track_deg by (gs_kt * dt_s) — consumer-side
// dead-reckoning. No-op unless valid, not fixed, has track, sane GS.
void adbp_dead_reckon(pos_state_t *p, double dt_s);

// Parse <parameter name="X"> names from an ADBP request into names[][ADBP_MAXNAME].
// Returns count (<= maxn). Ignores the <method name="..."> tag.
int adbp_parse_params(const char *req, char names[][ADBP_MAXNAME], int maxn);

// Read the integer inside <tag>N</tag>; returns dflt if absent.
long adbp_tag_num(const char *req, const char *tag, long dflt);

// Build <parameters>…</parameters> for the requested names. fresh = valid and
// within the stale window; stamp_ms = value for the time="" attribute. Sets
// *miss true if any requested name was unknown. Returns bytes written.
int adbp_params_block(char *out, size_t cap, char names[][ADBP_MAXNAME], int n,
                      const pos_state_t *p, const aidlink_cfg_t *cfg,
                      bool fresh, uint64_t stamp_ms, bool *miss);

// Sync response: <?xml…?>\n<response method="M" errorcode="E">BODY</response>.
int adbp_wrap_resp(char *out, size_t cap, const char *method, int errorcode, const char *body);

// Push frame honoring cfg->frame_len (0 full / 1 method-element / 2 omit) and
// with_prolog. Returns bytes written (delimiter is appended by the caller).
int adbp_wrap_push(char *out, size_t cap, const char *method, const char *body,
                   bool with_prolog, const aidlink_cfg_t *cfg);
