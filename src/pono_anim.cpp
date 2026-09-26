// SPDX-License-Identifier: GPL-3.0-only
// pono_anim.cpp - canned animation helpers. See pono_anim.h.
#include "pono_anim.h"
#include "pono_theme.h"
#include <cstdio>  // sscanf: parse "Full Cal X/N" for the banner progress strip
#include <cmath>   // cos/sin: the Make Pono orbit path

namespace {
// Singleton "working" overlay, lazily built on lv_layer_top.
lv_obj_t *g_busy = nullptr;
lv_obj_t *g_busy_spinner = nullptr;
lv_obj_t *g_busy_label = nullptr;
lv_timer_t *g_busy_watchdog = nullptr;  // force-drops a stranded "working" scrim
}  // namespace

namespace pono {

lv_obj_t *spinner_create(lv_obj_t *parent, uint16_t period_ms) {
  lv_obj_t *a = lv_animimg_create(parent);
  // lv_animimg stores the frame count in an int8_t (pic_count); clamp so a
  // future frame-count bump past 127 can't wrap it negative and read OOB in the
  // index modulo. At the current 24 frames this is a no-op.
  uint8_t n = pono_spinner_frame_count > 127 ? 127 : pono_spinner_frame_count;
  lv_animimg_set_src(a, (const void **)pono_spinner_frames, n);
  lv_animimg_set_duration(a, period_ms);
  lv_animimg_set_repeat_count(a, LV_ANIM_REPEAT_INFINITE);
  lv_animimg_start(a);
  // lv_animimg is an lv_img; size to the frame so callers can align it.
  lv_obj_set_size(a, pono_spinner_frames[0]->header.w, pono_spinner_frames[0]->header.h);
  return a;
}

namespace {
// Hand the catch-the-wind intro off to the steady loop. The intro's last frame
// matches loop frame 0 (same small amplitude), so swapping the source in place
// is invisible. One-shot: the timer auto-deletes after this fires.
void boot_flag_settle_cb(lv_timer_t *t) {
  lv_obj_t *im = (lv_obj_t *)t->user_data;
  lv_obj_set_user_data(im, nullptr);   // clear the back-ref; this timer is going away
  uint8_t nl = pono_flag_boot_frame_count > 127 ? 127 : pono_flag_boot_frame_count;
  lv_animimg_set_src(im, (const void **)pono_flag_boot_frames, nl);
  lv_animimg_set_duration(im, (uint32_t)nl * 1000u / 60u);
  lv_animimg_set_repeat_count(im, LV_ANIM_REPEAT_INFINITE);
  lv_animimg_start(im);
}
// If the flag is deleted before the gust settles, kill the pending swap timer.
void boot_flag_del_cb(lv_event_t *e) {
  lv_obj_t *im = lv_event_get_target(e);
  lv_timer_t *t = (lv_timer_t *)lv_obj_get_user_data(im);
  if (t) lv_timer_del(t);
}
}  // namespace

lv_obj_t *boot_flag_create(lv_obj_t *parent) {
  lv_obj_t *a = lv_animimg_create(parent);
  // Catch the wind: play the decaying-amplitude intro once, then settle into
  // the steady loop. lv_animimg pic_count is int8_t; both sets are under 127.
  uint8_t ni = pono_flag_boot_intro_frame_count > 127 ? 127 : pono_flag_boot_intro_frame_count;
  lv_animimg_set_src(a, (const void **)pono_flag_boot_intro_frames, ni);
  uint32_t intro_ms = (uint32_t)ni * 1000u / 60u;   // ~60 fps
  lv_animimg_set_duration(a, intro_ms);
  lv_animimg_set_repeat_count(a, 1);
  lv_animimg_start(a);
  lv_obj_set_size(a, pono_flag_boot_intro_frames[0]->header.w,
                  pono_flag_boot_intro_frames[0]->header.h);
  // Swap to the loop when the gust finishes; tie the one-shot timer to the
  // flag's lifetime so it can never fire on a freed object.
  lv_timer_t *t = lv_timer_create(boot_flag_settle_cb, intro_ms, a);
  lv_timer_set_repeat_count(t, 1);
  lv_obj_set_user_data(a, t);
  lv_obj_add_event_cb(a, boot_flag_del_cb, LV_EVENT_DELETE, nullptr);
  return a;
}

// A blocking op whose completion reply never arrives (a Klipper error on a
// still-open socket, a macro that never returns) would otherwise leave the
// scrim up forever. busy_show arms a one-shot watchdog that force-drops it;
// busy_hide (the normal completion path, and the disconnect reset) cancels it.
static void busy_cancel_watchdog() {
  if (g_busy_watchdog) { lv_timer_del(g_busy_watchdog); g_busy_watchdog = nullptr; }
}
static void busy_watchdog_cb(lv_timer_t *) {
  g_busy_watchdog = nullptr;  // LVGL auto-frees this finished one-shot timer
  busy_hide();
}

void busy_show(const char *text) {
  if (g_busy == nullptr) {
    g_busy = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_busy);
    lv_obj_set_size(g_busy, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_busy, color_surface_base, 0);
    lv_obj_set_style_bg_opa(g_busy, LV_OPA_90, 0);  // scrim swallows taps behind it
    lv_obj_clear_flag(g_busy, LV_OBJ_FLAG_SCROLLABLE);
    g_busy_spinner = spinner_create(g_busy, 1000);
    lv_obj_align(g_busy_spinner, LV_ALIGN_CENTER, 0, -16);
    g_busy_label = lv_label_create(g_busy);
    lv_obj_set_style_text_color(g_busy_label, color_text_primary, 0);
    lv_obj_set_style_text_font(g_busy_label, font_body, 0);
    lv_obj_set_style_text_align(g_busy_label, LV_TEXT_ALIGN_CENTER, 0);
    // Wrap long status lines instead of letting them run off the 480px panel.
    lv_obj_set_width(g_busy_label, 440);
    lv_label_set_long_mode(g_busy_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(g_busy_label, LV_ALIGN_CENTER, 0, 50);
  }
  lv_label_set_text(g_busy_label, text != nullptr ? text : "Working");
  lv_obj_align(g_busy_label, LV_ALIGN_CENTER, 0, 50);  // re-center after (re)wrap
  lv_animimg_start(g_busy_spinner);  // re-arm (busy_hide stopped it)
  lv_obj_clear_flag(g_busy, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(g_busy);
  // (Re)arm the stranded-scrim watchdog. 180s comfortably outlasts the longest
  // real blocking op here (a full-length filament load), so it only fires on a
  // genuine hang and never cuts a healthy op short.
  busy_cancel_watchdog();
  g_busy_watchdog = lv_timer_create(busy_watchdog_cb, 180000, nullptr);
  lv_timer_set_repeat_count(g_busy_watchdog, 1);
}

void busy_hide() {
  busy_cancel_watchdog();
  if (g_busy == nullptr) return;
  lv_anim_del(g_busy_spinner, nullptr);  // stop the loop -> zero cost while idle
  lv_obj_add_flag(g_busy, LV_OBJ_FLAG_HIDDEN);
}

namespace {
// Singleton OMEGA status banner, lazily built on lv_layer_top.
lv_obj_t *g_omega = nullptr;
lv_obj_t *g_omega_label = nullptr;
lv_obj_t *g_omega_bar = nullptr;  // campaign progress strip along the banner's bottom edge
}  // namespace

void omega_status_show(const char *text) {
  if (g_omega == nullptr) {
    g_omega = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_omega);
    // The whole header row, so the E-STOP sits inside the strip instead of
    // hanging off its bottom edge.
    lv_obj_set_size(g_omega, LV_PCT(100), estop_clear_y);
    lv_obj_set_pos(g_omega, 0, 0);
    lv_obj_set_style_bg_color(g_omega, color_state_warning, 0);  // OMEGA amber
    lv_obj_set_style_bg_opa(g_omega, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_omega, LV_OBJ_FLAG_SCROLLABLE);
    g_omega_label = lv_label_create(g_omega);
    lv_obj_set_style_text_color(g_omega_label, color_surface_base, 0);  // dark on amber
    lv_obj_set_style_text_font(g_omega_label, font_body, 0);
    lv_obj_set_style_text_align(g_omega_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_omega_label, LV_LABEL_LONG_DOT);  // ellipsize a long step
    lv_obj_set_width(g_omega_label, estop_clear_x - 4 - 12);  // a long step ends before the E-STOP
    // One line high: LV_LABEL_LONG_DOT honours height too, so an auto height
    // wraps a long step onto a second line instead of ellipsizing it.
    lv_obj_set_height(g_omega_label, lv_font_get_line_height(font_body));
    lv_obj_align(g_omega_label, LV_ALIGN_LEFT_MID, 12, -2);  // clear the bar strip below
    g_omega_bar = lv_obj_create(g_omega);
    lv_obj_remove_style_all(g_omega_bar);
    lv_obj_clear_flag(g_omega_bar, LV_OBJ_FLAG_CLICKABLE);  // a readout, not a control
    lv_obj_set_style_bg_color(g_omega_bar, color_surface_base, 0);  // dark on amber, like the text
    lv_obj_set_style_bg_opa(g_omega_bar, LV_OPA_COVER, 0);
    lv_obj_set_pos(g_omega_bar, 0, estop_clear_y - 4);
    lv_obj_set_size(g_omega_bar, 0, 4);
    lv_obj_add_flag(g_omega_bar, LV_OBJ_FLAG_HIDDEN);
  }
  lv_label_set_text(g_omega_label, text != nullptr ? text : "Full Cal");
  // "Full Cal X/N: ..." carries the campaign position; render it as a width.
  int done = 0, total = 0;
  if (text != nullptr && std::sscanf(text, "Full Cal %d/%d", &done, &total) == 2 && total > 0) {
    if (done < 0) done = 0;
    if (done > total) done = total;
    lv_obj_set_size(g_omega_bar, (lv_coord_t)((480 * done) / total), 4);
    lv_obj_clear_flag(g_omega_bar, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(g_omega_bar, LV_OBJ_FLAG_HIDDEN);  // no count, no fake bar
  }
  lv_obj_clear_flag(g_omega, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(g_omega);
}

void omega_status_hide() {
  if (g_omega == nullptr) return;
  lv_obj_add_flag(g_omega, LV_OBJ_FLAG_HIDDEN);
}

namespace {
// The Make Pono orbit geometry, baked for the Tune cluster (two tiers up top +
// five tiles below). One orbit per screen, so the path needs no per-instance
// state: the exec_cb reads these constants. Center + radii trace the perimeter
// of the options on the 480x272 panel.
constexpr double ORBIT_CX = 240.0, ORBIT_CY = 134.0;
constexpr double ORBIT_RX = 226.0, ORBIT_RY =  86.0;
// v is the angle in tenths of a degree (0..3600): constant angular speed on an
// ellipse gives a varying linear speed (quicker along the long axis), which
// reads as an organic round, and the 0==3600 wrap stays invisible.
void tune_orbit_exec_cb(void *obj, int32_t v) {
  lv_obj_t *dot = (lv_obj_t *)obj;
  double rad = (double)v * 0.1 * 0.017453292519943295;  // deg -> rad
  lv_coord_t w = lv_obj_get_width(dot), hh = lv_obj_get_height(dot);
  lv_obj_set_pos(dot,
    (lv_coord_t)(ORBIT_CX + ORBIT_RX * std::cos(rad)) - w / 2,
    (lv_coord_t)(ORBIT_CY + ORBIT_RY * std::sin(rad)) - hh / 2);
}
}  // namespace

lv_obj_t *tune_orbit_create(lv_obj_t *parent) {
  lv_obj_t *dot = lv_obj_create(parent);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 12, 12);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, color_accent_primary, 0);   // the lamp (amber)
  lv_obj_set_style_bg_opa(dot, LV_OPA_60, 0);
  // The bloom: a wide, low-opacity amber shadow = the porch lamp's glow.
  lv_obj_set_style_shadow_color(dot, color_accent_primary, 0);
  lv_obj_set_style_shadow_width(dot, 26, 0);
  lv_obj_set_style_shadow_opa(dot, LV_OPA_40, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_move_background(dot);  // behind the cards: grazes the panel, never text
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, dot);
  lv_anim_set_exec_cb(&a, tune_orbit_exec_cb);
  lv_anim_set_values(&a, 0, 3600);
  lv_anim_set_time(&a, 6000);                       // a calm 6s round
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_linear);     // linear angle: the wrap is invisible
  lv_anim_start(&a);
  return dot;
}

