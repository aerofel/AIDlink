// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Flight display for the LilyGO T-Display-S3: ST7789 170x320 (landscape 320x170)
// behind an Intel-8080 8-bit bus, driven by esp_lcd + LVGL (esp_lvgl_port).
// Layout:
//   F-ONEO                        SB800     tail (cyan) · flight number
//              NOU -> NRT                   route, large
//            2 431 NM TO GO                 great-circle distance to arrival
//   UTC+11                     14:23:05     solar-time zone + local time at position
//
// The UTC offset is the nautical/solar estimate round(lon/15) — a real IANA
// timezone lookup needs a shapefile database that has no place on this chip.
// The clock ticks from the last fix's UTC timestamp (Viasat provides it), so it
// stays right even when SNTP never ran.
#include "display.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "board.h"
#include "pos.h"
#include "geo.h"
#include "airports.h"
#include "tzdb.h"
#include "netcore.h"
#include "log.h"
#include "buildnum.h"
#include "eta.h"
#include "eta_profile.h"
#include "perfdb.h"
#include "esp_heap_caps.h"

static const char *TAG = "disp";

// single-glyph U+27A4 arrowhead font (font_arrow.c, generated)
LV_FONT_DECLARE(font_arrow);
// single-glyph U+1F310 globe font (font_globe.c, generated)
LV_FONT_DECLARE(font_globe);
// single-glyph U+2198 top-of-descent icon (font_tod.c, generated)
LV_FONT_DECLARE(font_tod);

// Bring-up evidence into the /log ring buffer: this board has no serial console
// (TinyUSB owns the only USB port), so the web /log endpoint is the only way to
// see how far display init got. Cheap enough to keep permanently.
#define DLOG(fmt, ...) do { ESP_LOGI(TAG, fmt, ##__VA_ARGS__); logln("[disp] " fmt, ##__VA_ARGS__); } while (0)

// --- LilyGO T-Display-S3 wiring (factory pin map) ---
#define PIN_PWR_ON   15   // board/LCD power rail enable
#define PIN_BL       38   // backlight
#define PIN_RST      5
#define PIN_CS       6
#define PIN_DC       7
#define PIN_WR       8    // i80 write strobe (pclk)
#define PIN_RD       9    // read strobe — unused, must idle high
#define LCD_H        320  // landscape
#define LCD_V        170
#define LCD_GAP_Y    35   // 170-line panel sits at offset 35 in the 240-line ST7789 RAM
#define LCD_PCLK_HZ  (8 * 1000 * 1000)

// palette (avionics-flavored, matches the web portal where possible)
#define COL_GREY    0x9E9E9E
#define COL_WHITE   0xFFFFFF
#define COL_CYAN    0x00E5FF
#define COL_AMBER   0xFFB300
#define COL_GREEN   0x34D399
#define COL_MAGENTA 0xFF55FF
#define COL_RED     0xFF3B30
#define COL_YELLOW  0xFFEB3B
#define COL_DIMMED  0x3A3A3A   // unlit signal bars / idle feed icon
// portal palette (web.c CSS vars) for the no-identity splash row
#define COL_LOGO_CY 0x22D3EE   // --cy: "AID"
#define COL_LOGO_GR 0x34D399   // --gr: "link"
#define COL_MUTED   0x8AA0C0   // --mut: build number

