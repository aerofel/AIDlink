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

// Position freshness as reported to the EFB (spec 2026-08-13):
//   FRESH — valid fix inside the stale window; stock quality figures.
//   DR    — fix older than stale_ms but inside the dead-reckoning hold; the
//           retained lat/lon are extrapolated, FOM/HDOP degrade with age.
//   NCD   — no fix, or hold expired; positional params go validity="2".
typedef enum { ADBP_POS_FRESH = 0, ADBP_POS_DR = 1, ADBP_POS_NCD = 2 } adbp_pstate_t;

// Classify freshness from the fix age and the two config windows (pure).
// last_fix_ms == 0 means "never had a fix" and is always NCD.
adbp_pstate_t adbp_classify(bool valid, uint32_t last_fix_ms, uint32_t now_ms,
                            uint32_t stale_ms, uint32_t dr_hold_ms);

// Push-due decision for one subscription (pure). A state transition is due
// immediately (the EFB is told about DR/NCD instead of left to time out);
// on-event subs additionally fall back to their period while the position is
// still reportable (fresh or DR) but the fix sequence is frozen — without
// this an on-event EFB goes silent at the first missed poll.
bool adbp_push_due(bool on_event, uint32_t cur_fix, uint32_t last_fix_seen,
                   uint32_t now_ms, uint32_t last_push_ms, uint32_t period_ms,
                   adbp_pstate_t st, adbp_pstate_t prev_st);

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

// Build <parameters>…</parameters> for the requested names. state selects the
// freshness behavior (see adbp_pstate_t); age_s = fix age in seconds, used to
// degrade FOM/HDOP during the DR hold; stamp_ms = value for the time=""
// attribute. Sets *miss true if any requested name was unknown. Returns bytes.
int adbp_params_block(char *out, size_t cap, char names[][ADBP_MAXNAME], int n,
                      const pos_state_t *p, const aidlink_cfg_t *cfg,
                      adbp_pstate_t state, uint32_t age_s, uint64_t stamp_ms, bool *miss);

// Sync response: <?xml…?>\n<response method="M" errorcode="E">BODY</response>.
int adbp_wrap_resp(char *out, size_t cap, const char *method, int errorcode, const char *body);

// Push frame honoring cfg->frame_len (0 full / 1 method-element / 2 omit) and
// with_prolog. Returns bytes written (delimiter is appended by the caller).
int adbp_wrap_push(char *out, size_t cap, const char *method, const char *body,
                   bool with_prolog, const aidlink_cfg_t *cfg);
