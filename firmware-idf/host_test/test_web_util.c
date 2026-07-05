// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
// Host unit test for the pure web helpers (build with clang, no ESP-IDF).
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "web.h"

int main(void) {
    char buf[256];

    // --- HTML escape ---
    web_html_esc(buf, sizeof buf, "a<b>&\"'");
    assert(strcmp(buf, "a&lt;b&gt;&amp;&quot;&#39;") == 0);
    web_html_esc(buf, sizeof buf, "plain");
    assert(strcmp(buf, "plain") == 0);

    // --- cookie parsing ---
    assert(web_cookie_val("AIDSESS=abc123", "AIDSESS", buf, sizeof buf) && !strcmp(buf, "abc123"));
    assert(web_cookie_val("foo=1; AIDSESS=xy; bar=2", "AIDSESS", buf, sizeof buf) && !strcmp(buf, "xy"));
    assert(web_cookie_val("foo=1;AIDSESS=zz", "AIDSESS", buf, sizeof buf) && !strcmp(buf, "zz"));
    assert(!web_cookie_val("foo=1; bar=2", "AIDSESS", buf, sizeof buf));
    // must not match a suffix/prefix collision (SESS vs AIDSESS)
    assert(!web_cookie_val("XAIDSESS=nope", "AIDSESS", buf, sizeof buf));

    // --- url decode ---
    web_url_decode(buf, sizeof buf, "hello+world%21");
    assert(strcmp(buf, "hello world!") == 0);
    web_url_decode(buf, sizeof buf, "a%2Bb");
    assert(strcmp(buf, "a+b") == 0);

    // --- form field extraction ---
    assert(web_form_field("u=admin&p=pass%20word&remember=1", "p", buf, sizeof buf) && !strcmp(buf, "pass word"));
    assert(web_form_field("u=admin&p=x", "u", buf, sizeof buf) && !strcmp(buf, "admin"));
    assert(!web_form_field("u=admin&p=x", "zzz", buf, sizeof buf));
    // key that is a substring of another key must not false-match
    assert(web_form_field("ssid=Net&ssidx=bad", "ssid", buf, sizeof buf) && !strcmp(buf, "Net"));

    printf("test_web_util: PASS\n");
    return 0;
}