namespace {
// Make Pono narration "logbook" box, lazily built on lv_layer_top.
lv_obj_t *g_callog = nullptr, *g_callog_now = nullptr, *g_callog_next = nullptr;
lv_obj_t *g_callog_count = nullptr, *g_callog_bar = nullptr, *g_callog_rule = nullptr;
lv_obj_t *g_callog_stop = nullptr;
bool g_callog_stop_wired = false;
}  // namespace

void cal_log_show(const char *now, const char *next, int done, int total,
                  bool fault, lv_event_cb_t stop_cb, void *stop_ud) {
  if (g_callog == nullptr) {
    const lv_coord_t H = 70, Y = 272 - H;
    g_callog = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_callog);
    lv_obj_set_pos(g_callog, 0, Y);
    lv_obj_set_size(g_callog, 480, H);
    lv_obj_set_style_bg_color(g_callog, color_surface_raised, 0);
    lv_obj_set_style_bg_opa(g_callog, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_callog, LV_OBJ_FLAG_SCROLLABLE);
    // top hairline (the lamp's edge; turns alarm on a lost-contact fault)
    g_callog_rule = lv_obj_create(g_callog);
    lv_obj_remove_style_all(g_callog_rule);
    lv_obj_set_pos(g_callog_rule, 0, 0);
    lv_obj_set_size(g_callog_rule, 480, 2);
    lv_obj_set_style_bg_opa(g_callog_rule, LV_OPA_COVER, 0);
    // "MAKE PONO" caps tag (instrument lettering)
    lv_obj_t *tg = lv_label_create(g_callog);
    lv_obj_set_style_text_color(tg, color_accent_secondary, 0);
    lv_obj_set_style_text_font(tg, font_micro, 0);
    lv_obj_set_style_text_letter_space(tg, 1, 0);
    lv_label_set_text(tg, "MAKE PONO");
    lv_obj_set_pos(tg, 12, 10);
    // count "5/40"
    g_callog_count = lv_label_create(g_callog);
    lv_obj_set_style_text_color(g_callog_count, color_accent_primary, 0);
    lv_obj_set_style_text_font(g_callog_count, font_caption, 0);
    lv_obj_align(g_callog_count, LV_ALIGN_TOP_RIGHT, -12, 10);
    // NOW (the running step) - phosphor, the live readout
    g_callog_now = lv_label_create(g_callog);
    lv_obj_set_style_text_font(g_callog_now, font_body, 0);
    lv_label_set_long_mode(g_callog_now, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_callog_now, 348);
    lv_obj_set_pos(g_callog_now, 12, 26);
    // NEXT (what is coming) - dim
    g_callog_next = lv_label_create(g_callog);
    lv_obj_set_style_text_color(g_callog_next, color_text_secondary, 0);
    lv_obj_set_style_text_font(g_callog_next, font_caption, 0);
    lv_label_set_long_mode(g_callog_next, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_callog_next, 348);
    lv_obj_set_pos(g_callog_next, 12, 46);
    // STOP - always reachable; friction goes on the dangerous act, never the exit
    g_callog_stop = lv_obj_create(g_callog);
    lv_obj_remove_style_all(g_callog_stop);
    lv_obj_set_size(g_callog_stop, 84, 40);
    lv_obj_align(g_callog_stop, LV_ALIGN_RIGHT_MID, -12, 4);
    lv_obj_set_style_bg_color(g_callog_stop, color_surface_base, 0);
    lv_obj_set_style_bg_opa(g_callog_stop, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_callog_stop, radius_sm, 0);
    lv_obj_set_style_border_color(g_callog_stop, color_state_error, 0);
    lv_obj_set_style_border_width(g_callog_stop, 1, 0);
    lv_obj_set_style_border_opa(g_callog_stop, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_callog_stop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *sl = lv_label_create(g_callog_stop);
    lv_obj_set_style_text_color(sl, color_state_error, 0);
    lv_obj_set_style_text_font(sl, font_caption, 0);
    lv_obj_set_style_text_letter_space(sl, 1, 0);
    lv_label_set_text(sl, "STOP");
    lv_obj_center(sl);
    // progress bar along the bottom edge
    g_callog_bar = lv_obj_create(g_callog);
    lv_obj_remove_style_all(g_callog_bar);
    lv_obj_set_style_bg_color(g_callog_bar, color_accent_primary, 0);
    lv_obj_set_style_bg_opa(g_callog_bar, LV_OPA_COVER, 0);
    lv_obj_set_pos(g_callog_bar, 0, H - 3);
    lv_obj_set_size(g_callog_bar, 0, 3);
  }
  // Wire STOP once (the app owns the stop gcode; pono_anim has no ws handle).
  if (!g_callog_stop_wired && stop_cb != nullptr) {
    lv_obj_add_event_cb(g_callog_stop, stop_cb, LV_EVENT_CLICKED, stop_ud);
    g_callog_stop_wired = true;
  }
  // NOW: the running step, or the lost-contact alarm on a fault.
  lv_obj_set_style_bg_color(g_callog_rule, fault ? color_state_error : color_accent_primary, 0);
  lv_obj_set_style_text_color(g_callog_now, fault ? color_state_error : color_accent_secondary, 0);
  lv_label_set_text(g_callog_now, now != nullptr ? now : "");
  lv_label_set_text(g_callog_next, next != nullptr ? next : "");
  if (total > 0) {
    if (done < 0) done = 0;
    if (done > total) done = total;
    lv_label_set_text_fmt(g_callog_count, "%d/%d", done, total);
    lv_obj_set_size(g_callog_bar, (lv_coord_t)((480 * done) / total), 3);
    lv_obj_clear_flag(g_callog_bar, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(g_callog_count, "");
    lv_obj_add_flag(g_callog_bar, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_clear_flag(g_callog, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(g_callog);
}

void cal_log_hide() {
  if (g_callog == nullptr) return;
  lv_obj_add_flag(g_callog, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace pono
