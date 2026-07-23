// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AIDlink contributors
//
// Board 3 layout — LilyGO T-Display-S3, 320x170 colour.
//
//   F-ONEO                 A20N                 ACI740     identity row
//   ============================================   62%     trip completion
//        NOU >>> NRT                      [ 12:50 L ]      route + local ETA
//   N22°34.12 E110°10.23                     31000 ft      position
//   4300 NM        v11:42z  (o)12:50z          UTC+11      distance / ETA / zone
//   13:02:45z         .il (cloud) ^          23:50:12      UTC / status / local
//
// Pure presentation: every number here arrives preformatted in flightview_t.
#include "paneldrv.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include <stdio.h>
#include <math.h>

// single-glyph generated fonts (see tools/gen_*_font.py)
LV_FONT_DECLARE(font_arrow);   // U+27A4 arrowhead
LV_FONT_DECLARE(font_cloud);   // U+2601 internet icon
LV_FONT_DECLARE(font_tod);     // U+2198 top-of-descent
LV_FONT_DECLARE(font_eta);     // U+25CE arrival bullseye

// palette (avionics-flavored, matches the web portal where possible)
#define COL_GREY    0x9E9E9E
#define COL_WHITE   0xFFFFFF
#define COL_AMBER   0xFFB300
#define COL_GREEN   0x34D399
#define COL_MAGENTA 0xFF55FF
#define COL_RED     0xFF3B30
#define COL_YELLOW  0xFFEB3B
#define COL_DIMMED  0x3A3A3A   // unlit signal bars / idle feed icon
#define COL_ETA     0xA8D1D1   // ETA times, UTC + destination-local
#define COL_DIST    0xFFCBCB   // remaining-distance value
#define COL_TOD     0xF1F7B5   // top-of-descent icon + time
#define COL_INET    0x9EA1D4   // cloud icon when the internet is reachable
#define COL_LOGO_CY 0x22D3EE   // portal --cy: "AID"
#define COL_LOGO_GR 0x34D399   // portal --gr: "link"
#define COL_MUTED   0x8AA0C0   // portal --mut: build number

#define TRIP_X 8
#define TRIP_W 254

static lv_obj_t *s_tail, *s_actype, *s_tz, *s_clock;
static lv_obj_t *s_wifi_arc[3], *s_wifi_dot;
static lv_obj_t *s_globe, *s_feed;
static lv_obj_t *sg_brand, *s_build, *s_ip;
static lv_span_t *sp_br_aid, *sp_br_link;
static lv_obj_t *s_trip, *s_trip_arrow, *s_trip_pct;
static lv_obj_t *sg_nm, *sg_eta;
static lv_span_t *sp_nm_v, *sp_nm_u;
static lv_span_t *sp_tod_l, *sp_tod_t, *sp_tod_z;
static lv_span_t *sp_eta_i, *sp_eta_t, *sp_eta_z;
static lv_obj_t *sg_leta;
static lv_span_t *sp_leta_t, *sp_leta_l;
static lv_obj_t *sg_flight, *sg_route, *sg_line, *sg_alt, *sg_utc;
static lv_span_t *sp_fl_pre, *sp_fl_num;
static lv_span_t *sp_ro_o, *sp_ro_ar, *sp_ro_d;
static lv_span_t *sp_lat_h, *sp_lat_v, *sp_lon_h, *sp_lon_v;
static lv_span_t *sp_alt_v, *sp_alt_v2, *sp_alt_u;
static lv_span_t *sp_utc_t, *sp_utc_z;
static bool s_splash_filled;

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