static const aidlink_cfg_t *CFG;
static lv_obj_t *s_tail, *s_actype, *s_tz, *s_clock;              // single-color labels
static lv_obj_t *s_wifi_arc[3], *s_wifi_dot;                      // Wi-Fi fan (signal bars)
static lv_obj_t *s_globe, *s_feed;                                // internet + feed activity
static lv_obj_t *sg_brand, *s_build, *s_ip;                       // no-identity splash row
static lv_span_t *sp_br_aid, *sp_br_link;                         // AID | link
static lv_obj_t *s_trip, *s_trip_arrow, *s_trip_pct;              // trip-completion bar
static lv_obj_t *sg_nm, *sg_eta, *sg_tod;                         // distance + TOD + ETA line
static lv_span_t *sp_nm_v, *sp_nm_u;                              // 4300 | NM
static lv_span_t *sp_eta_t, *sp_eta_z;                            // 12:50 | z
static lv_span_t *sp_tod_l, *sp_tod_t, *sp_tod_z;                 // TOD | 11:42 | z
static eta_state_t s_eta;                                         // made-good estimator (fallback + ring)
static etap_state_t s_etap;                                       // theoretical-profile estimator
static lv_obj_t *sg_flight, *sg_route, *sg_line, *sg_alt, *sg_utc; // multi-color spangroups
static lv_span_t *sp_fl_pre, *sp_fl_num;                      // ACI | 330
static lv_span_t *sp_ro_o, *sp_ro_ar, *sp_ro_d;               // NWWW | small arrow | NFFN
static lv_span_t *sp_lat_h, *sp_lat_v, *sp_lon_h, *sp_lon_v;  // N | 22°34.12 | E | 110°10.23
static lv_span_t *sp_alt_v, *sp_alt_u;                        // 31000 | ft
static lv_span_t *sp_utc_t, *sp_utc_z;                        // 13:02:45 | z

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---- panel bring-up ----------------------------------------------------

