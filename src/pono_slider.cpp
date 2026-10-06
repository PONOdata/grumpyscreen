// pono_slider.cpp - fine-drag slider and its live value bubble.
//
// All handlers are registered with LV_EVENT_PREPROCESS so they run before the
// lv_slider and lv_obj class handlers. Stopping PRESSED kills the slider's
// jump-to-touch; stopping PRESSING keeps the class from moving the value.
// Because the base class never sees PRESSED, LV_STATE_PRESSED is managed here.
// Every handler runs on the LVGL loop with the UI mutex already held: no locks.

#include "pono_slider.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "pono_theme.h"
#include "pono_anim.h"

namespace pono {
namespace {

constexpr int32_t    kTapSlop = 6;    // px of travel before a press is a drag
constexpr double     kFilter  = 0.5;  // low-pass weight on the raw point
constexpr double     kHyst    = 6.0;  // px of hysteresis at each gain edge
constexpr lv_coord_t kMargin  = 4;    // bubble keeps this far from screen edges
constexpr lv_coord_t kAbove   = 16;   // gap between bubble bottom and finger
constexpr lv_coord_t kSide    = 36;   // gap beside the finger; a pad is 60-70px wide, 36 clears half

// Distance from the bar centre at which each gain level starts.
const double kEdge[4] = {0.0, 30.0, 70.0, 110.0};
const char *const kTag[4] = {"UP FOR FINE", "FINE 1/2", "FINE 1/4", "FINE 1/8"};

struct FineSliderCfg {
  char unit[8];
};

// One finger, one session.
struct DragSession {
  lv_obj_t *slider;  // null when no fine slider is held
  const FineSliderCfg *cfg;
  int32_t v0;
  double acc;
  lv_coord_t x0;
  lv_coord_t y0;
  double fx;
  double fy;
  int level;
};

DragSession g_sess = {nullptr, nullptr, 0, 0.0, 0, 0, 0.0, 0.0, 0};
// Travel of the last session; kept until the next press so CLICKED and
// SHORT_CLICKED (which follow RELEASED) can tell a tap from a drag.
int32_t g_travel = 0;

// The bubble: created lazily on lv_layer_top() and reused.
lv_obj_t *g_bub      = nullptr;
lv_obj_t *g_bub_val  = nullptr;
lv_obj_t *g_bub_unit = nullptr;
lv_obj_t *g_bub_tag  = nullptr;
int32_t g_shown_v = INT32_MIN;
int g_shown_level = -1;
const FineSliderCfg *g_shown_cfg = nullptr;

void session_end() {
  g_sess.slider = nullptr;
  g_sess.cfg = nullptr;
}

int level_for(double d, int cur) {
  int lv = cur;
  while (lv < 3 && d >= kEdge[lv + 1]) lv++;
  while (lv > 0 && d < kEdge[lv] - kHyst) lv--;
  return lv;
}

double bar_centre_y(lv_obj_t *sl) {
  lv_area_t a;
  lv_obj_get_coords(sl, &a);
  return (a.y1 + a.y2) / 2.0;
}

bool read_point(lv_point_t *p) {
  lv_indev_t *in = lv_indev_get_act();
  if (!in) return false;
  lv_indev_get_point(in, p);
  return true;
}

void bubble_deleted_cb(lv_event_t *) {
  g_bub = nullptr;
  g_bub_val = nullptr;
  g_bub_unit = nullptr;
  g_bub_tag = nullptr;
}

lv_obj_t *plain_box(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  return o;
}

lv_obj_t *bubble_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text_static(l, "");
  return l;
}

void bubble_create() {
  g_bub = plain_box(lv_layer_top());
  lv_obj_add_flag(g_bub, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(g_bub, color_surface_raised, 0);
  lv_obj_set_style_bg_opa(g_bub, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(g_bub, color_accent_primary, 0);
  lv_obj_set_style_border_opa(g_bub, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(g_bub, 1, 0);
  lv_obj_set_style_radius(g_bub, radius_sm, 0);
  lv_obj_set_style_shadow_width(g_bub, 0, 0);
  lv_obj_set_style_pad_hor(g_bub, 6, 0);
  lv_obj_set_style_pad_ver(g_bub, 4, 0);
  lv_obj_set_style_pad_row(g_bub, 1, 0);
  lv_obj_set_flex_flow(g_bub, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(g_bub, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_add_event_cb(g_bub, bubble_deleted_cb, LV_EVENT_DELETE, nullptr);

  // Line 1: value in the lamp colour with the unit beside it.
  lv_obj_t *row = plain_box(g_bub);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(row, 3, 0);
  g_bub_val  = bubble_label(row, font_num_medium, color_accent_primary);
  g_bub_unit = bubble_label(row, font_caption, color_text_secondary);

  // Line 2: the gain tag in phosphor.
  g_bub_tag = bubble_label(g_bub, font_micro, color_accent_secondary);

  g_shown_v = INT32_MIN;
  g_shown_level = -1;
  g_shown_cfg = nullptr;
}

// Set texts only when they change; the value is formatted on the stack.
void bubble_fill(int32_t v, int level, const FineSliderCfg *cfg) {
  if (v != g_shown_v) {
    char b[16];
    snprintf(b, sizeof b, "%d", (int)v);
    lv_label_set_text(g_bub_val, b);
    g_shown_v = v;
  }
  if (cfg != g_shown_cfg) {
    lv_label_set_text(g_bub_unit, cfg ? cfg->unit : "");
    g_shown_cfg = cfg;
  }
  if (level != g_shown_level) {
    lv_label_set_text_static(g_bub_tag, kTag[level]);
    g_shown_level = level;
  }
}

// Bubble goes above the finger when it fits. Near the top edge it goes beside
// the fingertip at the same height, on the side away from the near screen edge.
// Below the finger would be under the hand.
void bubble_place(double fx, double fy) {
  lv_obj_update_layout(g_bub);
  lv_coord_t w  = lv_obj_get_width(g_bub);
  lv_coord_t h  = lv_obj_get_height(g_bub);
  lv_coord_t sw = lv_disp_get_hor_res(NULL);
  lv_coord_t sh = lv_disp_get_ver_res(NULL);
  lv_coord_t px = (lv_coord_t)lround(fx);
  lv_coord_t py = (lv_coord_t)lround(fy);

  lv_coord_t x, y;
  if (py - kAbove - h >= kMargin) {
    x = px - w / 2;
    y = py - kAbove - h;
  } else {
    x = (px > sw / 2) ? px - kSide - w : px + kSide;
    y = py - h / 2;
  }
  if (x > sw - kMargin - w) x = sw - kMargin - w;
  if (x < kMargin) x = kMargin;
  if (y > sh - kMargin - h) y = sh - kMargin - h;
  if (y < kMargin) y = kMargin;

  // Never enter the E-STOP reach.
  if (x + w > estop_clear_x && y < estop_clear_y) x = estop_clear_x - w;

  lv_obj_set_pos(g_bub, x, y);
}

void bubble_show(int32_t v, int level, const FineSliderCfg *cfg, double fx, double fy) {
  if (!g_bub) bubble_create();
  if (!g_bub) return;
  bubble_fill(v, level, cfg);
  lv_obj_clear_flag(g_bub, LV_OBJ_FLAG_HIDDEN);
  raise_overlay(g_bub);
  bubble_place(fx, fy);
}

void bubble_update(int32_t v, int level, const FineSliderCfg *cfg, double fx, double fy) {
  if (!g_bub) return;
  bubble_fill(v, level, cfg);
  bubble_place(fx, fy);
}

void bubble_hide() {
  if (g_bub) lv_obj_add_flag(g_bub, LV_OBJ_FLAG_HIDDEN);
}

// ---- event handlers ----

void on_pressed(lv_event_t *e) {
  lv_obj_t *sl = lv_event_get_target(e);
  const FineSliderCfg *cfg = (const FineSliderCfg *)lv_event_get_user_data(e);

  lv_point_t p = {0, 0};
  if (!read_point(&p)) {
    lv_area_t a;
    lv_obj_get_coords(sl, &a);
    p.x = (lv_coord_t)((a.x1 + a.x2) / 2);
    p.y = (lv_coord_t)((a.y1 + a.y2) / 2);
  }

  g_sess.slider = sl;
  g_sess.cfg = cfg;
  g_sess.v0 = lv_slider_get_value(sl);
  g_sess.acc = (double)g_sess.v0;
  g_sess.x0 = p.x;
  g_sess.y0 = p.y;
  g_sess.fx = p.x;
  g_sess.fy = p.y;
  g_sess.level = level_for(std::fabs(g_sess.fy - bar_centre_y(sl)), 0);
  g_travel = 0;

  lv_obj_add_state(sl, LV_STATE_PRESSED);
  lv_obj_invalidate(sl);
  bubble_show(g_sess.v0, g_sess.level, cfg, g_sess.fx, g_sess.fy);

  lv_event_stop_processing(e);  // no jump to the touch point
}

void on_pressing(lv_event_t *e) {
  lv_obj_t *sl = lv_event_get_target(e);
  lv_point_t p = {0, 0};
  if (g_sess.slider != sl || !read_point(&p)) {
    lv_event_stop_processing(e);
    return;
  }

  double prev_fx = g_sess.fx;
  g_sess.fx += kFilter * (p.x - g_sess.fx);
  g_sess.fy += kFilter * (p.y - g_sess.fy);
  int32_t t = std::abs(p.x - g_sess.x0) + std::abs(p.y - g_sess.y0);
  if (t > g_travel) g_travel = t;
  if (g_travel < kTapSlop) {  // a tap never moves the value
    lv_event_stop_processing(e);
    return;
  }

  g_sess.level = level_for(std::fabs(g_sess.fy - bar_centre_y(sl)), g_sess.level);
  double gain = 1.0 / (double)(1 << g_sess.level);

  int32_t mn = lv_slider_get_min_value(sl);
  int32_t mx = lv_slider_get_max_value(sl);
  lv_coord_t cw = lv_obj_get_content_width(sl);
  if (cw < 1) cw = 1;
  g_sess.acc += (g_sess.fx - prev_fx) * ((double)(mx - mn) / (double)cw) * gain;
  if (g_sess.acc < mn) g_sess.acc = mn;
  if (g_sess.acc > mx) g_sess.acc = mx;
  int32_t v = (int32_t)lround(g_sess.acc);

  if (v != lv_slider_get_value(sl)) {
    lv_slider_set_value(sl, v, LV_ANIM_OFF);
    if (lv_event_send(sl, LV_EVENT_VALUE_CHANGED, NULL) != LV_RES_OK) {
      lv_event_stop_processing(e);  // slider deleted by a handler
      return;
    }
  }
  if (g_sess.slider == sl) {
    bubble_update(lv_slider_get_value(sl), g_sess.level, g_sess.cfg, g_sess.fx, g_sess.fy);
  }
  lv_event_stop_processing(e);
}

// RELEASED and PRESS_LOST.
void on_released(lv_event_t *e) {
  lv_obj_t *sl = lv_event_get_target(e);
  lv_obj_clear_state(sl, LV_STATE_PRESSED);
  lv_obj_invalidate(sl);
  bubble_hide();
  if (g_sess.slider != sl) return;  // no session: pass through
  bool unchanged = lv_slider_get_value(sl) == g_sess.v0;
  session_end();
  if (unchanged) lv_event_stop_processing(e);  // nothing to commit
}

// SHORT_CLICKED and CLICKED: a drag is not a tap.
void on_click(lv_event_t *e) {
  if (g_travel >= kTapSlop) lv_event_stop_processing(e);
}

void on_delete(lv_event_t *e) {
  lv_obj_t *sl = lv_event_get_target(e);
  FineSliderCfg *cfg = (FineSliderCfg *)lv_event_get_user_data(e);
  if (g_sess.slider == sl) {
    session_end();
    bubble_hide();
  }
  if (g_shown_cfg == cfg) g_shown_cfg = nullptr;
  delete cfg;
}

void add_pre(lv_obj_t *sl, lv_event_cb_t cb, lv_event_code_t code, void *ud) {
  lv_obj_add_event_cb(sl, cb, (lv_event_code_t)(code | LV_EVENT_PREPROCESS), ud);
}

}  // namespace

void fine_slider_attach(lv_obj_t *slider, const char *unit) {
  if (!slider) return;
  FineSliderCfg *cfg = new FineSliderCfg;
  snprintf(cfg->unit, sizeof cfg->unit, "%s", unit ? unit : "");

  // A vertical roll toward fine gain must never reach a parent's swipe.
  lv_obj_clear_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);

  add_pre(slider, on_pressed, LV_EVENT_PRESSED, cfg);
  add_pre(slider, on_pressing, LV_EVENT_PRESSING, cfg);
  add_pre(slider, on_released, LV_EVENT_RELEASED, cfg);
  add_pre(slider, on_released, LV_EVENT_PRESS_LOST, cfg);
  add_pre(slider, on_click, LV_EVENT_SHORT_CLICKED, cfg);
  add_pre(slider, on_click, LV_EVENT_CLICKED, cfg);
  lv_obj_add_event_cb(slider, on_delete, LV_EVENT_DELETE, cfg);
}

}  // namespace pono