static void vis(lv_obj_t *o, bool on) {
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void tdisp_build(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_tail = mklabel(scr, &lv_font_montserrat_20, COL_LOGO_CY, LV_ALIGN_TOP_LEFT, 8, 6);

    // status icon row: bottom line center, between the UTC readout (left) and
    // the local clock (right) — Wi-Fi fan · cloud (internet) · upload (feed)
    const int wcx = 137, wcy = 156;
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

    s_globe = mklabel(scr, &font_cloud, COL_RED, LV_ALIGN_TOP_LEFT, 156, 144);
    lv_label_set_text(s_globe, "\xE2\x98\x81");
    s_feed = mklabel(scr, &lv_font_montserrat_16, COL_DIMMED, LV_ALIGN_TOP_LEFT, 186, 142);
    lv_label_set_text(s_feed, LV_SYMBOL_UPLOAD);

    s_actype = mklabel(scr, &lv_font_montserrat_20, COL_YELLOW, LV_ALIGN_TOP_MID, 0, 6);

    sg_flight = mkspangroup(scr, LV_ALIGN_TOP_RIGHT, -8, 6);
    sp_fl_pre = addspan(sg_flight, &lv_font_montserrat_20, COL_GREY);
    sp_fl_num = addspan(sg_flight, &lv_font_montserrat_20, COL_WHITE);

    // no-identity splash row: AIDlink logo · build number · AID IP
    sg_brand   = mkspangroup(scr, LV_ALIGN_TOP_LEFT, 8, 6);
    sp_br_aid  = addspan(sg_brand, &lv_font_montserrat_20, COL_LOGO_CY);
    sp_br_link = addspan(sg_brand, &lv_font_montserrat_20, COL_LOGO_GR);
    lv_span_set_text(sp_br_aid, "AID");
    lv_span_set_text(sp_br_link, "link");
    lv_spangroup_refresh(sg_brand);

    s_build = mklabel(scr, &lv_font_montserrat_20, COL_MUTED, LV_ALIGN_TOP_MID, 0, 6);
    s_ip = mklabel(scr, &lv_font_montserrat_16, COL_LOGO_CY, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_border_width(s_ip, 1, 0);
    lv_obj_set_style_border_color(s_ip, lv_color_hex(COL_LOGO_CY), 0);
    lv_obj_set_style_border_opa(s_ip, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_ip, 10, 0);
    lv_obj_set_style_pad_hor(s_ip, 7, 0);
    lv_obj_set_style_pad_ver(s_ip, 3, 0);

    // trip-completion bar: thin rounded track, cyan->green gradient fill,
    // the route arrowhead riding the tip, percentage right
    s_trip = lv_bar_create(scr);
    lv_obj_set_size(s_trip, TRIP_W, 5);
    lv_obj_set_pos(s_trip, TRIP_X, 38);
    lv_bar_set_range(s_trip, 0, 1000);
    lv_obj_set_style_bg_color(s_trip, lv_color_hex(0x1E2A44), LV_PART_MAIN);
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

    // route sits slightly left of center: the local-ETA badge owns the right
    sg_route = mkspangroup(scr, LV_ALIGN_CENTER, -24, -18);
    sp_ro_o  = addspan(sg_route, &lv_font_montserrat_32, COL_WHITE);
    sp_ro_ar = addspan(sg_route, &font_arrow, COL_WHITE);
    sp_ro_d  = addspan(sg_route, &lv_font_montserrat_32, COL_WHITE);

    sg_leta = mkspangroup(scr, LV_ALIGN_RIGHT_MID, -6, -18);
    lv_obj_set_style_border_width(sg_leta, 1, 0);
    lv_obj_set_style_border_color(sg_leta, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_border_opa(sg_leta, LV_OPA_40, 0);
    lv_obj_set_style_radius(sg_leta, 8, 0);
    lv_obj_set_style_pad_hor(sg_leta, 6, 0);
    // digits have no descenders, so the font's descender gap leaves dead space
    // at the frame bottom — asymmetric padding re-centers the glyphs optically
    lv_obj_set_style_pad_top(sg_leta, 5, 0);
    lv_obj_set_style_pad_bottom(sg_leta, 1, 0);
    sp_leta_t = addspan(sg_leta, &lv_font_montserrat_24, COL_ETA);
    sp_leta_l = addspan(sg_leta, &lv_font_montserrat_14, COL_GREEN);

    sg_nm   = mkspangroup(scr, LV_ALIGN_BOTTOM_LEFT, 8, -34);
    sp_nm_v = addspan(sg_nm, &lv_font_montserrat_16, COL_DIST);
    sp_nm_u = addspan(sg_nm, &lv_font_montserrat_14, COL_GREY);

    // TOD and arrival share ONE spangroup pinned to the line's center, so the
    // pair is always centered (and a lone ETA self-centers when TOD is hidden)
    sg_eta   = mkspangroup(scr, LV_ALIGN_BOTTOM_MID, 0, -34);
    sp_tod_l = addspan(sg_eta, &font_tod, COL_TOD);
    sp_tod_t = addspan(sg_eta, &lv_font_montserrat_16, COL_TOD);
    sp_tod_z = addspan(sg_eta, &lv_font_montserrat_14, COL_GREY);
    sp_eta_i = addspan(sg_eta, &font_eta, COL_ETA);
    sp_eta_t = addspan(sg_eta, &lv_font_montserrat_16, COL_ETA);
    sp_eta_z = addspan(sg_eta, &lv_font_montserrat_14, COL_GREY);

    sg_line  = mkspangroup(scr, LV_ALIGN_LEFT_MID, 8, 16);
    sp_lat_h = addspan(sg_line, &lv_font_montserrat_16, COL_GREY);
    sp_lat_v = addspan(sg_line, &lv_font_montserrat_16, COL_GREEN);
    sp_lon_h = addspan(sg_line, &lv_font_montserrat_16, COL_GREY);
    sp_lon_v = addspan(sg_line, &lv_font_montserrat_16, COL_GREEN);
    sg_alt   = mkspangroup(scr, LV_ALIGN_RIGHT_MID, -8, 16);
    sp_alt_v = addspan(sg_alt, &lv_font_montserrat_16, COL_WHITE);
    // tens+units dimmed halfway toward the unit grey: the bright leading digits
    // read directly as the flight level while the full altitude stays
    sp_alt_v2 = addspan(sg_alt, &lv_font_montserrat_16, 0xCECECE);
    sp_alt_u  = addspan(sg_alt, &lv_font_montserrat_14, COL_GREY);

    s_tz    = mklabel(scr, &lv_font_montserrat_16, COL_GREY,  LV_ALIGN_BOTTOM_RIGHT, -8, -34);
    s_clock = mklabel(scr, &lv_font_montserrat_24, COL_WHITE, LV_ALIGN_BOTTOM_RIGHT, -8, -2);

    sg_utc   = mkspangroup(scr, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    sp_utc_t = addspan(sg_utc, &lv_font_montserrat_24, COL_MAGENTA);
    sp_utc_z = addspan(sg_utc, &lv_font_montserrat_20, COL_GREY);
}

static void tdisp_render(const flightview_t *v)
{
    char buf[48];

    if (!s_splash_filled) {
        s_splash_filled = true;
        snprintf(buf, sizeof buf, "b%d", v->buildnum);
        lv_label_set_text(s_build, buf);
        lv_label_set_text(s_ip, v->ip);
    }

    // Until the feed provides an identity, show the splash row instead.
    vis(s_tail, v->have_id); vis(s_actype, v->have_id); vis(sg_flight, v->have_id);
    vis(sg_brand, !v->have_id); vis(s_build, !v->have_id); vis(s_ip, !v->have_id);

    lv_label_set_text(s_tail, v->tail);
    lv_label_set_text(s_actype, v->actype);
    lv_span_set_text(sp_fl_pre, v->fl_prefix);
    lv_span_set_text(sp_fl_num, v->fl_number);
    lv_spangroup_refresh(sg_flight);

    // route greyed while the fix is invalid/stale
    uint32_t rocol = v->valid ? COL_WHITE : 0x616161;
    lv_style_set_text_color(lv_span_get_style(sp_ro_o), lv_color_hex(rocol));
    lv_style_set_text_color(lv_span_get_style(sp_ro_ar), lv_color_hex(rocol));
    lv_style_set_text_color(lv_span_get_style(sp_ro_d), lv_color_hex(rocol));
    if (v->have_route) {
        lv_span_set_text(sp_ro_o, v->orig);
        lv_span_set_text(sp_ro_ar, "\xE2\x9E\xA4");   // padding baked into the glyph
        lv_span_set_text(sp_ro_d, v->dest);
    } else {
        lv_span_set_text(sp_ro_o, v->route_placeholder);
        lv_span_set_text(sp_ro_ar, "");
        lv_span_set_text(sp_ro_d, "");
    }
    lv_spangroup_refresh(sg_route);

    if (v->have_pos) {
        lv_span_set_text(sp_lat_h, v->lat_h);
        lv_span_set_text(sp_lat_v, v->lat_v);
        lv_span_set_text(sp_lon_h, v->lon_h);
        lv_span_set_text(sp_lon_v, v->lon_v);
        lv_span_set_text(sp_alt_v, v->alt_lead);
        lv_span_set_text(sp_alt_v2, v->alt_last2);
        lv_span_set_text(sp_alt_u, "ft");
    } else {
        lv_span_set_text(sp_lat_h, ""); lv_span_set_text(sp_lat_v, "");
        lv_span_set_text(sp_lon_h, ""); lv_span_set_text(sp_lon_v, "");
        lv_span_set_text(sp_alt_v, ""); lv_span_set_text(sp_alt_v2, "");
        lv_span_set_text(sp_alt_u, "");
    }
    lv_spangroup_refresh(sg_line);
    lv_spangroup_refresh(sg_alt);

    vis(s_trip, v->have_trip); vis(s_trip_arrow, v->have_trip); vis(s_trip_pct, v->have_trip);
    if (v->have_trip) {
        lv_bar_set_value(s_trip, (int)lround(v->trip_pct * 10), LV_ANIM_OFF);
        // arrow tip rides the end of the fill (glyph tip ~20 px into the label)
        lv_obj_set_pos(s_trip_arrow, TRIP_X + (int)(v->trip_pct / 100.0 * TRIP_W) - 20, 32);
        snprintf(buf, sizeof buf, "%d%%", (int)lround(v->trip_pct));
        lv_label_set_text(s_trip_pct, buf);
    }

    lv_span_set_text(sp_nm_v, v->nm_val);
    lv_span_set_text(sp_nm_u, v->nm_has_unit ? "NM" : "");
    lv_spangroup_refresh(sg_nm);

    lv_span_set_text(sp_utc_t, v->have_utc ? v->utc_hms : "--:--:--");
    lv_span_set_text(sp_utc_z, "z");
    lv_spangroup_refresh(sg_utc);

    lv_label_set_text(s_tz, v->have_tz ? v->tz_label : "");
    lv_label_set_text(s_clock, v->have_local ? v->local_hms : "--:--:--");

    if (v->have_eta) {
        lv_span_set_text(sp_eta_i, "\xE2\x97\x8E");   // U+25CE arrival bullseye
        lv_span_set_text(sp_eta_t, v->eta_hhmm);
        lv_span_set_text(sp_eta_z, "z");
    } else {
        lv_span_set_text(sp_eta_i, "");
        lv_span_set_text(sp_eta_t, "");
        lv_span_set_text(sp_eta_z, "");
    }
    if (v->have_tod) {
        lv_span_set_text(sp_tod_l, "\xE2\x86\x98");   // U+2198 TOD icon
        lv_span_set_text(sp_tod_t, v->tod_hhmm);
        lv_span_set_text(sp_tod_z, "z  ");            // trailing gap to the ETA
    } else {
        lv_span_set_text(sp_tod_l, "");
        lv_span_set_text(sp_tod_t, "");
        lv_span_set_text(sp_tod_z, "");
    }
    lv_spangroup_refresh(sg_eta);

    // the whole badge hides with the estimate — an empty frame would linger
    vis(sg_leta, v->have_leta);
    if (v->have_leta) {
        lv_span_set_text(sp_leta_t, v->leta_hhmm);
        lv_span_set_text(sp_leta_l, "L");
        lv_spangroup_refresh(sg_leta);
    }
}

// Wi-Fi fan — signal bars, weakest arc innermost:
//   slow red blink (all bars)   = no uplink connection
//   fast orange blink (all)     = scanning
//   1 bar orange / 2 dimmed     = connected, weak    (RSSI < -70 dBm)
//   2 bars yellow / 1 dimmed    = connected, medium  (-70 .. -60)
//   3 bars green                = connected, strong  (>= -60)
// Cloud: violet = internet reachable, red = not.
// Upload: magenta flash whenever a location is RECEIVED from the feed.
static void tdisp_status(const fv_status_t *s)
{
    uint32_t lit;
    int nlit = 3;
    switch (s->link) {
    case FV_LINK_SCANNING: lit = COL_AMBER; break;
    case FV_LINK_UP:
        nlit = s->bars;
        lit  = s->bars >= 3 ? COL_GREEN : (s->bars == 2 ? COL_YELLOW : COL_AMBER);
        break;
    default:               lit = COL_RED; break;
    }
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_arc_color(s_wifi_arc[i],
                                   lv_color_hex(i < nlit ? lit : COL_DIMMED), LV_PART_MAIN);
        lv_obj_set_style_opa(s_wifi_arc[i], s->blink_on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    lv_obj_set_style_bg_color(s_wifi_dot, lv_color_hex(lit), 0);
    lv_obj_set_style_opa(s_wifi_dot, s->blink_on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

    lv_obj_set_style_text_color(s_globe, lv_color_hex(s->internet ? COL_INET : COL_RED), 0);
    lv_obj_set_style_text_color(s_feed,
                                lv_color_hex(s->feed_active ? COL_MAGENTA : COL_DIMMED), 0);
}

const layout_drv_t layout_tdisplay = {
    .name = "tdisplay-320x170",
    .build = tdisp_build,
    .render = tdisp_render,
    .status = tdisp_status,
};

#endif // CONFIG_IDF_TARGET_ESP32S3