static bool panel_init(esp_lcd_panel_io_handle_t *io_out, esp_lcd_panel_handle_t *panel_out) {
    // Power + control strap pins first: rail on, RD parked high.
    gpio_config_t gc = {
        .pin_bit_mask = (1ULL << PIN_PWR_ON) | (1ULL << PIN_RD) | (1ULL << PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&gc);
    gpio_set_level(PIN_PWR_ON, 1);
    gpio_set_level(PIN_RD, 1);
    // Backlight stays dark until the first frame is flushed: the ST7789's
    // graphics RAM survives resets, so an early backlight shows whatever the
    // previous firmware left there (the factory demo, memorably).
    gpio_set_level(PIN_BL, 0);
    DLOG("gpio ok (pwr15=1 rd9=1 bl38=0), heap %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    esp_lcd_i80_bus_handle_t bus = NULL;
    esp_lcd_i80_bus_config_t bc = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = PIN_DC,
        .wr_gpio_num = PIN_WR,
        .data_gpio_nums = { 39, 40, 41, 42, 45, 46, 47, 48 },
        .bus_width = 8,
        .max_transfer_bytes = LCD_H * 80 * sizeof(uint16_t),
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    esp_err_t e = esp_lcd_new_i80_bus(&bc, &bus);
    DLOG("i80 bus: %s", esp_err_to_name(e));
    if (e != ESP_OK) return false;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i80_config_t ioc = {
        .cs_gpio_num = PIN_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 20,
        .dc_levels = { .dc_idle_level = 0, .dc_cmd_level = 0, .dc_dummy_level = 0, .dc_data_level = 1 },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    e = esp_lcd_new_panel_io_i80(bus, &ioc, &io);
    DLOG("i80 io: %s", esp_err_to_name(e));
    if (e != ESP_OK) return false;

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    e = esp_lcd_new_panel_st7789(io, &pc, &panel);
    DLOG("st7789 new: %s", esp_err_to_name(e));
    if (e != ESP_OK) return false;

    e = esp_lcd_panel_reset(panel);
    esp_err_t e2 = esp_lcd_panel_init(panel);
    DLOG("panel reset %s init %s", esp_err_to_name(e), esp_err_to_name(e2));
    esp_lcd_panel_invert_color(panel, true);      // this panel is IPS: inverted
    esp_lcd_panel_swap_xy(panel, true);           // landscape, USB-C to the right
    esp_lcd_panel_mirror(panel, false, true);
    esp_lcd_panel_set_gap(panel, 0, LCD_GAP_Y);
    e = esp_lcd_panel_disp_on_off(panel, true);
    DLOG("disp_on: %s", esp_err_to_name(e));

    *io_out = io; *panel_out = panel;
    return true;
}

// ---- screen construction (called under the LVGL lock) -------------------

static lv_obj_t *mklabel(lv_obj_t *parent, const lv_font_t *font, uint32_t hex,
                         lv_align_t align, int x, int y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
    lv_obj_align(l, align, x, y);
    lv_label_set_text(l, "");
    return l;
}

// LVGL9 dropped label recoloring — spangroups do the mixed-color lines.
static lv_obj_t *mkspangroup(lv_obj_t *parent, lv_align_t align, int x, int y) {
    lv_obj_t *sg = lv_spangroup_create(parent);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_EXPAND);
    lv_obj_align(sg, align, x, y);
    return sg;
}
static lv_span_t *addspan(lv_obj_t *sg, const lv_font_t *font, uint32_t hex) {
    lv_span_t *s = lv_spangroup_new_span(sg);
    lv_span_set_text(s, "");
    lv_style_set_text_font(lv_span_get_style(s), font);
    lv_style_set_text_color(lv_span_get_style(s), lv_color_hex(hex));
    return s;
}

static void build_ui(lv_display_t *disp) {
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_tail   = mklabel(scr, &lv_font_montserrat_20, COL_CYAN,  LV_ALIGN_TOP_LEFT,      8,   6);

    // status icon row: bottom line center, between the UTC readout (left) and
    // the local clock (right) —
    // Wi-Fi fan (3 signal bars) · globe (internet) · upload (feed activity)
    const int wcx = 137, wcy = 156;            // Wi-Fi fan center = the dot
    for (int i = 0; i < 3; i++) {
        int d = 8 + i * 6;                     // arc diameters 8 / 14 / 20
        lv_obj_t *a = lv_arc_create(scr);
        lv_obj_set_size(a, d, d);
        lv_obj_set_pos(a, wcx - d / 2, wcy - d / 2);
        lv_arc_set_bg_angles(a, 225, 315);     // top-quarter fan
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(a, 2, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
        s_wifi_arc[i] = a;
    }
    s_wifi_dot = lv_obj_create(scr);
    lv_obj_set_size(s_wifi_dot, 4, 4);
    lv_obj_set_pos(s_wifi_dot, wcx - 2, wcy - 2);
    lv_obj_set_style_radius(s_wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_wifi_dot, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_dot, lv_color_hex(COL_DIMMED), 0);
    lv_obj_clear_flag(s_wifi_dot, LV_OBJ_FLAG_SCROLLABLE);

    s_globe = mklabel(scr, &font_globe, COL_RED, LV_ALIGN_TOP_LEFT, 157, 144);
    lv_label_set_text(s_globe, "\xF0\x9F\x8C\x90");        // U+1F310

    s_feed = mklabel(scr, &lv_font_montserrat_16, COL_DIMMED, LV_ALIGN_TOP_LEFT, 180, 142);
    lv_label_set_text(s_feed, LV_SYMBOL_UPLOAD);

    // aircraft type (resolved perf-DB code), top-center, avionics yellow
    s_actype = mklabel(scr, &lv_font_montserrat_20, COL_YELLOW, LV_ALIGN_TOP_MID,      0,   6);

    // flight number: airline prefix grayed, number+suffix white
    sg_flight = mkspangroup(scr, LV_ALIGN_TOP_RIGHT, -8, 6);
    sp_fl_pre = addspan(sg_flight, &lv_font_montserrat_20, COL_GREY);
    sp_fl_num = addspan(sg_flight, &lv_font_montserrat_20, COL_WHITE);

    // no-identity splash row (shown until the feed provides a tail; refresh()
    // swaps it with tail/type/flight): AIDlink logo · build number · AID IP,
    // mirroring the portal hero (same colors, badge-framed IP)
    char buf[24];
    sg_brand   = mkspangroup(scr, LV_ALIGN_TOP_LEFT, 8, 6);
    sp_br_aid  = addspan(sg_brand, &lv_font_montserrat_20, COL_LOGO_CY);
    sp_br_link = addspan(sg_brand, &lv_font_montserrat_20, COL_LOGO_GR);
    lv_span_set_text(sp_br_aid, "AID");
    lv_span_set_text(sp_br_link, "link");
    lv_spangroup_refresh(sg_brand);

    s_build = mklabel(scr, &lv_font_montserrat_20, COL_MUTED, LV_ALIGN_TOP_MID, 0, 6);
    snprintf(buf, sizeof buf, "b%d", FW_BUILDNUM);
    lv_label_set_text(s_build, buf);

    s_ip = mklabel(scr, &lv_font_montserrat_16, COL_LOGO_CY, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_border_width(s_ip, 1, 0);
    lv_obj_set_style_border_color(s_ip, lv_color_hex(COL_LOGO_CY), 0);
    lv_obj_set_style_border_opa(s_ip, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_ip, 10, 0);
    lv_obj_set_style_pad_hor(s_ip, 7, 0);
    lv_obj_set_style_pad_ver(s_ip, 3, 0);
    lv_label_set_text(s_ip, CFG->ap_ip);

    // trip-completion bar (second line): thin rounded track, cyan->green
    // gradient fill, the route arrowhead riding the tip, percentage right
    #define TRIP_X 8
    #define TRIP_W 254
    s_trip = lv_bar_create(scr);
    lv_obj_set_size(s_trip, TRIP_W, 5);
    lv_obj_set_pos(s_trip, TRIP_X, 38);
    lv_bar_set_range(s_trip, 0, 1000);
    lv_obj_set_style_bg_color(s_trip, lv_color_hex(0x1E2A44), LV_PART_MAIN);   // portal --line
    lv_obj_set_style_bg_opa(s_trip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_trip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_trip, lv_color_hex(COL_LOGO_CY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(s_trip, lv_color_hex(COL_LOGO_GR), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(s_trip, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_trip, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_trip, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    s_trip_arrow = mklabel(scr, &font_arrow, COL_WHITE, LV_ALIGN_TOP_LEFT, -20, 32);
    lv_label_set_text(s_trip_arrow, "\xE2\x9E\xA4");
    s_trip_pct = mklabel(scr, &lv_font_montserrat_16, COL_WHITE, LV_ALIGN_TOP_RIGHT, -8, 32);

    // route line: 32pt ICAO codes joined by a smaller arrow, centered;
    // remaining distance rides the same line, pinned right (16pt so a 4-digit
    // NM clears the centered route at 320 px).
    sg_route = mkspangroup(scr, LV_ALIGN_CENTER, 0, -18);
    sp_ro_o  = addspan(sg_route, &lv_font_montserrat_32, COL_WHITE);
    sp_ro_ar = addspan(sg_route, &font_arrow, COL_WHITE);
    sp_ro_d  = addspan(sg_route, &lv_font_montserrat_32, COL_WHITE);
    // remaining distance sits above the UTC readout, mirroring the zone label
    // that sits above the local clock on the right; unit grayed like alt's "ft"
    sg_nm    = mkspangroup(scr, LV_ALIGN_BOTTOM_LEFT, 8, -34);
    sp_nm_v  = addspan(sg_nm, &lv_font_montserrat_16, COL_AMBER);
    sp_nm_u  = addspan(sg_nm, &lv_font_montserrat_14, COL_GREY);
    // top-of-descent (icon + time) and arrival estimate, centered together on
    // the distance line: dist left | ↘TOD · ETA centered | zone label right
    sg_tod   = mkspangroup(scr, LV_ALIGN_BOTTOM_MID, -44, -34);
    sp_tod_l = addspan(sg_tod, &font_tod, COL_GREY);           // ↘ icon
    sp_tod_t = addspan(sg_tod, &lv_font_montserrat_16, COL_AMBER);
    sp_tod_z = addspan(sg_tod, &lv_font_montserrat_14, COL_GREY);
    sg_eta   = mkspangroup(scr, LV_ALIGN_BOTTOM_MID, 36, -34);
    sp_eta_t = addspan(sg_eta, &lv_font_montserrat_16, COL_AMBER);
    sp_eta_z = addspan(sg_eta, &lv_font_montserrat_14, COL_GREY);

    // data line: coordinates full left, altitude full right, same 16pt
    sg_line  = mkspangroup(scr, LV_ALIGN_LEFT_MID, 8, 16);
    sp_lat_h = addspan(sg_line, &lv_font_montserrat_16, COL_GREY);
    sp_lat_v = addspan(sg_line, &lv_font_montserrat_16, COL_GREEN);
    sp_lon_h = addspan(sg_line, &lv_font_montserrat_16, COL_GREY);
    sp_lon_v = addspan(sg_line, &lv_font_montserrat_16, COL_GREEN);
    sg_alt   = mkspangroup(scr, LV_ALIGN_RIGHT_MID, -8, 16);
    sp_alt_v = addspan(sg_alt, &lv_font_montserrat_16, COL_WHITE);
    sp_alt_u = addspan(sg_alt, &lv_font_montserrat_14, COL_GREY);

    s_tz     = mklabel(scr, &lv_font_montserrat_16, COL_GREY,  LV_ALIGN_BOTTOM_RIGHT, -8, -34);
    s_clock  = mklabel(scr, &lv_font_montserrat_24, COL_WHITE, LV_ALIGN_BOTTOM_RIGHT, -8,  -2);

    // UTC bottom-left: magenta time, grayed 'z'
    sg_utc   = mkspangroup(scr, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    sp_utc_t = addspan(sg_utc, &lv_font_montserrat_24, COL_MAGENTA);
    sp_utc_z = addspan(sg_utc, &lv_font_montserrat_20, COL_GREY);
}

// Status icon row, updated every task tick (blinking needs a faster cadence
// than the content refresh).
//
// Wi-Fi fan — signal bars, weakest arc innermost:
//   slow red blink (all bars)   = no uplink connection
//   fast orange blink (all)     = scanning
//   1 bar orange / 2 dimmed     = connected, weak    (RSSI < -70 dBm)
//   2 bars yellow / 1 dimmed    = connected, medium  (-70 .. -60)
//   3 bars green                = connected, strong  (>= -60)
// Globe: green = internet reachable (netcore's frugal TCP probe), red = not.
// Upload: magenta flash whenever a location is RECEIVED from the feed
// (pos_fix_seq — not ADBP sends: with no EFB subscribed it never moved).
static void icons_update(void) {
    uint32_t now = now_ms();
    uint32_t lit;
    int nlit = 3;
    bool show = true;
    if (netcore_scanning()) {
        lit = COL_AMBER; show = (now / 150) % 2;          // fast, ~3 Hz
    } else if (netcore_sta_up(NULL)) {
        // banded level with 3 dB hysteresis per boundary so a hovering RSSI
        // doesn't flicker between colors
        static bool ge_med, ge_str;
        int rssi = netcore_sta_rssi();
        if (rssi >= -67) ge_med = true; else if (rssi <= -73) ge_med = false;
        if (rssi >= -57) ge_str = true; else if (rssi <= -63) ge_str = false;
        nlit = ge_str ? 3 : (ge_med ? 2 : 1);
        lit  = ge_str ? COL_GREEN : (ge_med ? COL_YELLOW : COL_AMBER);
    } else {
        lit = COL_RED; show = (now / 600) % 2;            // slow red, ~0.8 Hz
    }
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_arc_color(s_wifi_arc[i], lv_color_hex(i < nlit ? lit : COL_DIMMED), LV_PART_MAIN);
        lv_obj_set_style_opa(s_wifi_arc[i], show ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    lv_obj_set_style_bg_color(s_wifi_dot, lv_color_hex(lit), 0);
    lv_obj_set_style_opa(s_wifi_dot, show ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

    lv_obj_set_style_text_color(s_globe, lv_color_hex(netcore_inet_up() ? COL_GREEN : COL_RED), 0);

    static uint32_t feed_seq, feed_until;
    uint32_t seq = pos_fix_seq();
    if (seq != feed_seq) { feed_seq = seq; feed_until = now + 180; }
    lv_obj_set_style_text_color(s_feed, lv_color_hex(now < feed_until ? COL_MAGENTA : COL_DIMMED), 0);
}

// ---- 1 Hz content refresh ------------------------------------------------

static void vis(lv_obj_t *o, bool on) {
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void refresh(void) {
    pos_state_t p; pos_get(&p);
    char buf[48];

    // Top row: until the feed has provided an identity (live or last-known),
    // show the splash row (AIDlink · build · IP) instead of tail/type/flight.
    bool have_id = p.tail[0] || CFG->ac_tail[0];
    vis(s_tail, have_id); vis(s_actype, have_id); vis(sg_flight, have_id);
    vis(sg_brand, !have_id); vis(s_build, !have_id); vis(s_ip, !have_id);

    lv_label_set_text(s_tail, p.tail[0] ? p.tail : CFG->ac_tail);
    // resolved performance-DB type code (portal choice or feed pre-select) —
    // never the raw feed string; blank when no profile is selected
    const perf_ac_t *perf = perfdb_find(CFG->perf_type);
    lv_label_set_text(s_actype, perf ? perf->type : "");

    // flight number: gray airline prefix (letters), white number + suffix
    const char *fl = p.flight;
    int pre = 0;
    while (fl[pre] && !isdigit((unsigned char)fl[pre]) && pre < 7) pre++;
    char prebuf[8];
    memcpy(prebuf, fl, pre); prebuf[pre] = 0;
    lv_span_set_text(sp_fl_pre, prebuf);
    lv_span_set_text(sp_fl_num, fl + pre);
    lv_spangroup_refresh(sg_flight);

    // route with a small arrow; greyed while the fix is invalid/stale
    uint32_t rocol = p.valid ? COL_WHITE : 0x616161;
    lv_style_set_text_color(lv_span_get_style(sp_ro_o), lv_color_hex(rocol));
    lv_style_set_text_color(lv_span_get_style(sp_ro_ar), lv_color_hex(rocol));
    lv_style_set_text_color(lv_span_get_style(sp_ro_d), lv_color_hex(rocol));
    if (p.orig[0] || p.dest[0]) {
        // prefer IATA for display (NOU➤NRT reads better than NWWW➤RJAA at
        // 320 px); ICAO when the gazetteer has no IATA, code-as-received last
        const char *o = airports_iata(p.orig); if (!o) o = airports_icao(p.orig);
        if (!o) o = p.orig[0] ? p.orig : "----";
        const char *d = airports_iata(p.dest); if (!d) d = airports_icao(p.dest);
        if (!d) d = p.dest[0] ? p.dest : "----";
        lv_span_set_text(sp_ro_o, o);
        lv_span_set_text(sp_ro_ar, "\xE2\x9E\xA4");   // U+27A4, padding baked into the glyph
        lv_span_set_text(sp_ro_d, d);
    } else {
        lv_span_set_text(sp_ro_o, p.valid ? "- - -" : "NO POSITION");
        lv_span_set_text(sp_ro_ar, "");
        lv_span_set_text(sp_ro_d, "");
    }
    lv_spangroup_refresh(sg_route);

    // data line: coords left, then altitude; distance pinned right
    if (p.valid) {
        double av = fabs(p.lat);
        lv_span_set_text(sp_lat_h, p.lat >= 0 ? "N" : "S");
        snprintf(buf, sizeof buf, "%02d\xC2\xB0%05.2f", (int)av, (av - (int)av) * 60.0);
        lv_span_set_text(sp_lat_v, buf);
        av = fabs(p.lon);
        lv_span_set_text(sp_lon_h, p.lon >= 0 ? " E" : " W");
        snprintf(buf, sizeof buf, "%03d\xC2\xB0%05.2f", (int)av, (av - (int)av) * 60.0);
        lv_span_set_text(sp_lon_v, buf);
        // altitude rounded to 10 ft — raw feet jitter isn't information
        snprintf(buf, sizeof buf, "%d", (int)(lround(p.alt_ft / 10.0) * 10));
        lv_span_set_text(sp_alt_v, buf);
        lv_span_set_text(sp_alt_u, "ft");
    } else {
        lv_span_set_text(sp_lat_h, ""); lv_span_set_text(sp_lat_v, "");
        lv_span_set_text(sp_lon_h, ""); lv_span_set_text(sp_lon_v, "");
        lv_span_set_text(sp_alt_v, ""); lv_span_set_text(sp_alt_u, "");
    }
    lv_spangroup_refresh(sg_line);
    lv_spangroup_refresh(sg_alt);

    double alat, alon, dist_nm = -1;
    int dest_elev = 0;
    if (p.valid && p.dest[0] && airports_lookup_ex(p.dest, &alat, &alon, &dest_elev))
        dist_nm = geo_dist_nm(p.lat, p.lon, alat, alon);

    // trip completion: remaining vs the dep->arr great-circle total
    double olat, olon, tot_nm = -1;
    if (dist_nm >= 0 && p.orig[0] && airports_lookup(p.orig, &olat, &olon))
        tot_nm = geo_dist_nm(olat, olon, alat, alon);
    bool trip = tot_nm > 10;
    vis(s_trip, trip); vis(s_trip_arrow, trip); vis(s_trip_pct, trip);
    if (trip) {
        double pct = (1.0 - dist_nm / tot_nm) * 100.0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_bar_set_value(s_trip, (int)lround(pct * 10), LV_ANIM_OFF);
        // arrow tip rides the end of the fill (glyph tip ~20 px into the label box)
        lv_obj_set_pos(s_trip_arrow, TRIP_X + (int)(pct / 100.0 * TRIP_W) - 20, 32);
        snprintf(buf, sizeof buf, "%d%%", (int)lround(pct));
        lv_label_set_text(s_trip_pct, buf);
    }
    if (dist_nm >= 0) {
        snprintf(buf, sizeof buf, "%d", (int)lround(dist_nm));
        lv_span_set_text(sp_nm_v, buf);
        lv_span_set_text(sp_nm_u, "NM");
    } else if (p.valid && p.simulated) {
        lv_span_set_text(sp_nm_v, "SIM");
        lv_span_set_text(sp_nm_u, "");
    } else {
        lv_span_set_text(sp_nm_v, "");
        lv_span_set_text(sp_nm_u, "");
    }
    lv_spangroup_refresh(sg_nm);

    // UTC: prefer the system clock (SNTP / HTTP-Date disciplined, see poller.c),
    // else tick the last fix's UTC timestamp forward.
    uint64_t utc = 0;
    time_t st = time(NULL);
    if (st > 1750000000) utc = (uint64_t)st * 1000ULL;
    else if (p.utc_ms) utc = p.utc_ms + (now_ms() - p.last_fix_ms);
    uint32_t utc_s = (uint32_t)(utc / 1000);

    // Local time at position from the embedded IANA-derived timezone grid
    // (handles DST and :30/:45 offsets); tzdb falls back to solar internally.
    int off_min = 0; bool have_off = false;
    if (p.valid || p.lat != 0 || p.lon != 0) {
        off_min = tzdb_offset_min(p.lat, p.lon, utc_s ? utc_s : 1767225600u);
        have_off = true;
    }

    if (!have_off) {
        lv_label_set_text(s_tz, "");
    } else if (off_min % 60) {
        snprintf(buf, sizeof buf, "UTC%+d:%02d", off_min / 60, abs(off_min % 60));
        lv_label_set_text(s_tz, buf);
    } else if (off_min == 0) {
        lv_label_set_text(s_tz, "UTC");
    } else {
        snprintf(buf, sizeof buf, "UTC%+d", off_min / 60);
        lv_label_set_text(s_tz, buf);
    }

    struct tm tm;
    if (utc) {
        time_t u = (time_t)(utc / 1000);
        gmtime_r(&u, &tm);
        snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        lv_span_set_text(sp_utc_t, buf);
    } else {
        lv_span_set_text(sp_utc_t, "--:--:--");
    }
    lv_span_set_text(sp_utc_z, "z");
    lv_spangroup_refresh(sg_utc);

    // Arrival estimate on the distance line. The made-good estimator always
    // runs (it owns the sample ring and is the fallback); when an aircraft
    // profile is resolved and the route is known, the theoretical-profile
    // estimator overrides it and adds the TOD readout (see eta_profile.h).
    long eta_min = eta_update(&s_eta, dist_nm, p.gs_kt, utc ? utc / 1000.0 : 0);
    long tod_min = 0;
    if (perf && utc && dist_nm >= 0 && tot_nm > 10) {
        time_t us = (time_t)(utc / 1000);
        struct tm tmu;
        gmtime_r(&us, &tmu);
        etap_out_t po = etap_update(&s_etap, perf, p.lat, p.lon, alat, alon,
                                    dest_elev, tot_nm, dist_nm, p.gs_kt,
                                    eta_made_good_kt(&s_eta), utc / 1000.0,
                                    tmu.tm_mon + 1, CFG->winds_enable);
        if (po.eta_min > 0) { eta_min = po.eta_min; tod_min = po.tod_min; }
    }
    if (eta_min > 0) {
        time_t et = (time_t)eta_min * 60;
        gmtime_r(&et, &tm);
        snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
        lv_span_set_text(sp_eta_t, buf);
        lv_span_set_text(sp_eta_z, "z");
    } else {
        lv_span_set_text(sp_eta_t, "");
        lv_span_set_text(sp_eta_z, "");
    }
    lv_spangroup_refresh(sg_eta);
    if (tod_min > 0) {
        time_t tt = (time_t)tod_min * 60;
        gmtime_r(&tt, &tm);
        snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
        lv_span_set_text(sp_tod_l, "\xE2\x86\x98");   // U+2198 TOD icon
        lv_span_set_text(sp_tod_t, buf);
        lv_span_set_text(sp_tod_z, "z");
    } else {
        lv_span_set_text(sp_tod_l, "");
        lv_span_set_text(sp_tod_t, "");
        lv_span_set_text(sp_tod_z, "");
    }
    lv_spangroup_refresh(sg_tod);
    if (utc && have_off) {
        time_t local = (time_t)(utc / 1000) + (time_t)off_min * 60;
        gmtime_r(&local, &tm);
        snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        lv_label_set_text(s_clock, buf);
    } else {
        lv_label_set_text(s_clock, "--:--:--");
    }
}

static void display_task(void *arg) {
    bool first = true;
    // 100 ms tick: the Wi-Fi indicator blinks every tick, the (heavier)
    // content refresh keeps its former 500 ms cadence.
    // Diagnostics (2026-07-09 freeze hunt): LV_USE_ASSERT_MALLOC's default
    // handler is while(1) — a failed LVGL pool alloc silently hangs the render
    // task while it holds the lock, freezing the screen with the device alive.
    // The heartbeat below surfaces pool usage in /log; the stuck-lock warning
    // distinguishes "render task hung" from "our refresh crashed".
    int lockfail = 0;
    for (int tick = 0;; tick++) {
        if (lvgl_port_lock(100)) {
            lockfail = 0;
            if (tick % 5 == 0) refresh();
            icons_update();
            if (tick % 600 == 0) {   // ~60 s heartbeat with LVGL pool stats
                lv_mem_monitor_t m;
                lv_mem_monitor(&m);
                DLOG("hb tick=%d lv used=%d%% frag=%d%% free=%u biggest=%u",
                     tick, m.used_pct, m.frag_pct,
                     (unsigned)m.free_size, (unsigned)m.free_biggest_size);
            }
            lvgl_port_unlock();
            if (first) { first = false; DLOG("task alive, first refresh done"); }
        } else if (++lockfail == 20) {
            DLOG("LVGL lock stuck 2+ s — render task hung (pool exhausted?)");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void display_start(const aidlink_cfg_t *cfg) {
    // Force /log capture during bring-up so the evidence is web-readable on a
    // board with no serial console; restore the configured setting after.
    log_set_enable(true);   // diagnostic build: keep /log capture on
    DLOG("board=%s has_display=%d", board_get()->name, board_get()->has_display);
    if (!board_get()->has_display) return;
    CFG = cfg;

    esp_lcd_panel_io_handle_t io; esp_lcd_panel_handle_t panel;
    if (!panel_init(&io, &panel)) return;

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t pe = lvgl_port_init(&port_cfg);
    DLOG("lvgl_port_init: %s", esp_err_to_name(pe));
    if (pe != ESP_OK) return;

    const lvgl_port_display_cfg_t dc = {
        .io_handle = io,
        .panel_handle = panel,
        // Internal SRAM is scarce here (see LEARNING.md): a single 20-line
        // partial buffer (12.8 KB DMA) is plenty for a 1 Hz textual UI.
        .buffer_size = LCD_H * 20,
        .double_buffer = false,
        .hres = LCD_H,
        .vres = LCD_V,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = true, .mirror_x = false, .mirror_y = true },
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&dc);
    DLOG("lvgl_port_add_disp: %s, heap %u", disp ? "ok" : "NULL",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (!disp) return;

    if (lvgl_port_lock(0)) {
        build_ui(disp);
        refresh();
        lvgl_port_unlock();
        DLOG("ui built");
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(PIN_BL, 1);

    // 4 KB stack: refresh() now walks the theoretical-profile estimator
    // (deep double math + gmtime/snprintf); 3 KB panicked once the first
    // valid fix arrived — the profile table itself already lives in the
    // etap_state_t, NOT on this stack.
    xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
    DLOG("display up (320x170, i80 @ %d MHz)", LCD_PCLK_HZ / 1000000);
}

#else   // targets without the LCD peripheral (classic ESP32)
void display_start(const aidlink_cfg_t *cfg) { (void)cfg; }
#endif
