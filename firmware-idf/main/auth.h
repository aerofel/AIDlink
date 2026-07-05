// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Settings-portal authentication: salted SHA-256 credentials + a single
// server-side session token (mirrors the v9 Arduino scheme).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

// hex(SHA-256(salt ":" password)) -> out64 (must hold 65 bytes).
void auth_hash(const char *salt, const char *password, char out64[65]);

// n random bytes as lowercase hex -> out (needs 2*n+1 bytes).
void auth_rand_hex(char *out, int nbytes);

// Restore any persisted "remember me" session token at boot.
void auth_init(void);

// True if auth is disabled/unconfigured, or the request's cookie matches the
// active session. cookie_hdr may be NULL. Refreshes idle timer on success.
bool auth_ok(const aidlink_cfg_t *c, const char *cookie_hdr);

// Validate credentials; on success mint a session and return the Set-Cookie
// value into set_cookie (>=160 bytes). remember persists across reboots.
// Returns true on success.
bool auth_login(const aidlink_cfg_t *c, const char *user, const char *pass,
                bool remember, char *set_cookie, size_t sccap);

// Clear the active session; returns an expiring Set-Cookie into set_cookie.
void auth_logout(char *set_cookie, size_t sccap);

// Session invalidation (e.g. after a password change).
void auth_reset_session(void);
