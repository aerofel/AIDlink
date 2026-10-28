// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// ADC side of the battery gauge: oneshot reads on the board's sense pin,
// curve-fitting calibration, divider undone, handed to batgauge.c; the
// remembered facts (battery seen, capacity, typical drain) live in NVS.
#include "battery.h"
#include "board.h"
#include "log.h"
#include <string.h>
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "tusb.h"

static const char *TAG = "bat";
#define NVS_NS "battery"

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static bool s_ok;

// owned by the polling task
static batgauge_t s_g;
static batstats_t s_st;
static bool s_cold;             // this boot was a power-on reset
static int  s_ticks;
static int64_t s_rate_saved_t;

// shared: written by the polling task / portal, read by everyone
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static battery_info_t s_pub;
static bool s_seen;
static int  s_cap, s_pph10 = -1;

static void nvs_put(const char *k, int32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, k, v);
    nvs_commit(h);
    nvs_close(h);
}

static int32_t nvs_take(nvs_handle_t h, const char *k, int32_t def)
{
    int32_t v;
    return nvs_get_i32(h, k, &v) == ESP_OK ? v : def;
}

void battery_start(void)
{
    int gpio = board_get()->bat_adc_gpio;
    if (gpio < 0 || s_ok) return;
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(gpio, &unit, &s_chan) != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGW(TAG, "GPIO%d is not an ADC1 pin — gauge disabled", gpio);
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) return;
    // 12 dB: full scale ~3.1 V; the 4.7 V USB rail is 2.37 V at the pin
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_adc, s_chan, &ccfg) != ESP_OK) return;
    adc_cali_curve_fitting_config_t cc = {
        .unit_id = ADC_UNIT_1, .chan = s_chan,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cc, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no eFuse ADC calibration — gauge disabled");
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        s_seen  = nvs_take(h, "seen", 0) != 0;
        s_cap   = nvs_take(h, "mah", 0);
        s_pph10 = nvs_take(h, "pph10", -1);
        nvs_close(h);
    }
    s_cold = esp_reset_reason() == ESP_RST_POWERON;
    batgauge_init(&s_g);
    batstats_init(&s_st);
    s_pub.supported = true;
    s_pub.g.pct = -1;
    s_ok = true;
    logln("[bat] gauge on GPIO%d (ADC1 ch%d), battery %s, capacity %d mAh",
          gpio, (int)s_chan, s_seen ? "remembered" : "not seen yet", s_cap);
}

bool battery_supported(void) { return s_ok; }

static void set_seen(bool v, const char *why)
{
    portENTER_CRITICAL(&s_mux);
    s_seen = v;
    portEXIT_CRITICAL(&s_mux);
    nvs_put("seen", v);
    logln("[bat] battery %s (%s)", v ? "fitted" : "not fitted", why);
}

// Once a second: learned flags, statistics, published snapshot.
static void second_tick(void)
{
    batg_state_t st = s_g.state;
    int64_t now = esp_timer_get_time() / 1000000;

    if (st != BATG_UNKNOWN && s_cold) {
        s_cold = false;
        // Cold boot straight onto USB: a fitted, charged cell would have kept
        // the board running, so whatever was remembered is stale.
        if (st == BATG_ON_USB && s_seen) set_seen(false, "cold boot on USB");
    }
    if (st == BATG_ON_BATTERY && !s_seen) set_seen(true, "running on battery");

    int cap, pph10;
    bool seen;
    portENTER_CRITICAL(&s_mux);
    cap = s_cap; pph10 = s_pph10; seen = s_seen;
    portEXIT_CRITICAL(&s_mux);

    batstats_out_t so;
    batstats_tick(&s_st, now, st, batgauge_cell_mv(&s_g), s_g.med_mv, cap, pph10, &so);

    // Remember a measured drain (every 15 min of a long enough episode) so the
    // portal can quote an autonomy while on USB and early in the next episode.
    if (so.rate_live && so.episode_s >= 900 && now - s_rate_saved_t >= 900) {
        s_rate_saved_t = now;
        portENTER_CRITICAL(&s_mux);
        s_pph10 = so.rate_pph10;
        portEXIT_CRITICAL(&s_mux);
        nvs_put("pph10", so.rate_pph10);
    }

    portENTER_CRITICAL(&s_mux);
    battery_t *b = &s_pub.g;
    b->present = seen;
    b->mv = s_g.med_mv;
    b->ripple_mv = s_g.ripple_mv;
    if (st == BATG_ON_BATTERY) {
        b->ext_power = b->charging = false;
        b->pct = s_g.pct;
    } else if (st == BATG_ON_USB) {
        b->ext_power = true;
        b->charging = seen;
        b->pct = -1;                     // the cell is invisible on USB
    }                                    // UNKNOWN: keep the last picture
    s_pub.s = so;
    s_pub.cap_mah = cap;
    portEXIT_CRITICAL(&s_mux);
}

void battery_poll(void)
{
    if (!s_ok) return;
    int sum = 0, n = 0, raw;
    for (int i = 0; i < 4; i++)
        if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK) { sum += raw; n++; }
    int pin_mv;
    if (!n || adc_cali_raw_to_voltage(s_cali, (sum + n / 2) / n, &pin_mv) != ESP_OK) return;

    batg_state_t was = s_g.state;
    // tud_mounted() alone stays true after the cable is pulled (no VBUS
    // monitoring on this board), but a pulled cable leaves the bus suspended.
    bool host = tud_mounted() && !tud_suspended();
    batgauge_sample(&s_g, pin_mv * 2, host);            // 100k/100k divider
    if (++s_ticks % 10 == 0) second_tick();

    // Transitions plus a 5-minute trace: the evidence for tuning batgauge.c.
    static int trace;
    if (s_g.state != was || ++trace >= 3000) {
        trace = 0;
        static const char *NAMES[] = { "unknown", "on battery", "on USB" };
        logln("[bat] %s rail %d mV ripple %d mV %d%% host=%d",
              NAMES[s_g.state], s_g.med_mv, s_g.ripple_mv, s_g.pct, host);
    }
}

void battery_get(battery_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_pub.g;
    portEXIT_CRITICAL(&s_mux);
}

void battery_info(battery_info_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_pub;
    portEXIT_CRITICAL(&s_mux);
}

void battery_set_capacity(int mah)
{
    if (!s_ok) return;
    if (mah < 0) mah = 0;
    if (mah > 20000) mah = 20000;
    portENTER_CRITICAL(&s_mux);
    s_cap = mah;
    portEXIT_CRITICAL(&s_mux);
    nvs_put("mah", mah);
    logln("[bat] capacity set to %d mAh", mah);
}

void battery_forget(void)
{
    if (!s_ok) return;
    set_seen(false, "portal");
    portENTER_CRITICAL(&s_mux);
    s_pub.g.present = s_pub.g.charging = false;
    portEXIT_CRITICAL(&s_mux);
}

#else   // classic ESP32: no board declares a sense pin
void battery_start(void) {}
void battery_poll(void) {}
bool battery_supported(void) { return false; }
void battery_get(battery_t *out) { memset(out, 0, sizeof *out); out->pct = -1; }
void battery_info(battery_info_t *out) { memset(out, 0, sizeof *out); }
void battery_set_capacity(int mah) { (void)mah; }
void battery_forget(void) {}
#endif
