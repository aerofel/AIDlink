// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Web config portal (esp_http_server). Public entry + pure helpers (the helpers
// are IDF-free so they can be host-unit-tested).
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "config.h"

// Start the HTTP config server on :80. Non-fatal on failure (logs).
void web_start(aidlink_cfg_t *cfg);

// --- pure helpers (host-testable) ---

// HTML-escape src into dst (NUL-terminated, never overflows dstsz). Escapes
// & < > " '. Returns dst.
char *web_html_esc(char *dst, size_t dstsz, const char *src);

// Extract cookie value `name` from a Cookie header into out (NUL-terminated).
// Returns true if found. Handles "a=1; NAME=val; b=2" with optional spaces.
bool web_cookie_val(const char *cookie_hdr, const char *name, char *out, size_t outsz);

// URL-decode src (application/x-www-form-urlencoded: %XX and '+'->space) into
// dst (NUL-terminated, bounded by dstsz). Returns dst.
char *web_url_decode(char *dst, size_t dstsz, const char *src);

// Find key in a urlencoded body ("a=1&b=2"), URL-decode its value into out.
// Returns true if the key was present.
bool web_form_field(const char *body, const char *key, char *out, size_t outsz);
