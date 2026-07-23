// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Board 4 layout — LilyGO T3-S3, SSD1306 128x64 monochrome.
//
// Same six bands as the 320x170 colour layout, same left/centre/right anchors,
// same reading order — scaled to the panel's own pixel budget:
//
//   band A  y0    8px   F-ONEO      A20N   ACI740     tail | type | flight
//   band B  y11   3px   [========------------]  62%   trip completion
//   band C  y17  16px   NOU>NRT           12:50L      route (hero) | local ETA
//   band D  y35   8px   N22°34 E110°10    31000ft     position | altitude
//   band E  y44   8px   4300NM 11:42/12:50    +11     distance | TOD/ETA | zone
//   band F  y52  12px   13:02z   ...        23:50     UTC | status | local
//                       8 + 3 + 16 + 8 + 8 + 12 = 55px, 9px of inter-band gap
//
// Monochrome forces three translations from the colour board:
//   * colour-coded state becomes SHAPE: Wi-Fi strength is bar count, internet
//     is a filled vs hollow block, feed activity is a blinking dot.
//   * the dimmed-trailing-digits altitude trick and the greyed-when-stale route
//     have no equivalent, so they render plain.
//   * seconds are dropped from both clocks and coordinates lose their decimals
//     — that is what buys band F its centre gap and band D its altitude.
//
// POLARITY: esp_lvgl_port's mono transform lights a pixel for a BLACK source
// pixel (esp_lvgl_port_disp.c:615-619). So in this file lv_color_black() means
// "lit" and lv_color_white() means "dark". The macros below say so out loud.
#include "paneldrv.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include <stdio.h>
#include <string.h>
#include <math.h>

#define OLED_LIT   lv_color_black()   // -> pixel ON  after the port's inversion
#define OLED_DARK  lv_color_white()   // -> pixel OFF after the port's inversion

// 1-bpp fonts generated from the panel's native geometry. LVGL's built-in
// Montserrat fonts are 4-bpp anti-aliased; thresholded to one bit their stems
// break up and read as furry noise. See tools/gen_oled_fonts.sh.
LV_FONT_DECLARE(font_aid8);
LV_FONT_DECLARE(font_aid12);
LV_FONT_DECLARE(font_aid16);

#define OLED_W 128
#define OLED_H 64

static lv_obj_t *a_tail, *a_type, *a_flight;
static lv_obj_t *b_bar, *b_pct;
static lv_obj_t *c_route, *c_leta;
static lv_obj_t *d_pos, *d_alt;
static lv_obj_t *e_nm, *e_eta, *e_tz;
static lv_obj_t *f_utc, *f_local;
static lv_obj_t *i_bar[3], *i_inet, *i_feed;
static bool s_splash_filled;

static lv_obj_t *mk(lv_obj_t *scr, const lv_font_t *f, lv_align_t al, int x, int y)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, OLED_LIT, 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, al, x, y);
    return l;
}

// A plain lit rectangle. Used for the signal bars and the status blocks:
// at this size a drawn shape reads far better than a glyph.
static lv_obj_t *mkblock(lv_obj_t *scr, int x, int y, int w, int h, bool filled)
{
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, filled ? 0 : 1, 0);
    lv_obj_set_style_border_color(o, OLED_LIT, 0);
    lv_obj_set_style_bg_opa(o, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(o, OLED_LIT, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void vis(lv_obj_t *o, bool on)
{
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void oled_build(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, OLED_DARK, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // band A — identity
    a_tail   = mk(scr, &font_aid8, LV_ALIGN_TOP_LEFT,  0, 0);
    a_type   = mk(scr, &font_aid8, LV_ALIGN_TOP_MID,   0, 0);
    a_flight = mk(scr, &font_aid8, LV_ALIGN_TOP_RIGHT, 0, 0);

    // band B — trip completion. Hollow track, solid fill; no gradient and no
    // rounded caps, both of which turn to mush at 3 px on a 1-bit panel.
    b_bar = lv_bar_create(scr);
    lv_obj_remove_style_all(b_bar);
    lv_obj_set_size(b_bar, 98, 3);
    lv_obj_set_pos(b_bar, 0, 11);
    lv_bar_set_range(b_bar, 0, 1000);
    lv_obj_set_style_radius(b_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(b_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(b_bar, OLED_LIT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b_bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(b_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(b_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b_bar, OLED_LIT, LV_PART_INDICATOR);
    b_pct = mk(scr, &font_aid8, LV_ALIGN_TOP_RIGHT, 0, 9);

    // band C — route is the hero line, local ETA rides its right
    c_route = mk(scr, &font_aid16, LV_ALIGN_TOP_LEFT,  0, 17);
    c_leta  = mk(scr, &font_aid12, LV_ALIGN_TOP_RIGHT, 0, 20);

    // band D — position | altitude
    d_pos = mk(scr, &font_aid8, LV_ALIGN_TOP_LEFT,  0, 35);
    d_alt = mk(scr, &font_aid8, LV_ALIGN_TOP_RIGHT, 0, 35);

    // band E — distance | TOD/ETA | zone
    e_nm  = mk(scr, &font_aid8, LV_ALIGN_TOP_LEFT,  0, 44);
    e_eta = mk(scr, &font_aid8, LV_ALIGN_TOP_MID,   2, 44);
    e_tz  = mk(scr, &font_aid8, LV_ALIGN_TOP_RIGHT, 0, 44);

    // band F — UTC | status indicators | local time
    f_utc   = mk(scr, &font_aid12, LV_ALIGN_BOTTOM_LEFT,  0, 0);
    f_local = mk(scr, &font_aid12, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    // status row, centred between the two clocks: three rising signal bars,
    // an internet block, and a feed-activity dot.
    for (int i = 0; i < 3; i++)
        i_bar[i] = mkblock(scr, 48 + i * 4, 62 - (3 + i * 2), 3, 3 + i * 2, true);
    i_inet = mkblock(scr, 63, 56, 7, 6, true);
    i_feed = mkblock(scr, 74, 58, 4, 4, true);
}

static void oled_render(const flightview_t *v)
{
    char buf[40];

    if (!s_splash_filled) s_splash_filled = true;

    if (v->have_id) {
        lv_label_set_text(a_tail, v->tail);
        lv_label_set_text(a_type, v->actype);
        // prefix + number: no per-span colour on a 1-bit panel, so they join
        snprintf(buf, sizeof buf, "%s%s", v->fl_prefix, v->fl_number);
        lv_label_set_text(a_flight, buf);
    } else {
        // splash: mirrors the colour board's AIDlink | build | IP row
        lv_label_set_text(a_tail, "AIDlink");
        lv_label_set_text(a_type, "");
        snprintf(buf, sizeof buf, "b%d", v->buildnum);
        lv_label_set_text(a_flight, buf);
    }

    // band B
    vis(b_bar, v->have_trip);
    vis(b_pct, v->have_trip);
    if (v->have_trip) {
        lv_bar_set_value(b_bar, (int)lround(v->trip_pct * 10), LV_ANIM_OFF);
        snprintf(buf, sizeof buf, "%d%%", (int)lround(v->trip_pct));
        lv_label_set_text(b_pct, buf);
    }

    // band C — route, or the AID's IP while there is no identity yet
    if (!v->have_id) {
        lv_label_set_text(c_route, v->ip);
    } else if (v->have_route) {
        snprintf(buf, sizeof buf, "%s>%s", v->orig, v->dest);
        lv_label_set_text(c_route, buf);
    } else {
        lv_label_set_text(c_route, v->route_placeholder);
    }
    lv_label_set_text(c_leta, v->have_leta ? v->leta_hhmm : "");

    // band D — coordinates without decimals: the fractional minutes cost ~20px
    // that the altitude needs, and they are not readable at 8px anyway.
    if (v->have_pos) {
        char lat[16], lon[16];
        snprintf(lat, sizeof lat, "%s", v->lat_v);
        snprintf(lon, sizeof lon, "%s", v->lon_v);
        char *dot = strchr(lat, '.'); if (dot) *dot = 0;
        dot = strchr(lon, '.');       if (dot) *dot = 0;
        snprintf(buf, sizeof buf, "%s%s %s%s", v->lat_h, lat, v->lon_h, lon);
        lv_label_set_text(d_pos, buf);
        snprintf(buf, sizeof buf, "%s%sft", v->alt_lead, v->alt_last2);
        lv_label_set_text(d_alt, buf);
    } else {
        lv_label_set_text(d_pos, "");
        lv_label_set_text(d_alt, "");
    }

    // band E
    if (v->nm_val[0]) {
        snprintf(buf, sizeof buf, "%s%s", v->nm_val, v->nm_has_unit ? "NM" : "");
        lv_label_set_text(e_nm, buf);
    } else {
        lv_label_set_text(e_nm, "");
    }
    // TOD/ETA share the centre. The colour board marks them with the U+2198 and
    // U+25CE glyphs; those exist only as 4-bpp anti-aliased fonts, so here the
    // pair is written "tod/eta" and a lone ETA stands by itself.
    if (v->have_tod && v->have_eta)
        snprintf(buf, sizeof buf, "%s/%s", v->tod_hhmm, v->eta_hhmm);
    else if (v->have_eta)
        snprintf(buf, sizeof buf, "%s", v->eta_hhmm);
    else
        buf[0] = 0;
    lv_label_set_text(e_eta, buf);
    // "UTC+11" -> "+11": the prefix is redundant next to a UTC clock
    if (v->have_tz)
        lv_label_set_text(e_tz, strncmp(v->tz_label, "UTC", 3) == 0 && v->tz_label[3]
                                ? v->tz_label + 3 : "Z");
    else
        lv_label_set_text(e_tz, "");

    // band F — clocks without seconds (HH:MM), see header note
    if (v->have_utc) {
        snprintf(buf, sizeof buf, "%.5sz", v->utc_hms);
        lv_label_set_text(f_utc, buf);
    } else {
        lv_label_set_text(f_utc, "--:--z");
    }
    if (v->have_local) {
        snprintf(buf, sizeof buf, "%.5s", v->local_hms);
        lv_label_set_text(f_local, buf);
    } else {
        lv_label_set_text(f_local, "--:--");
    }
}

static void oled_status(const fv_status_t *s)
{
    int nlit = (s->link == FV_LINK_UP) ? s->bars : 3;
    for (int i = 0; i < 3; i++)
        vis(i_bar[i], s->blink_on && i < nlit);
    // internet: solid block up, hollow outline down
    lv_obj_set_style_bg_opa(i_inet, s->internet ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(i_inet, s->internet ? 0 : 1, 0);
    vis(i_feed, s->feed_active);
}

const layout_drv_t layout_oled = {
    .name = "oled-128x64",
    .build = oled_build,
    .render = oled_render,
    .status = oled_status,
};

#endif // CONFIG_IDF_TARGET_ESP32S3
