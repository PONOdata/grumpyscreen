// SPDX-License-Identifier: GPL-3.0-only
// pono_home.cpp - Pono Print home cockpit builder (see pono_home.h)
//
// Kukui on glass: the printer's screen is the engine-room gauge
// panel of the night bridge. Warm black room, one amber lamp on the action
// at hand, phosphor needles for every live number, mono caps for every
// label, hairline rules, machined corners. The word pono keeps the log
// while the machine is idle (the dictionary entry is the idle hero).
//
// Pure LVGL v8 + pono_theme tokens, designed natively for the printer's real
// 480x272. Laid out on ONE grid: 12px outer margin, a top bar, a two-column
// main zone (hero + readouts/actions), and a bottom nav bar.
//
// Within a 2-core ARMv7 software renderer (no GPU): flat fills only (no
// gradients to composite), one hero arc while printing, heat-aware
// instruments, and exactly one tiny idle pulse on the live dot. No
// perpetual full-area animation.

#include "pono_home.h"
#include "pono_theme.h"
#include "pono_anim.h"   // Hawaii flag asset + comet spinner for the boot screen

#include <cstdio>
#include <cstring>
#include <ctime>

namespace pono {

// ---- small builders -------------------------------------------------------

static lv_obj_t *card(lv_obj_t *p, int x, int y, int w, int h,
                      lv_color_t bg, int radius = radius_sm,
                      lv_opa_t opa = LV_OPA_COVER) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, bg, 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t *lbl(lv_obj_t *p, const char *txt, const lv_font_t *font,
                     lv_color_t color, int x, int y) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_obj_set_pos(l, x, y);
  return l;
}

// Letterspaced caps tag: the instrument label voice. Author text UPPERCASE.
static lv_obj_t *tag(lv_obj_t *p, const char *txt, lv_color_t color, int x, int y) {
  lv_obj_t *l = lbl(p, txt, font_micro, color, x, y);
  lv_obj_set_style_text_letter_space(l, track_caps, 0);
  return l;
}

// 1px hairline border: ink over the room at a quiet opacity. The engineering-
// drawing rule that replaces gradients and glows everywhere.
static void hairline(lv_obj_t *o, lv_opa_t opa = opa_border_subtle) {
  lv_obj_set_style_border_color(o, color_text_primary, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_opa(o, opa, 0);
}

// Hairline in a stated color (state pills, alarm outlines).
static void hairline_c(lv_obj_t *o, lv_color_t color, lv_opa_t opa) {
  lv_obj_set_style_border_color(o, color, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_opa(o, opa, 0);
}

// Panel: the standard raised surface with a resting hairline.
static lv_obj_t *panel(lv_obj_t *p, int x, int y, int w, int h,
                       lv_opa_t border = opa_border_subtle) {
  lv_obj_t *o = card(p, x, y, w, h, color_surface_raised);
  hairline(o, border);
  return o;
}

// Solid lamp switch: THE primary action. Dark text on lit amber.
static lv_obj_t *lamp_btn(lv_obj_t *p, int x, int y, int w, int h,
                          const char *txt, const lv_font_t *f) {
  lv_obj_t *b = card(p, x, y, w, h, color_accent_primary);
  lv_obj_t *l = lbl(b, txt, f, color_surface_base, 0, 0);
  lv_obj_center(l);
  return b;
}

// Phosphor bloom pulse on a small live indicator - the one idle-state
// animation. Authentic CRT bloom on the instrument's heartbeat; decorative
// glows elsewhere are banned, this is the documented exception.
static void bloom_pulse(lv_obj_t *o, lv_color_t color, int lo, int hi, uint32_t period) {
  lv_obj_set_style_shadow_color(o, color, 0);
  lv_obj_set_style_shadow_opa(o, LV_OPA_60, 0);
  lv_obj_set_style_shadow_spread(o, 0, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
    lv_obj_set_style_shadow_width((lv_obj_t *)obj, v, 0);
  });
  lv_anim_set_values(&a, lo, hi);
  lv_anim_set_time(&a, period);
  lv_anim_set_playback_time(&a, period);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

// Public: start/stop the PRINTING-pill beat (declared in pono_home.h). Call only
// on the idle<->printing transition - it deletes any running pulse first, so a
// per-frame call would reset and stutter the animation. Keeps the always-on
// idle dashboard at zero animation cost (no pulse runs while not printing).
void set_state_pulse(lv_obj_t *dot, bool on) {
  if (!dot) return;
  lv_anim_del(dot, nullptr);
  if (on) {
    bloom_pulse(dot, color_accent_primary, 3, 11, 850);
  } else {
    lv_obj_set_style_shadow_width(dot, 3, 0);  // settle to a calm static dot
  }
}

// Heat-aware temperature readout: "NOZZLE   248 / 250" (or "off" when idle).
// Fixed columns so a digit-count change on live update never shifts the layout.
static void temp_card(lv_obj_t *p, int x, int y, int w, int h, const char *name,
                      int val, int target,
                      lv_obj_t **o_card, lv_obj_t **o_val, lv_obj_t **o_set) {
  lv_obj_t *c = panel(p, x, y, w, h);
  lv_obj_t *nm = tag(c, name, color_text_secondary, 0, 0);
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 14, 0);
  // Live reading: phosphor when cool, lamp while heat is at work, alarm only
  // past the hotend's 280C rating (an actual overheat, not normal PA temps).
  lv_color_t vc = (val >= 280) ? color_state_error
                : (val >= 45)  ? color_accent_primary
                               : color_accent_secondary;
  char vb[12]; snprintf(vb, sizeof vb, "%d", val);
  lv_obj_t *v = lbl(c, vb, font_num_medium, vc, 0, 0);
  lv_obj_align(v, LV_ALIGN_LEFT_MID, 118, 0);
  char sb[16];
  if (target > 0) snprintf(sb, sizeof sb, "/ %d", target);
  else            snprintf(sb, sizeof sb, "off");
  lv_obj_t *s = lbl(c, sb, font_caption, color_text_tertiary, 0, 0);
  lv_obj_align(s, LV_ALIGN_RIGHT_MID, -14, 0);
  if (o_card) *o_card = c;
  if (o_val)  *o_val = v;
  if (o_set)  *o_set = s;
}

// Bottom-bar navigation tile: centered icon over a caps caption.
static lv_obj_t *nav_tile(lv_obj_t *p, int x, int y, int w, int h,
                          const char *icon, const char *name, const lv_font_t *ms) {
  lv_obj_t *t = panel(p, x, y, w, h);
  lv_obj_t *ic = lbl(t, icon, ms, color_text_secondary, 0, 0);
  lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 9);
  lv_obj_t *nl = tag(t, name, color_text_tertiary, 0, 0);
  lv_obj_align(nl, LV_ALIGN_BOTTOM_MID, 0, -7);
  return t;
}

// ---- the 43 ----------------------------------------------------------------
// Pukui-Elbert senses 1 and 2 of pono: 35 nvs. glosses then 8 vs. glosses.
// The canonical list (the deck's PONO array, kept in the same order).
static const char *GLOSS[43] = {
  "goodness", "uprightness", "morality", "moral qualities",
  "correct or proper procedure", "excellence", "well-being", "prosperity",
  "welfare", "benefit", "behalf", "equity", "sake",
  "true condition or nature", "duty", "moral", "fitting", "proper",
  "righteous", "right", "upright", "just", "virtuous", "fair",
  "beneficial", "successful", "in perfect order", "accurate", "correct",
  "eased", "relieved", "should", "ought", "must", "necessary",
  "completely", "properly", "rightly", "well", "exactly", "carefully",
  "satisfactorily", "much",
};
static const int GLOSS_NVS = 35;  // 0..34 nvs., 35..42 vs.

int gloss_count() { return 43; }

int gloss_today_index() {
  // One meaning a day, same seed as the deck: days-since-epoch modulo 43.
  long days = (long)(time(nullptr) / 86400);
  int ix = (int)(days % 43);
  return ix < 0 ? ix + 43 : ix;
}

const char *gloss_text(int ix) { return GLOSS[((ix % 43) + 43) % 43]; }
const char *gloss_pos(int ix)  { return (((ix % 43) + 43) % 43) < GLOSS_NVS ? "nvs." : "vs."; }

void home_set_stale(HomeHandles *h, int age_s) {
  if (!h || !h->stale) return;
  if (age_s < 0) {
    lv_obj_add_flag(h->stale, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  char b[28];
  snprintf(b, sizeof b, "READOUT STALE %ds", age_s);
  lv_label_set_text(h->stale, b);
  lv_obj_clear_flag(h->stale, LV_OBJ_FLAG_HIDDEN);
}

void home_set_gloss(HomeHandles *h, int ix) {
  if (!h) return;
  ix = ((ix % 43) + 43) % 43;
  if (h->def)    lv_label_set_text(h->def, GLOSS[ix]);
  if (h->defpos) lv_label_set_text(h->defpos, gloss_pos(ix));
  if (h->defn) {
    char b[12]; snprintf(b, sizeof b, "%02d / 43", ix + 1);
    lv_label_set_text(h->defn, b);
  }
}

// ---- model ----------------------------------------------------------------

HomeModel demo_home_model() {
  HomeModel m{};
  m.printing = true;
  m.progress_pct = 47;
  m.layer = 84;
  m.layer_total = 180;
  m.job_name = "omega_cube.gcode";
  m.material = "PA-CF . 0.25 diamond";
  m.nozzle = 248;
  m.nozzle_set = 250;
  m.bed = 60;
  m.bed_set = 60;
  m.eta = "1:12 left";
  return m;
}

HomeModel demo_home_paused_model() {
  HomeModel m = demo_home_model();
  m.paused = true;
  return m;
}

HomeModel demo_home_idle_model() {
  HomeModel m{};
  m.printing = false;
  m.progress_pct = 0;
  m.layer = 0;
  m.layer_total = 0;
  m.job_name = "";
  m.material = "PA-CF . 0.25 diamond";
  m.nozzle = 32;
  m.nozzle_set = 0;
  m.bed = 32;
  m.bed_set = 0;
  m.eta = "";
  return m;
}

// Forward decls: sub-screen chrome is defined in the native section below,
// but build_tune (above it) references screen_header.
static lv_obj_t *screen_header(lv_obj_t *parent, const char *title);
static lv_obj_t *tap_btn(lv_obj_t *p, int x, int y, int w, int h,
                         const char *txt, const lv_font_t *f, lv_color_t tc);

// ---- the cockpit -----------------------------------------------------------

lv_obj_t *build_home(lv_obj_t *parent, const HomeModel &m, HomeHandles *out) {
  const lv_font_t *ms = &lv_font_montserrat_14; // built-in: carries LV_SYMBOL_*
  const bool pr = m.printing;

  // ---- one grid for the whole screen ----
  const int PAD = 12, GUT = 10, W = 480, H = 272;
  const int CX0 = PAD, CX1 = W - PAD;             // content x: 12 .. 468 (456 wide)
  const int TOP_Y = 10, TOP_H = 26;               // top bar
  const int MAIN_Y = TOP_Y + TOP_H + 8;           // 44
  const int BAR_H = 54, BAR_Y = H - PAD - BAR_H;  // 206
  const int MAIN_H = BAR_Y - GUT - MAIN_Y;        // 152

  lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(parent, 0, 0);

  // the room: flat warm black
  lv_obj_set_style_bg_color(parent, color_surface_base, 0);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

  // ===== TOP BAR: the flag mark + loaded material + one state chip =====
  // The Hawaii flag is the identity mark (boot screen set the precedent);
  // 192x96 asset zoomed to 48x24 around a top-left pivot.
  {
    lv_obj_t *fl = lv_img_create(parent);
    lv_img_set_src(fl, &pono_flag);
    lv_img_set_pivot(fl, 0, 0);
    lv_img_set_zoom(fl, 64);
    lv_obj_set_pos(fl, CX0, TOP_Y + 1);
  }
  // Loaded material rides the top bar: a truthful instrument line, present in
  // both layouts so the hero stays free for the entry / the ring.
  //
  // Bounded to the gap between the flag mark and the state chip, and
  // ellipsized. This was TOP_MID at its natural width, so it grew outward from
  // the centre of the screen and ran under the chip, which is created after it
  // and therefore painted on top: "0.25 diamond" lost the tail of its last
  // glyph on every cockpit screen. Centre-aligning inside a fixed band keeps
  // the optical centring and makes a longer line (a 0.4 hardened brass, a
  // longer filament name) ellipsize honestly instead of vanishing under the
  // chip a word at a time.
  lv_obj_t *mat = tag(parent, m.material ? m.material : "", color_text_tertiary, 0, 0);
  {
    const int mat_x = CX0 + 54;                   // clear the 48px flag plus a gap
    const int mat_w = (CX1 - 160) - mat_x - 8;    // stop short of the chip's left edge
    // Height is pinned to a single line on purpose. LV_LABEL_LONG_DOT honours
    // the object's height as well as its width, so with an auto height it wraps
    // to a second line and spills into the content below instead of ellipsizing.
    // One line high means the dots land on the line that is actually there.
    lv_obj_set_size(mat, mat_w, lv_font_get_line_height(font_micro));
    lv_label_set_long_mode(mat, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(mat, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(mat, mat_x, TOP_Y + 7);
  }
  {
    lv_color_t sc = m.paused ? color_state_warning
                  : pr       ? color_accent_primary : color_accent_secondary;
    lv_obj_t *pill = card(parent, CX1 - 160, TOP_Y, 90, TOP_H - 2, color_surface_raised);  // narrowed to clear the top-layer E-STOP at the right margin
    hairline_c(pill, sc, opa_border_strong);
    lv_obj_t *dot = card(pill, 0, 0, 8, 8, sc, 4);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 11, 0);
    lv_obj_t *prl = tag(pill, m.paused ? "PAUSED" : (pr ? "PRINTING" : "READY"), sc, 0, 0);
    lv_obj_align(prl, LV_ALIGN_LEFT_MID, 25, 0);
    if (pr && !m.paused) bloom_pulse(dot, sc, 3, 11, 850);
    if (out) { out->state_pill = pill; out->state_dot = dot; }
  }
  card(parent, CX0, TOP_Y + TOP_H + 1, CX1 - CX0, 1, color_text_primary, 0, opa_border_subtle);

  // ===== MAIN: left hero + right column (temps + actions) =====
  const int HERO_W = 176, HERO_X = CX0;
  const int RCOL_X = CX0 + HERO_W + GUT, RCOL_W = CX1 - RCOL_X;  // 198, 270

  lv_obj_t *hero = panel(parent, HERO_X, MAIN_Y, HERO_W, MAIN_H);

  lv_obj_t *arc = nullptr;
  lv_obj_t *pl = nullptr, *lyl = nullptr, *eta_l = nullptr, *stale_l = nullptr;
  lv_obj_t *gl_def = nullptr, *gl_pos = nullptr, *gl_cnt = nullptr;
  if (pr) {
    // ---- printing hero: the progress ring ----
    const int ringD = 104, ringX = HERO_X + (HERO_W - ringD) / 2, ringY = MAIN_Y + 14;
    arc = lv_arc_create(parent);
    lv_obj_set_size(arc, ringD, ringD);
    lv_obj_set_pos(arc, ringX, ringY);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, m.progress_pct);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, color_surface_elevated, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, color_accent_primary, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);

    char pctbuf[8];
    snprintf(pctbuf, sizeof pctbuf, "%d%%", m.progress_pct);
    pl = lbl(parent, pctbuf, font_num_large, color_accent_secondary, 0, 0);
    lv_obj_align_to(pl, arc, LV_ALIGN_CENTER, 0, 0);

    char lybuf[28];
    snprintf(lybuf, sizeof lybuf, "layer %d / %d", m.layer, m.layer_total);
    lyl = lbl(parent, lybuf, font_micro, color_text_secondary, 0, 0);
    lv_obj_align_to(lyl, arc, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    eta_l = lbl(parent, m.eta ? m.eta : "", font_caption, color_accent_primary, 0, 0);
    lv_obj_align_to(eta_l, lyl, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    // The readout-stale flag (instrument-glass law: stale flags itself).
    // Hidden until the app's status watchdog sees the notify stream go
    // quiet mid-print; it sits on the instrument it indicts, in the free
    // band above the ring. Warn, not alarm: the machine may be fine, the
    // READOUT is what stopped moving.
    stale_l = tag(parent, "READOUT STALE", color_state_warning, 0, 0);
    lv_obj_align_to(stale_l, hero, LV_ALIGN_TOP_MID, 0, 3);
    lv_obj_add_flag(stale_l, LV_OBJ_FLAG_HIDDEN);
  } else {
    // ---- idle hero: the dictionary entry ----
    // The word keeps the log while the machine is idle. Day-seeded gloss,
    // tap anywhere on the entry for the next; the counter is the instrument.
    lv_obj_add_flag(hero, LV_OBJ_FLAG_CLICKABLE);
    int ix = gloss_today_index();

    lv_obj_t *word = lbl(hero, "pono", font_serif_display, color_text_primary, 0, 0);
    lv_obj_align(word, LV_ALIGN_TOP_MID, 0, 4);

    gl_def = lbl(hero, gloss_text(ix), font_serif_italic, color_text_secondary, 0, 0);
    lv_obj_set_width(gl_def, HERO_W - 24);
    lv_label_set_long_mode(gl_def, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(gl_def, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(word, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_align(gl_def, LV_ALIGN_TOP_MID, 0, 60);

    gl_pos = tag(hero, gloss_pos(ix), color_text_tertiary, 0, 0);
    lv_obj_align(gl_pos, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    char cb[12]; snprintf(cb, sizeof cb, "%02d / 43", ix + 1);
    gl_cnt = lbl(hero, cb, font_micro, color_accent_secondary, 0, 0);
    lv_obj_align(gl_cnt, LV_ALIGN_BOTTOM_RIGHT, -12, -8);
  }

  // ---- right column: two temp readouts ----
  const int tH = 42;
  lv_obj_t *nzc = nullptr, *nz_num = nullptr, *nz_set = nullptr;
  lv_obj_t *bdc = nullptr, *bd_num = nullptr, *bd_set = nullptr;
  temp_card(parent, RCOL_X, MAIN_Y, RCOL_W, tH, "NOZZLE",
            m.nozzle, m.nozzle_set, &nzc, &nz_num, &nz_set);
  temp_card(parent, RCOL_X, MAIN_Y + tH + 8, RCOL_W, tH, "BED",
            m.bed, m.bed_set, &bdc, &bd_num, &bd_set);

  // ---- action row: printing/paused -> [Pause|Resume] + Cancel + Tune; idle -> Print + Tune ----
  const int ay = MAIN_Y + 2 * (tH + 8);          // 144
  const int aH = MAIN_Y + MAIN_H - ay;           // 52
  lv_obj_t *prim = nullptr, *cxl = nullptr;
  int tnx, tnw;
  if (pr) {
    const int pw = 104, cw = 66, g = 6;
    // primary: the lamp marks the action at hand (Resume when paused, Pause otherwise)
    prim = lamp_btn(parent, RCOL_X, ay, pw, aH,
                    m.paused ? (LV_SYMBOL_PLAY "  Resume") : (LV_SYMBOL_PAUSE "  Pause"), ms);
    // cancel/abort: alarm outline (destructive; the app gates it behind confirm)
    cxl = card(parent, RCOL_X + pw + g, ay, cw, aH, color_surface_raised);
    hairline_c(cxl, color_state_error, opa_border_strong);
    lv_obj_center(lbl(cxl, LV_SYMBOL_STOP, ms, color_state_error, 0, 0));
    tnx = RCOL_X + pw + g + cw + g;   // 182
    tnw = CX1 - tnx;                  // 88, to the content right edge
  } else {
    const int primW = 168;
    prim = lamp_btn(parent, RCOL_X, ay, primW, aH, LV_SYMBOL_PLAY "  Print", ms);
    tnx = RCOL_X + primW + GUT - 2;
    tnw = RCOL_W - primW - GUT + 2;
  }

  lv_obj_t *tn = panel(parent, tnx, ay, tnw, aH, opa_border_medium);
  lv_obj_t *tni = lbl(tn, LV_SYMBOL_SETTINGS, ms, color_text_secondary, 0, 0);
  lv_obj_align(tni, LV_ALIGN_TOP_MID, 0, 8);
  lv_obj_t *tnl = tag(tn, "TUNE", color_text_tertiary, 0, 0);
  lv_obj_align(tnl, LV_ALIGN_BOTTOM_MID, 0, -7);

  // ===== BOTTOM NAV BAR: five equal tiles =====
  const char *bi[5] = {LV_SYMBOL_GPS, LV_SYMBOL_DOWNLOAD, LV_SYMBOL_DIRECTORY,
                       LV_SYMBOL_LOOP, LV_SYMBOL_LIST};
  const char *bn[5] = {"MOVE", "FILAMENT", "FILES", "FANS", "MORE"};
  const int tw = 84, tg = 8;
  const int bx0 = CX0 + ((CX1 - CX0) - (5 * tw + 4 * tg)) / 2;  // centered (== 14)
  for (int i = 0; i < 5; i++) {
    int x = bx0 + i * (tw + tg);
    lv_obj_t *t = nav_tile(parent, x, BAR_Y, tw, BAR_H, bi[i], bn[i], ms);
    if (out) { if (i < 4) out->qa[i] = t; else out->tile_more = t; }
  }

  if (out) {
    out->arc = arc; out->pct = pl; out->layer = lyl; out->stale = stale_l;
    out->job = nullptr; out->material = mat; out->eta = eta_l;
    out->nozzle = nz_num; out->bed = bd_num;
    out->nozzle_set = nz_set; out->bed_set = bd_set;
    out->tile_nozzle = nzc; out->tile_bed = bdc;
    out->tile_tune = tn; out->btn_pausestop = prim; out->btn_cancel = cxl;
    // The idle-only quartet is assigned HERE, not in the idle branch, so a
    // printing build nulls it. The caller's HomeHandles persists across
    // rebuild_home(); leaving these untouched on the idle->printing flip kept
    // a pointer into the hero lv_obj_clean() had just freed, and
    // attach_home_taps() then wired a tap callback onto freed memory
    // (use-after-free, instant segfault on the first print of the Kukui
    // cockpit). Every handle gets exactly one assignment site: this block.
    out->hero = pr ? nullptr : hero;
    out->def = gl_def; out->defpos = gl_pos; out->defn = gl_cnt;
  }
  return arc ? arc : hero;
}

// Persistent full-kill E-STOP. The one alarm lamp, always lit top-right above
// every screen. Built on lv_layer_top() (NOT home_scr) so it survives
// rebuild_home()'s lv_obj_clean and rides over every sub-screen. Solid alarm
// fill, dark label, machined corner - the same treatment as the Shutdown card.
// The app wires the tap to a confirm -> printer.emergency_stop (full halt).
lv_obj_t *build_estop(lv_obj_t *parent) {
  const int W = 64, H = 26, X = 480 - 12 - W, Y = 10;  // top-right, on the 12px outer margin
  lv_obj_t *b = card(parent, X, Y, W, H, color_state_error);
  lv_obj_t *l = lbl(b, "E-STOP", font_caption, color_surface_base, 0, 0);
  lv_obj_center(l);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(b, 8);  // generous hit target on the resistive panel
  return b;
}

// ---- Tune screen -----------------------------------------------------------
// Flexibility lives one tap in: the two tiers up top, granular calibrations in
// the middle, and a live speed slider at the base (the smooth 60fps moment -
// dragging repaints only the slider, so it stays buttery on this SoC).
void build_tune(lv_obj_t *parent, TuneHandles *h) {
  const lv_font_t *ms = &lv_font_montserrat_14;
  lv_obj_t *back = screen_header(parent, "TUNE");
  if (h) h->back = back;

  // ---- two tiers ----
  lv_obj_t *st = panel(parent, 12, 54, 224, 50, opa_border_medium);
  lv_obj_t *stt = lbl(st, "Standard", font_body, color_text_primary, 0, 0);
  lv_obj_align(stt, LV_ALIGN_TOP_LEFT, 14, 8);
  lv_obj_t *sts = tag(st, "MACHINE AUTO-CALS", color_text_tertiary, 0, 0);
  lv_obj_align(sts, LV_ALIGN_BOTTOM_LEFT, 14, -8);
  if (h) h->standard = st;

  // Make Pono: the full-calibration tier, lamp-marked (the action this screen
  // exists for). The name is the word in its first sense - correct, accurate,
  // in perfect order; the subtitle stays plain so the operator knows what it is.
  lv_obj_t *om = card(parent, 244, 54, 224, 50, color_surface_raised);
  hairline_c(om, color_accent_primary, LV_OPA_COVER);
  lv_obj_t *omt = lbl(om, "Make Pono", font_body, color_accent_primary, 0, 0);
  lv_obj_align(omt, LV_ALIGN_TOP_LEFT, 14, 8);
  lv_obj_t *oms = tag(om, "FULL CALIBRATION", color_text_tertiary, 0, 0);
  lv_obj_align(oms, LV_ALIGN_BOTTOM_LEFT, 14, -8);
  if (h) h->omega = om;

  // ---- individual calibrations ----
  tag(parent, "INDIVIDUAL", color_text_tertiary, 12, 112);
  const char *cals[5] = {"Bed Mesh", "Pressure Adv", "Flow", "Input Shaper", "Z-Offset"};
  for (int i = 0; i < 5; i++) {
    int col = i % 3, row = i / 3;
    int x = 12 + col * 154;
    int y = 128 + row * 44;
    lv_obj_t *t = panel(parent, x, y, 146, 38);
    lv_obj_t *nl = lbl(t, cals[i], font_caption, color_text_primary, 0, 0);
    lv_obj_align(nl, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *ch = lbl(t, LV_SYMBOL_RIGHT, ms, color_text_tertiary, 0, 0);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -8, 0);
    if (h) h->cals[i] = t;
  }

  // The porch lamp makes its round of the options: a 60fps amber orbit, ambient
  // only - the live state lives in the Make Pono narration, never in this light.
  lv_obj_t *orb = tune_orbit_create(parent);
  if (h) h->orbit = orb;

  // ---- live speed slider ----
  tag(parent, "SPEED", color_text_tertiary, 12, 224);
  lv_obj_t *spv = lbl(parent, "100%", font_num_small, color_accent_secondary, 0, 0);
  lv_obj_align(spv, LV_ALIGN_TOP_RIGHT, -14, 222);
  if (h) h->speed_val = spv;
  lv_obj_t *sl = lv_slider_create(parent);
  lv_obj_set_pos(sl, 12, 244);
  lv_obj_set_size(sl, 456, 10);
  lv_obj_set_ext_click_area(sl, 16);  // a 10px track is too thin to grab on a resistive panel
  lv_slider_set_range(sl, 50, 200);
  lv_slider_set_value(sl, 100, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl, color_surface_elevated, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(sl, radius_sm, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, color_accent_primary, LV_PART_INDICATOR);
  lv_obj_set_style_radius(sl, radius_sm, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, color_text_primary, LV_PART_KNOB);
  lv_obj_set_style_radius(sl, radius_sm, LV_PART_KNOB);
  if (h) h->speed = sl;
}

// ---- Expert Tune screen ----------------------------------------------------
// One full-width settings row; returns the card so the caller drops the editor.
static lv_obj_t *setting_row(lv_obj_t *list, const char *name) {
  lv_obj_t *r = lv_obj_create(list);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, lv_pct(100), 36);
  lv_obj_set_style_bg_color(r, color_surface_raised, 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(r, radius_sm, 0);
  hairline(r);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *n = lbl(r, name, font_caption, color_text_primary, 0, 0);
  lv_obj_align(n, LV_ALIGN_LEFT_MID, 12, 0);
  return r;
}

// Tappable value field on the right of a row (tap -> keypad in the app).
// Returns the pill so the app can wire the tap and update the value text
// (the value label is the pill's first child).
static lv_obj_t *value_pill(lv_obj_t *row, const char *val) {
  lv_obj_t *p = lv_obj_create(row);
  lv_obj_remove_style_all(p);
  lv_obj_set_size(p, 80, 26);
  lv_obj_align(p, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_obj_set_style_bg_color(p, color_surface_base, 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(p, radius_sm, 0);
  hairline_c(p, color_accent_secondary, opa_border_medium);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *v = lbl(p, val, font_caption, color_accent_secondary, 0, 0);
  lv_obj_center(v);
  return p;
}

// Set the value text on a pill returned by value_pill (label is child 0).
static void pill_text(lv_obj_t *pill, const char *txt, lv_opa_t opa) {
  if (!pill) return;
  lv_obj_t *v = lv_obj_get_child(pill, 0);
  if (v) { lv_label_set_text(v, txt); lv_obj_set_style_text_opa(v, opa, 0); lv_obj_center(v); }
}
void pill_set(lv_obj_t *pill, const char *txt)     { pill_text(pill, txt, LV_OPA_COVER); }
void pill_pending(lv_obj_t *pill, const char *txt) { pill_text(pill, txt, LV_OPA_50); }

// A row of three tappable preset chips (quick values beside the keypad).
static void chip_row(lv_obj_t *list, const char *a, const char *b, const char *c, lv_obj_t *out[3]) {
  lv_obj_t *r = lv_obj_create(list);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, lv_pct(100), 30);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  const char *labs[3] = {a, b, c};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *ch = lv_obj_create(r);
    lv_obj_remove_style_all(ch);
    lv_obj_set_size(ch, 140, 30);
    lv_obj_set_style_bg_color(ch, color_surface_raised, 0);
    lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ch, radius_sm, 0);
    hairline(ch);
    lv_obj_clear_flag(ch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lbl(ch, labs[i], font_micro, color_text_secondary, 0, 0);
    lv_obj_center(l);
    out[i] = ch;
  }
}

void build_settings(lv_obj_t *parent, SettingsHandles *h) {
  lv_obj_t *back = screen_header(parent, "EXPERT TUNE");
  if (h) h->back = back;

  lv_obj_t *list = lv_obj_create(parent);
  lv_obj_remove_style_all(list);
  lv_obj_set_pos(list, 12, 54);
  lv_obj_set_size(list, 456, 210);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 7, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(list, color_text_secondary, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, LV_OPA_40, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(list, radius_sm, LV_PART_SCROLLBAR);

  // A [-] value [+] row: the value stays keypad-tappable, the buttons step it.
  auto stepper = [&](const char *name, const char *val, lv_obj_t **minus, lv_obj_t **pill, lv_obj_t **plus) {
    lv_obj_t *r = setting_row(list, name);
    lv_obj_t *pls = card(r, 0, 0, 34, 28, color_surface_elevated);
    hairline(pls);
    lv_obj_align(pls, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_center(lbl(pls, LV_SYMBOL_PLUS, &lv_font_montserrat_14, color_text_primary, 0, 0));
    lv_obj_t *pv = value_pill(r, val);
    lv_obj_set_width(pv, 68);
    lv_obj_align(pv, LV_ALIGN_RIGHT_MID, -48, 0);
    lv_obj_t *mns = card(r, 0, 0, 34, 28, color_surface_elevated);
    hairline(mns);
    lv_obj_align(mns, LV_ALIGN_RIGHT_MID, -122, 0);
    lv_obj_center(lbl(mns, LV_SYMBOL_MINUS, &lv_font_montserrat_14, color_text_primary, 0, 0));
    *minus = mns; *pill = pv; *plus = pls;
  };
  SettingsHandles dummy;
  SettingsHandles *o = h ? h : &dummy;

  // Speed factor (M220): 5% steps, quick presets, and the melt rate it asks
  // for. A speed-up that outruns the hotend shows here before it prints.
  stepper("Speed factor", "100%", &o->speed_minus, &o->speed, &o->speed_plus);
  { lv_obj_t *c[3]; chip_row(list, "50%", "100%", "150%", c); if (h) { h->speed_p[0] = c[0]; h->speed_p[1] = c[1]; h->speed_p[2] = c[2]; } }
  {
    lv_obj_t *r = setting_row(list, "Melt rate");
    lv_obj_t *m = lbl(r, "-- mm3/s", font_caption, color_accent_secondary, 0, 0);
    lv_obj_align(m, LV_ALIGN_RIGHT_MID, -16, 0);
    if (h) h->melt = m;
  }

  // Flow factor (M221) + quick presets
  { lv_obj_t *p = value_pill(setting_row(list, "Flow factor"), "100%"); if (h) h->flow = p; }
  { lv_obj_t *c[3]; chip_row(list, "95%", "100%", "105%", c); if (h) { h->flow_p[0] = c[0]; h->flow_p[1] = c[1]; h->flow_p[2] = c[2]; } }

  // Z-offset (live babystep via SET_GCODE_OFFSET)
  stepper("Z-offset", "0.000", &o->zoff_minus, &o->zoff, &o->zoff_plus);

  // Pressure advance (SET_PRESSURE_ADVANCE)
  { lv_obj_t *p = value_pill(setting_row(list, "Pressure advance"), "0.040"); if (h) h->pa = p; }

  // Part fan (M106) + quick presets
  { lv_obj_t *p = value_pill(setting_row(list, "Part fan"), "0%"); if (h) h->fan = p; }
  { lv_obj_t *c[3]; chip_row(list, "Off", "50%", "Full", c); if (h) { h->fan_p[0] = c[0]; h->fan_p[1] = c[1]; h->fan_p[2] = c[2]; } }
}

// ============================================================================
// Native sub-screens (replace the legacy guppyscreen panels). Each is a full
// 480x272 screen built on the same grid + tokens as the cockpit, with a back
// chip top-left. The app shows/hides them over the cockpit and wires actions.
// ============================================================================

// Shared chrome: the room + top bar (back chip + caps title + divider).
// Returns the back chip so the caller wires "return to cockpit".
static lv_obj_t *screen_header(lv_obj_t *parent, const char *title) {
  const lv_font_t *ms = &lv_font_montserrat_14;
  lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(parent, 0, 0);
  lv_obj_set_style_bg_color(parent, color_surface_base, 0);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
  lv_obj_t *back = panel(parent, 12, 10, 42, 28, opa_border_medium);
  lv_obj_t *bi = lbl(back, LV_SYMBOL_LEFT, ms, color_text_primary, 0, 0);
  lv_obj_center(bi);
  lv_obj_t *t = lbl(parent, title, font_h2, color_text_primary, 64, 13);
  lv_obj_set_style_text_letter_space(t, track_caps, 0);
  card(parent, 12, 44, 456, 1, color_text_primary, 0, opa_border_subtle);
  return back;
}

// Generic tappable card-button with a centered label.
static lv_obj_t *tap_btn(lv_obj_t *p, int x, int y, int w, int h,
                         const char *txt, const lv_font_t *f, lv_color_t tc) {
  lv_obj_t *b = panel(p, x, y, w, h, opa_border_medium);
  lv_obj_t *l = lbl(b, txt, f, tc, 0, 0);
  lv_obj_center(l);
  return b;
}

void build_move(lv_obj_t *parent, MoveHandles *h) {
  const lv_font_t *ms = &lv_font_montserrat_14;
  lv_obj_t *back = screen_header(parent, "MOVE");
  if (h) h->back = back;

  const int bs = 50;
  // ---- XY jog cross (left) ----
  const int cx = 78, cy = 112;
  lv_obj_t *yp = tap_btn(parent, cx, cy - bs - 8, bs, bs, LV_SYMBOL_UP, ms, color_text_primary);
  lv_obj_t *ym = tap_btn(parent, cx, cy + bs + 8, bs, bs, LV_SYMBOL_DOWN, ms, color_text_primary);
  lv_obj_t *xm = tap_btn(parent, cx - bs - 8, cy, bs, bs, LV_SYMBOL_LEFT, ms, color_text_primary);
  lv_obj_t *xp = tap_btn(parent, cx + bs + 8, cy, bs, bs, LV_SYMBOL_RIGHT, ms, color_text_primary);
  lv_obj_t *hxy = tap_btn(parent, cx, cy, bs, bs, LV_SYMBOL_HOME, ms, color_accent_primary);
  if (h) { h->xplus = xp; h->xminus = xm; h->yplus = yp; h->yminus = ym; h->home_xy = hxy; }

  // ---- Z jog ----
  const int zx = 212;
  lv_obj_t *zp = tap_btn(parent, zx, cy - bs - 8, bs, bs, LV_SYMBOL_UP, ms, color_text_primary);
  lv_obj_t *zm = tap_btn(parent, zx, cy + bs + 8, bs, bs, LV_SYMBOL_DOWN, ms, color_text_primary);
  lbl(parent, "Z", font_num_small, color_text_secondary, zx + 20, cy + 16);
  if (h) { h->zplus = zp; h->zminus = zm; }

  // ---- step selector ----
  tag(parent, "STEP (MM)", color_text_tertiary, 282, 40);
  const char *steps[4] = {"0.1", "1", "10", "100"};
  for (int i = 0; i < 4; i++) {
    int sw = 44, sx = 280 + i * (sw + 4);
    bool on = (i == 1);
    lv_obj_t *sb = card(parent, sx, 54, sw, 32, on ? color_accent_primary : color_surface_raised);
    if (!on) hairline(sb);
    lv_obj_t *sl = lbl(sb, steps[i], font_caption, on ? color_surface_base : color_text_secondary, 0, 0);
    lv_obj_center(sl);
    if (h) h->step[i] = sb;
  }

  // ---- home all + motors off ----
  lv_obj_t *ha = lamp_btn(parent, 280, 96, 188, 46, LV_SYMBOL_HOME "  Home All", ms);
  lv_obj_t *mo = tap_btn(parent, 280, 150, 188, 46, "Motors Off", font_body, color_text_secondary);
  if (h) { h->home_all = ha; h->motors_off = mo; }

  // ---- position readout ----
  lv_obj_t *pc = card(parent, 280, 204, 188, 44, color_surface_base);
  hairline(pc);
  lv_obj_t *pl = lbl(pc, "X --  Y --  Z --", font_num_small, color_accent_secondary, 0, 0);
  lv_obj_center(pl);
  if (h) h->pos = pl;
}

void build_filament(lv_obj_t *parent, FilamentHandles *h) {
  const lv_font_t *ms = &lv_font_montserrat_14;
  lv_obj_t *back = screen_header(parent, "FILAMENT");
  if (h) h->back = back;

  // Live nozzle temp rides the header line (right side) - frees a full row so
  // material select, load length, and the action pairs all keep 44pt targets.
  lv_obj_t *tc = lv_obj_create(parent);
  lv_obj_remove_style_all(tc);
  lv_obj_set_pos(tc, 250, 6);
  lv_obj_set_size(tc, 218, 34);
  lv_obj_clear_flag(tc, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *tn = tag(tc, "NOZZLE", color_text_tertiary, 0, 0);
  lv_obj_align(tn, LV_ALIGN_LEFT_MID, 60, 0);
  lv_obj_t *tv = lbl(tc, "-- / --", font_num_small, color_accent_secondary, 0, 0);
  lv_obj_align(tv, LV_ALIGN_RIGHT_MID, -14, 0);
  lv_obj_add_flag(tv, LV_OBJ_FLAG_CLICKABLE);   // tap the nozzle temp to type an exact target
  lv_obj_set_ext_click_area(tv, 18);            // fat-finger touch target
  if (h) h->temp = tv;

  // Material select (doubles as preheat): the picked tier is the temp Load /
  // Unload run at. seg_highlight marks the active material; Off cools down.
  const char *pn[3] = {"PLA", "PETG", "PA-CF"};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *pb = tap_btn(parent, 12 + i * 116, 52, 108, 44, pn[i], font_caption, color_text_secondary);
    if (h) h->preset[i] = pb;
  }
  lv_obj_t *cd = tap_btn(parent, 360, 52, 108, 44, "Off", font_caption, color_text_secondary);
  if (h) h->cooldown = cd;

  // Load length: slider + live mm readout (used by Load; Extrude/Retract keep
  // their fixed 25mm purge).
  tag(parent, "LOAD LENGTH", color_text_tertiary, 12, 106);
  lv_obj_t *lval = lbl(parent, "200 mm", font_num_small, color_accent_secondary, 0, 0);
  lv_obj_align(lval, LV_ALIGN_TOP_RIGHT, -14, 104);
  lv_obj_t *sl = lv_slider_create(parent);
  lv_obj_set_pos(sl, 12, 128);
  lv_obj_set_size(sl, 456, 10);
  lv_slider_set_range(sl, 50, 300);
  lv_slider_set_value(sl, 200, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl, color_surface_elevated, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(sl, radius_sm, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, color_accent_primary, LV_PART_INDICATOR);
  lv_obj_set_style_radius(sl, radius_sm, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, color_text_primary, LV_PART_KNOB);
  lv_obj_set_style_radius(sl, radius_sm, LV_PART_KNOB);
  lv_obj_set_ext_click_area(sl, 16);  // thin track, fat finger
  if (h) { h->len_slider = sl; h->len_val = lval; }

  // load / unload (primary)
  lv_obj_t *ld = lamp_btn(parent, 12, 152, 224, 52, LV_SYMBOL_DOWN "  Load", ms);
  lv_obj_t *ul = tap_btn(parent, 244, 152, 224, 52, LV_SYMBOL_UP "  Unload", ms, color_text_primary);
  if (h) { h->load = ld; h->unload = ul; }

  // extrude / retract
  lv_obj_t *ex = tap_btn(parent, 12, 212, 224, 44, "Extrude 25", font_body, color_text_primary);
  lv_obj_t *rt = tap_btn(parent, 244, 212, 224, 44, "Retract 25", font_body, color_text_primary);
  if (h) { h->extrude = ex; h->retract = rt; }
}

// one temperature column (nozzle or bed): current, target, 3 presets, Off.
// out = [cur, tgt, b0, b1, b2, off].
static void temp_section(lv_obj_t *parent, int x, int w, const char *name,
                         const char *p0, const char *p1, const char *p2,
                         lv_obj_t *out[8]) {
  lv_obj_t *c = panel(parent, x, 52, w, 196);
  lv_obj_t *nm = tag(c, name, color_text_secondary, 0, 0);
  lv_obj_align(nm, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_t *cv = lbl(c, "--", font_num_large, color_accent_secondary, 0, 0);
  lv_obj_align(cv, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_add_flag(cv, LV_OBJ_FLAG_CLICKABLE);   // tap the number to type an exact target
  lv_obj_set_ext_click_area(cv, 18);            // fat-finger touch target
  // manual -/+ steppers flanking the live target value (the "set temp" control)
  lv_obj_t *mn = card(c, 10, 68, 46, 34, color_surface_elevated);
  hairline(mn);
  lv_obj_t *mnl = lbl(mn, LV_SYMBOL_MINUS, &lv_font_montserrat_14, color_text_primary, 0, 0);
  lv_obj_center(mnl);
  lv_obj_t *ps_btn = card(c, w - 10 - 46, 68, 46, 34, color_surface_elevated);
  hairline(ps_btn);
  lv_obj_t *psl = lbl(ps_btn, LV_SYMBOL_PLUS, &lv_font_montserrat_14, color_text_primary, 0, 0);
  lv_obj_center(psl);
  lv_obj_t *tv = lbl(c, "off", font_caption, color_accent_primary, 0, 0);
  lv_obj_align(tv, LV_ALIGN_TOP_MID, 0, 76);
  out[0] = cv; out[1] = tv; out[6] = mn; out[7] = ps_btn;
  const char *ps[3] = {p0, p1, p2};
  int pw = (w - 24 - 2 * 6) / 3;
  for (int i = 0; i < 3; i++) {
    lv_obj_t *pb = card(c, 12 + i * (pw + 6), 110, pw, 34, color_surface_elevated);
    hairline(pb);
    lv_obj_t *lab = lbl(pb, ps[i], font_micro, color_text_secondary, 0, 0);
    lv_obj_center(lab);
    out[2 + i] = pb;
  }
  lv_obj_t *ob = card(c, 12, 152, w - 24, 32, color_surface_base);
  hairline_c(ob, color_state_error, opa_border_medium);
  lv_obj_t *ol = lbl(ob, "Off", font_caption, color_state_error, 0, 0);
  lv_obj_center(ol);
  out[5] = ob;
}

void build_temps(lv_obj_t *parent, TempsHandles *h) {
  lv_obj_t *back = screen_header(parent, "TEMPERATURE");
  if (h) h->back = back;
  lv_obj_t *nz[8], *bd[8];
  temp_section(parent, 12, 224, "NOZZLE", "PLA 220", "PETG 240", "PA 260", nz);
  // PA 45, not 75. The Sunlu Easy-PA profile we actually ship runs
  // hot_plate_temp 45 (Easy-PA spec is 30-50C); 75 is generic raw-PA territory
  // and would put the bed 30C over the validated value. The nozzle row's PA 260
  // already matches its profile, which is why only this one moved.
  temp_section(parent, 244, 224, "BED", "PLA 60", "PETG 80", "PA 45", bd);
  if (h) {
    h->nz_cur = nz[0]; h->nz_tgt = nz[1];
    h->nz_preset[0] = nz[2]; h->nz_preset[1] = nz[3]; h->nz_preset[2] = nz[4]; h->nz_off = nz[5];
    h->nz_minus = nz[6]; h->nz_plus = nz[7];
    h->bd_cur = bd[0]; h->bd_tgt = bd[1];
    h->bd_preset[0] = bd[2]; h->bd_preset[1] = bd[3]; h->bd_preset[2] = bd[4]; h->bd_off = bd[5];
    h->bd_minus = bd[6]; h->bd_plus = bd[7];
  }
}

// ---- Bed mesh heatmap -------------------------------------------------------
// Color as function: probed Z deviation, cold -> hot. The ramp runs cold steel
// (low) through phosphor (level) to lamp and alarm (high), so "high spot" reads
// in the same language as every other warning on the panel. Universally-read
// cold/hot semantics, Kukui vocabulary.
static lv_color_t heat_color(float t) {
  if (t < 0.f) t = 0.f;
  if (t > 1.f) t = 1.f;
  // 4 stops: #3a4a63 cold steel -> #8fd6ad phosphor -> #e2a13c lamp -> #e06a5a alarm
  const uint8_t st[4][3] = {
    {0x3a, 0x4a, 0x63}, {0x8f, 0xd6, 0xad}, {0xe2, 0xa1, 0x3c}, {0xe0, 0x6a, 0x5a},
  };
  float seg = t * 3.0f;
  int i = (int)seg; if (i > 2) i = 2;
  float u = seg - (float)i;
  uint8_t r = (uint8_t)(st[i][0] + (st[i + 1][0] - st[i][0]) * u);
  uint8_t g = (uint8_t)(st[i][1] + (st[i + 1][1] - st[i][1]) * u);
  uint8_t b = (uint8_t)(st[i][2] + (st[i + 1][2] - st[i][2]) * u);
  return lv_color_make(r, g, b);
}

void mesh_render(lv_obj_t *grid, const float *z, int rows, int cols, float zmin, float zmax) {
  if (!grid || !z || rows < 1 || cols < 1) return;
  lv_obj_clean(grid);
  int gw = lv_obj_get_width(grid);  if (gw <= 0) gw = 192;
  int gh = lv_obj_get_height(grid); if (gh <= 0) gh = 192;
  float range = zmax - zmin; if (range < 1e-6f) range = 1e-6f;
  int cw = gw / cols, ch = gh / rows;
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++) {
      float t = (z[r * cols + c] - zmin) / range;
      lv_obj_t *cell = lv_obj_create(grid);
      lv_obj_remove_style_all(cell);
      lv_obj_set_size(cell, cw + 1, ch + 1);
      lv_obj_set_pos(cell, c * cw, (rows - 1 - r) * ch);  // front row at the bottom
      lv_obj_set_style_bg_color(cell, heat_color(t), 0);
      lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
      lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    }
}

void build_mesh(lv_obj_t *parent, MeshHandles *h) {
  lv_obj_t *back = screen_header(parent, "BED MESH");
  if (h) h->back = back;

  lv_obj_t *grid = lv_obj_create(parent);
  lv_obj_remove_style_all(grid);
  lv_obj_set_pos(grid, 14, 60);
  lv_obj_set_size(grid, 192, 192);
  lv_obj_set_style_bg_color(grid, color_surface_base, 0);
  lv_obj_set_style_bg_opa(grid, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(grid, radius_sm, 0);
  lv_obj_set_style_clip_corner(grid, true, 0);
  hairline(grid, opa_border_medium);
  lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  if (h) h->grid = grid;

  lv_obj_t *pf = lbl(parent, "Profile: default", font_caption, color_text_primary, 220, 66);
  if (h) h->profile = pf;
  lv_obj_t *rg = lbl(parent, "Range: --", font_micro, color_text_secondary, 220, 92);
  if (h) h->range = rg;

  // legend: alarm (high) top -> cold steel (low) bottom, the heat_color ramp
  lv_obj_t *bar = lv_obj_create(parent);
  lv_obj_remove_style_all(bar);
  lv_obj_set_pos(bar, 220, 132);
  lv_obj_set_size(bar, 22, 104);
  lv_obj_set_style_bg_color(bar, color_state_error, 0);
  lv_obj_set_style_bg_grad_color(bar, lv_color_make(0x3a, 0x4a, 0x63), 0);
  lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bar, radius_sm, 0);
  tag(parent, "HIGH", color_text_tertiary, 250, 132);
  tag(parent, "LOW", color_text_tertiary, 250, 226);

  // demo surface so the sim and first boot show a heatmap before a real probe
  static const float demo[49] = {
    0.05f, 0.03f, 0.00f,-0.02f, 0.00f, 0.03f, 0.06f,
    0.03f, 0.01f,-0.02f,-0.04f,-0.02f, 0.01f, 0.04f,
    0.00f,-0.02f,-0.05f,-0.07f,-0.05f,-0.02f, 0.01f,
   -0.02f,-0.04f,-0.07f,-0.09f,-0.07f,-0.03f, 0.00f,
    0.00f,-0.02f,-0.05f,-0.07f,-0.04f,-0.01f, 0.02f,
    0.03f, 0.01f,-0.02f,-0.03f,-0.01f, 0.02f, 0.05f,
    0.06f, 0.04f, 0.01f, 0.00f, 0.02f, 0.05f, 0.08f };
  mesh_render(grid, demo, 7, 7, -0.09f, 0.08f);
}

void build_more(lv_obj_t *parent, MoreHandles *h) {
  lv_obj_t *back = screen_header(parent, "MORE");
  if (h) h->back = back;

  lv_obj_t *list = lv_obj_create(parent);
  lv_obj_remove_style_all(list);
  lv_obj_set_pos(list, 12, 56);
  lv_obj_set_size(list, 456, 206);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 8, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(list, color_text_secondary, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, LV_OPA_40, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);

  auto row = [&](const char *icon, const char *title, const char *sub) -> lv_obj_t * {
    lv_obj_t *r = lv_obj_create(list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), 58);
    lv_obj_set_style_bg_color(r, color_surface_raised, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, radius_sm, 0);
    hairline(r);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ic = lbl(r, icon, &lv_font_montserrat_14, color_text_secondary, 0, 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t *tl = lbl(r, title, font_body, color_text_primary, 0, 0);
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 48, -9);
    lv_obj_t *sl = tag(r, sub, color_text_tertiary, 0, 0);
    lv_obj_align(sl, LV_ALIGN_LEFT_MID, 48, 10);
    lv_obj_t *ch = lbl(r, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, color_text_tertiary, 0, 0);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -14, 0);
    return r;
  };

  lv_obj_t *w  = row(LV_SYMBOL_WIFI, "Wi-Fi & Network", "SCAN AND CONNECT");
  lv_obj_t *m  = row(LV_SYMBOL_IMAGE, "Bed Mesh", "LIVE PROBED SURFACE HEATMAP");
  lv_obj_t *e  = row(LV_SYMBOL_SETTINGS, "Expert Tune", "LIVE PRINT TUNING");
  lv_obj_t *li = row(LV_SYMBOL_CHARGE, "Lights", "CASE + HOTEND LEDS");
  lv_obj_t *sy = row(LV_SYMBOL_LIST, "System", "VERSION, UPDATE, NETWORK");
  lv_obj_t *pw = row(LV_SYMBOL_POWER, "Power", "RESTART, REBOOT, SHUTDOWN");
  if (h) { h->wifi = w; h->mesh = m; h->expert = e; h->led = li; h->system = sy; h->power = pw; }
}

void build_system(lv_obj_t *parent, SystemHandles *h) {
  lv_obj_t *back = screen_header(parent, "SYSTEM");
  if (h) h->back = back;
  lv_obj_t *list = lv_obj_create(parent);
  lv_obj_remove_style_all(list);
  lv_obj_set_pos(list, 12, 54);
  lv_obj_set_size(list, 456, 180);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 3, 0);
  lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  auto inforow = [&](const char *name) -> lv_obj_t * {
    lv_obj_t *r = lv_obj_create(list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), 27);
    lv_obj_set_style_bg_color(r, color_surface_raised, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, radius_sm, 0);
    hairline(r);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *n = tag(r, name, color_text_tertiary, 0, 0);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *v = lbl(r, "--", font_caption, color_text_primary, 0, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -12, 0);
    return v;
  };
  // FIRMWARE row, with a small image-integrity badge just right of the label.
  // Tells the operator at a glance whether the running image is an official
  // signed build or has been modified (we do not block unsigned flashes, the
  // owner-open FEL/USB paths stay open, so we surface the state instead).
  lv_obj_t *fr = lv_obj_create(list);
  lv_obj_remove_style_all(fr);
  lv_obj_set_size(fr, lv_pct(100), 27);
  lv_obj_set_style_bg_color(fr, color_surface_raised, 0);
  lv_obj_set_style_bg_opa(fr, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(fr, radius_sm, 0);
  hairline(fr);
  lv_obj_clear_flag(fr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *frn = tag(fr, "FIRMWARE", color_text_tertiary, 0, 0);
  lv_obj_align(frn, LV_ALIGN_LEFT_MID, 12, 0);
  lv_obj_t *intg = lbl(fr, "unknown", font_micro, color_text_tertiary, 0, 0);
  lv_obj_align(intg, LV_ALIGN_LEFT_MID, 92, 0);
  lv_obj_add_flag(intg, LV_OBJ_FLAG_CLICKABLE);  // tap re-reads the unofficial notice
  lv_obj_set_ext_click_area(intg, 10);           // micro label, fat finger
  lv_obj_t *fw = lbl(fr, "--", font_caption, color_text_primary, 0, 0);
  lv_obj_align(fw, LV_ALIGN_RIGHT_MID, -12, 0);

  // Update row: live status on the right; the Install chip appears between
  // when a newer build is published (the app reveals it).
  lv_obj_t *ur = lv_obj_create(list);
  lv_obj_remove_style_all(ur);
  lv_obj_set_size(ur, lv_pct(100), 27);
  lv_obj_set_style_bg_color(ur, color_surface_raised, 0);
  lv_obj_set_style_bg_opa(ur, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(ur, radius_sm, 0);
  hairline(ur);
  lv_obj_clear_flag(ur, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *un = tag(ur, "UPDATE", color_text_tertiary, 0, 0);
  lv_obj_align(un, LV_ALIGN_LEFT_MID, 12, 0);
  lv_obj_t *us = lbl(ur, "checking...", font_caption, color_text_secondary, 0, 0);
  lv_obj_align(us, LV_ALIGN_RIGHT_MID, -12, 0);
  lv_obj_t *ub = card(ur, 0, 0, 84, 22, color_accent_primary);
  lv_obj_align(ub, LV_ALIGN_RIGHT_MID, -12, 0);
  lv_obj_t *ubl = tag(ub, "INSTALL", color_surface_base, 0, 0);
  lv_obj_center(ubl);
  lv_obj_add_flag(ub, LV_OBJ_FLAG_HIDDEN);   // hidden until an update is available

  lv_obj_t *hn = inforow("HOSTNAME");
  lv_obj_t *ip = inforow("IP ADDRESS");
  lv_obj_t *up = inforow("UPTIME");
  lv_obj_t *mc = inforow("CPU TEMP");

  // The plate line: the motto, engraved at the foot of the system page.
  lv_obj_t *motto = lbl(parent, "ua mau ke ea o ka ‘āina i ka pono",
                        font_serif_italic, color_text_tertiary, 0, 0);
  lv_obj_align(motto, LV_ALIGN_BOTTOM_MID, 0, -6);

  if (h) {
    h->version = fw; h->host = hn; h->ip = ip; h->uptime = up; h->mcu = mc;
    h->update_status = us; h->btn_install = ub; h->integrity = intg;
  }
}

IntegrityState integrity_state_from_wire(const char *state) {
  if (state && strcmp(state, "signed") == 0)   return IntegrityState::OfficialSigned;
  if (state && strcmp(state, "modified") == 0) return IntegrityState::Unofficial;
  // Fail closed: an absent, unreadable, or unrecognized provenance state
  // renders unknown, never official.
  return IntegrityState::Unknown;
}

void system_set_integrity(SystemHandles *h, IntegrityState state) {
  if (!h || !h->integrity) return;
  switch (state) {
  case IntegrityState::OfficialSigned:
    lv_label_set_text(h->integrity, "official-signed");
    lv_obj_set_style_text_color(h->integrity, color_accent_secondary, 0);  // phosphor, quiet
    break;
  case IntegrityState::Unofficial:
    lv_label_set_text(h->integrity, "UNOFFICIAL");
    lv_obj_set_style_text_color(h->integrity, color_state_warning, 0);     // amber caution
    break;
  default:
    lv_label_set_text(h->integrity, "unknown");
    lv_obj_set_style_text_color(h->integrity, color_text_tertiary, 0);     // dim
    break;
  }
}

void build_power(lv_obj_t *parent, PowerHandles *h) {
  lv_obj_t *back = screen_header(parent, "POWER");
  if (h) h->back = back;
  lv_obj_t *rk = tap_btn(parent, 12, 64, 224, 86, "Restart Klipper", font_body, color_text_primary);
  lv_obj_t *rf = tap_btn(parent, 244, 64, 224, 86, "Restart Firmware", font_body, color_text_primary);
  // Reboot: deep-amber caution fill. Shutdown: solid alarm. Both confirm-gated.
  lv_obj_t *rb = card(parent, 12, 160, 224, 86, color_state_warning);
  lv_obj_center(lbl(rb, "Reboot", font_body, color_surface_base, 0, 0));
  lv_obj_t *sd = card(parent, 244, 160, 224, 86, color_state_error);
  lv_obj_center(lbl(sd, "Shutdown", font_body, color_surface_base, 0, 0));
  if (h) { h->restart_klipper = rk; h->restart_fw = rf; h->reboot = rb; h->shutdown = sd; }
}

void seg_highlight(lv_obj_t *const *btns, int n, int active) {
  for (int i = 0; i < n; i++) {
    if (!btns[i]) continue;
    bool on = (i == active);
    lv_obj_set_style_bg_color(btns[i], on ? color_accent_primary : color_surface_raised, 0);
    lv_obj_t *l = lv_obj_get_child(btns[i], 0);
    if (l) lv_obj_set_style_text_color(l, on ? color_surface_base : color_text_secondary, 0);
  }
}

void build_confirm(lv_obj_t *parent, ConfirmHandles *h) {
  // Scrim first -> lower z than the card; both live on lv_layer_top, hidden
  // until the app shows them before a destructive action.
  lv_obj_t *scrim = lv_obj_create(parent);
  lv_obj_remove_style_all(scrim);
  lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(scrim, color_surface_base, 0);
  lv_obj_set_style_bg_opa(scrim, LV_OPA_70, 0);
  lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *cd = card(parent, 70, 71, 340, 130, color_surface_raised);  // centered on 480x272
  hairline(cd, opa_border_medium);
  lv_obj_add_flag(cd, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *msg = lbl(cd, "Are you sure?", font_body, color_text_primary, 0, 0);
  lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(msg, 308);
  lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 22);

  lv_obj_t *cancel = tap_btn(cd, 16, 76, 150, 40, "Cancel", font_body, color_text_primary);
  lv_obj_t *confirm = card(cd, 174, 76, 150, 40, color_state_error);
  lv_obj_center(lbl(confirm, "Confirm", font_body, color_surface_base, 0, 0));

  if (h) { h->scrim = scrim; h->card = cd; h->msg = msg; h->cancel = cancel; h->confirm = confirm; }
}

void build_notice(lv_obj_t *parent, NoticeHandles *h) {
  // Mirrors build_confirm (scrim under card, both hidden until shown), but
  // sized for paragraph-length copy the confirm card cannot hold (the B9
  // unofficial-build notice) and with a single OK instead of Cancel/Confirm.
  lv_obj_t *scrim = lv_obj_create(parent);
  lv_obj_remove_style_all(scrim);
  lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(scrim, color_surface_base, 0);
  lv_obj_set_style_bg_opa(scrim, LV_OPA_70, 0);
  lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *cd = card(parent, 40, 26, 400, 220, color_surface_raised);  // centered on 480x272
  hairline(cd, opa_border_medium);
  lv_obj_add_flag(cd, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *msg = lbl(cd, "", font_caption, color_text_primary, 0, 0);
  lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(msg, 368);
  lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 18);

  lv_obj_t *ok = tap_btn(cd, 125, 164, 150, 40, "OK", font_body, color_text_primary);

  if (h) { h->scrim = scrim; h->card = cd; h->msg = msg; h->ok = ok; }
}

void build_lights(lv_obj_t *parent, LightsHandles *h) {
  lv_obj_t *back = screen_header(parent, "LIGHTS");
  if (h) h->back = back;
  auto section = [&](int y, const char *name, lv_obj_t **off, lv_obj_t **mid, lv_obj_t **full) {
    lv_obj_t *c = panel(parent, 12, y, 456, 88);
    lv_obj_t *nm = lbl(c, name, font_body, color_text_primary, 0, 0);
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 16, 12);
    const int bw = 134, bh = 38, gap = 8, x0 = 16, by = 40;
    *off = tap_btn(c, x0, by, bw, bh, "Off", font_caption, color_text_secondary);
    *mid = tap_btn(c, x0 + bw + gap, by, bw, bh, "50%", font_caption, color_text_primary);
    *full = tap_btn(c, x0 + 2 * (bw + gap), by, bw, bh, "Full", font_caption, color_text_primary);
  };
  lv_obj_t *co, *cm, *cf, *ho, *hm, *hf;
  section(56, "Case", &co, &cm, &cf);
  section(150, "Hotend", &ho, &hm, &hf);
  // Seed to the real boot state, not to Full for both. machine.cfg sets
  // [led case] initial_WHITE: 1 and gives [led hotend] no initial, so case
  // boots full and hotend boots off. Nothing reads the LEDs back, so this seed
  // IS what the screen claims until someone taps. The app re-applies the
  // tracked level (main_panel led_case_level_ / led_hot_level_) on reopen.
  lv_obj_t *cb[3] = {co, cm, cf}; seg_highlight(cb, 3, 2);
  lv_obj_t *hb[3] = {ho, hm, hf}; seg_highlight(hb, 3, 0);
  if (h) { h->case_off = co; h->case_50 = cm; h->case_full = cf;
           h->hot_off = ho; h->hot_50 = hm; h->hot_full = hf; }
}

void build_fans(lv_obj_t *parent, FansHandles *h) {
  lv_obj_t *back = screen_header(parent, "FANS");
  if (h) h->back = back;

  // One row per fan. The three user-settable fans get a live slider (drag to 0
  // = off); the two Klipper-managed fans show an AUTO pill + live %.
  static const char *names[5]    = {"PART COOLING", "MODEL FAN", "BOX FAN", "MAINBOARD", "HOTEND"};
  static const bool  settable[5] = {true, true, true, false, false};
  const int X = 12, W = 456, RH = 38, Y0 = 60, GAP = 4;
  for (int i = 0; i < 5; i++) {
    int y = Y0 + i * (RH + GAP);
    lv_obj_t *c = panel(parent, X, y, W, RH);
    lv_obj_t *nm = tag(c, names[i], color_text_secondary, 0, 0);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 14, 0);
    // Fans read 0 at boot (Klipper zeroes every fan on restart, which is when
    // build_fans runs) and the consume() loop only updates a fan on a non-null
    // speed delta -- an idle fan never sends one, so seed 0% not "--%".
    lv_obj_t *pv = lbl(c, "0%", font_num_small, color_accent_secondary, 0, 0);
    lv_obj_align(pv, LV_ALIGN_RIGHT_MID, -14, 0);
    if (h) h->val[i] = pv;
    if (settable[i]) {
      lv_obj_t *sl = lv_slider_create(c);
      lv_obj_set_size(sl, 188, 8);
      lv_obj_set_ext_click_area(sl, 16);  // an 8px track is too thin to grab on a resistive panel
      lv_obj_align(sl, LV_ALIGN_CENTER, 30, 0);
      lv_slider_set_range(sl, 0, 100);
      lv_slider_set_value(sl, 0, LV_ANIM_OFF);
      lv_obj_set_style_bg_color(sl, color_surface_elevated, LV_PART_MAIN);
      lv_obj_set_style_radius(sl, radius_sm, LV_PART_MAIN);
      lv_obj_set_style_bg_color(sl, color_accent_primary, LV_PART_INDICATOR);
      lv_obj_set_style_radius(sl, radius_sm, LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(sl, color_text_primary, LV_PART_KNOB);
      lv_obj_set_style_radius(sl, radius_sm, LV_PART_KNOB);
      if (h) h->slider[i] = sl;
    } else {
      lv_obj_t *pill = card(c, 0, 0, 50, 22, color_surface_elevated);
      hairline_c(pill, color_accent_secondary, opa_border_strong);
      lv_obj_align(pill, LV_ALIGN_CENTER, 30, 0);
      lv_obj_t *al = tag(pill, "AUTO", color_accent_secondary, 0, 0);
      lv_obj_center(al);
    }
  }
}

// Append one file row to the Files list (public: the app populates real files).
void files_add_row(lv_obj_t *list, const char *name, const char *meta) {
  lv_obj_t *r = lv_obj_create(list);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, lv_pct(100), 54);
  lv_obj_set_style_bg_color(r, color_surface_raised, 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(r, radius_sm, 0);
  hairline(r);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  // child 0: thumbnail image (hidden until the app loads one from metadata).
  // pivot 0,0 + pos so a zoom-to-fit lands the visual exactly in a 46px slot.
  lv_obj_t *th = lv_img_create(r);
  lv_obj_add_flag(th, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_radius(th, radius_sm, 0);
  lv_obj_set_style_clip_corner(th, true, 0);
  lv_img_set_pivot(th, 0, 0);
  lv_obj_set_pos(th, 8, 4);
  // child 1: fallback file glyph (shown until a thumbnail replaces it)
  lv_obj_t *ic = lbl(r, LV_SYMBOL_FILE, &lv_font_montserrat_14, color_text_secondary, 0, 0);
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 18, 0);
  // child 2: name, child 3: meta
  lv_obj_t *nm = lbl(r, name, font_caption, color_text_primary, 0, 0);
  lv_obj_set_width(nm, 360);                      // clamp so a long name can't run through the play icon
  lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);  // ellipsize instead of overflowing the row
  lv_obj_align(nm, LV_ALIGN_LEFT_MID, 62, -8);
  lv_obj_t *mt = lbl(r, meta, font_micro, color_text_secondary, 0, 0);
  lv_obj_align(mt, LV_ALIGN_LEFT_MID, 62, 10);
  // child 4: play
  lv_obj_t *pi = lbl(r, LV_SYMBOL_PLAY, &lv_font_montserrat_14, color_accent_primary, 0, 0);
  lv_obj_align(pi, LV_ALIGN_RIGHT_MID, -14, 0);
}

// Apply async metadata to a file row: swap the glyph for the loaded thumbnail
// (child 0 img, child 1 glyph) and refresh the meta line (child 3).
void files_apply_meta(lv_obj_t *row, const char *thumb_path, int zoom, const char *meta) {
  if (!row) return;
  lv_obj_t *th = lv_obj_get_child(row, 0);
  lv_obj_t *ic = lv_obj_get_child(row, 1);
  lv_obj_t *mt = lv_obj_get_child(row, 3);
  if (th && thumb_path && thumb_path[0]) {
    lv_img_set_src(th, thumb_path);
    lv_img_set_pivot(th, 0, 0);
    if (zoom > 0) lv_img_set_zoom(th, zoom);
    lv_obj_set_pos(th, 8, 4);
    lv_obj_clear_flag(th, LV_OBJ_FLAG_HIDDEN);
    if (ic) lv_obj_add_flag(ic, LV_OBJ_FLAG_HIDDEN);
  }
  if (mt && meta && meta[0]) lv_label_set_text(mt, meta);
}

void build_files(lv_obj_t *parent, FilesHandles *h) {
  lv_obj_t *back = screen_header(parent, "FILES");
  if (h) h->back = back;
  lv_obj_t *list = lv_obj_create(parent);
  lv_obj_remove_style_all(list);
  lv_obj_set_pos(list, 12, 52);
  lv_obj_set_size(list, 456, 206);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 8, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_color(list, color_text_secondary, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, LV_OPA_40, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(list, radius_sm, LV_PART_SCROLLBAR);
  if (h) h->list = list;
  // placeholder rows; the app clears + repopulates from Moonraker.
  files_add_row(list, "omega_cube.gcode", "18m  .  PA-CF");
  files_add_row(list, "benchy_0.25.gcode", "1h 12m  .  PA-CF");
  files_add_row(list, "bracket_v3.gcode", "42m  .  PLA");
  files_add_row(list, "phone_stand.gcode", "2h 04m  .  PETG");
}

// ---- boot / connecting screen ----------------------------------------------
// A two-act boot (Jack, 2026-06-13). Act 1 hides the boot behind the canned
// animation: the flag flies in, the island voice cracks a joke, the dedication
// signs off big, all while the real Klipper/Moonraker connect runs in the
// background. Act 2 (boot_reveal_progress) crossfades the joke out and phases a
// legit progress bar + live status in, showing what is actively loading. The
// joke and the progress share one band, so the flag stays the hero and the
// dedication goes big. build_boot lays it out; boot_play_intro + the app drive
// the acts.
void build_boot(lv_obj_t *parent, BootHandles *h) {
  lv_obj_set_style_bg_color(parent, color_surface_base, 0);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

  // The lamp comes on: a soft amber bloom behind the flag (the intro fades it
  // in first). Created first so it sits behind everything; the opaque flag
  // covers its centre, so the glow reads in the warm-black room around it.
  lv_obj_t *glow = lv_img_create(parent);
  lv_img_set_src(glow, &pono_glow);
  lv_obj_set_pos(glow, (480 - pono_glow_w) / 2, -60);
  if (h) h->glow = glow;

  // The flag, flying. Canned wave, near-zero CPU, frameless on the warm black.
  const int fw = pono_flag_boot_w, fh = pono_flag_boot_h;  // 320 x 160
  const int fx = (480 - fw) / 2, fy = 2;
  lv_obj_t *flag = boot_flag_create(parent);
  lv_obj_set_pos(flag, fx, fy);
  if (h) h->flag = flag;

  const int band = fy + fh + 4;   // the shared joke / progress band, y ~166

  // Act 1: the island's voice while we wait. Centered, dim, wraps; the app
  // cycles the book (the sim seeds one).
  lv_obj_t *jk = lbl(parent, "", font_caption, color_text_secondary, 0, 0);
  lv_obj_set_width(jk, 456);
  lv_label_set_long_mode(jk, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(jk, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(jk, LV_ALIGN_TOP_MID, 0, band);
  if (h) h->joke = jk;

  // Act 2: the live status line + a real progress bar (0..100, app-driven), in
  // the same band. boot_play_intro holds them dark until boot_reveal_progress.
  lv_obj_t *st = lbl(parent, "Waiting for Klipper to start...", font_caption, color_text_secondary, 0, 0);
  lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(st, LV_ALIGN_TOP_MID, 0, band + 2);
  if (h) h->status = st;

  lv_obj_t *bar = lv_bar_create(parent);
  lv_obj_set_size(bar, 300, 4);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, band + 24);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, 4, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, color_surface_elevated, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, color_accent_primary, LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
  if (h) h->bar = bar;

  // For the two it is all for. The watch signs off big, in the logbook's hand.
  lv_obj_t *ded = lbl(parent, "For Elio and Io", font_serif_xl, color_accent_primary, 0, 0);
  lv_obj_align(ded, LV_ALIGN_BOTTOM_MID, 0, -6);
  if (h) h->dedication = ded;

  // The fault view (boot_show_fault), built hidden. Klipper's own reason, two
  // lines at most, under the headline; then the one way out, where the
  // dedication sits, lit amber because it is the only action on the screen.
  lv_obj_t *rs = lbl(parent, "", font_caption, color_text_secondary, 0, 0);
  lv_obj_set_width(rs, 440);
  lv_obj_set_height(rs, 2 * lv_font_get_line_height(font_caption));
  lv_label_set_long_mode(rs, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(rs, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(rs, LV_ALIGN_TOP_MID, 0, band + 22);
  lv_obj_add_flag(rs, LV_OBJ_FLAG_HIDDEN);
  if (h) h->reason = rs;

  lv_obj_t *act = lamp_btn(parent, (480 - 200) / 2, 272 - 8 - 36, 200, 36, "Restart firmware", font_body);
  lv_obj_add_flag(act, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_ext_click_area(act, 8);   // the resistive panel wants a generous target
  if (h) h->action = act;
}

void boot_show_loading(BootHandles *h) {
  if (!h) return;
  if (h->status) {
    lv_obj_set_style_text_font(h->status, font_caption, 0);
    lv_obj_set_style_text_color(h->status, color_text_secondary, 0);
  }
  if (h->reason) lv_obj_add_flag(h->reason, LV_OBJ_FLAG_HIDDEN);
  if (h->action) lv_obj_add_flag(h->action, LV_OBJ_FLAG_HIDDEN);
  if (h->bar) lv_obj_clear_flag(h->bar, LV_OBJ_FLAG_HIDDEN);
  if (h->dedication) lv_obj_clear_flag(h->dedication, LV_OBJ_FLAG_HIDDEN);
}

void boot_show_fault(BootHandles *h, const char *headline, const char *reason) {
  if (!h) return;
  if (h->status) {
    lv_obj_set_style_text_font(h->status, font_body, 0);
    lv_obj_set_style_text_color(h->status, color_text_primary, 0);
    lv_obj_set_style_opa(h->status, LV_OPA_COVER, 0);
    if (headline) lv_label_set_text(h->status, headline);
  }
  if (h->reason) {
    lv_label_set_text(h->reason, reason ? reason : "");
    if (reason && *reason) lv_obj_clear_flag(h->reason, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(h->reason, LV_OBJ_FLAG_HIDDEN);
  }
  if (h->bar) lv_obj_add_flag(h->bar, LV_OBJ_FLAG_HIDDEN);
  if (h->dedication) lv_obj_add_flag(h->dedication, LV_OBJ_FLAG_HIDDEN);
  if (h->action) lv_obj_clear_flag(h->action, LV_OBJ_FLAG_HIDDEN);
}

void boot_set_progress(BootHandles *h, int pct, const char *stage) {
  if (!h) return;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (h->bar) {
    const bool forward = pct >= lv_bar_get_value(h->bar);
    lv_bar_set_value(h->bar, pct, forward ? LV_ANIM_ON : LV_ANIM_OFF);
  }
  if (h->status && stage) lv_label_set_text(h->status, stage);
}

namespace {
// Boot-intro property anims: fade opacity in, and (optionally) rise from a
// small downward offset, eased. Captureless callbacks so they take function
// pointers; the rise and the fade are two anims keyed by var + exec_cb.
void intro_set_opa(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
void intro_set_ty(void *o, int32_t v)  { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }

void fade_rise(lv_obj_t *o, int rise, uint32_t delay, uint32_t dur, lv_anim_path_cb_t rise_path) {
  if (!o) return;
  lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);   // start dark; the anim brings it up
  lv_anim_t fa;
  lv_anim_init(&fa);
  lv_anim_set_var(&fa, o);
  lv_anim_set_time(&fa, dur);
  lv_anim_set_delay(&fa, delay);
  lv_anim_set_path_cb(&fa, lv_anim_path_ease_out);
  lv_anim_set_values(&fa, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_exec_cb(&fa, intro_set_opa);
  lv_anim_start(&fa);
  if (rise) {
    lv_obj_set_style_translate_y(o, rise, 0);
    lv_anim_t ra;
    lv_anim_init(&ra);
    lv_anim_set_var(&ra, o);
    lv_anim_set_time(&ra, dur);
    lv_anim_set_delay(&ra, delay);
    lv_anim_set_path_cb(&ra, rise_path);
    lv_anim_set_values(&ra, rise, 0);
    lv_anim_set_exec_cb(&ra, intro_set_ty);
    lv_anim_start(&ra);
  }
}
} // namespace

void boot_play_intro(BootHandles *h) {
  if (!h) return;
  // Act 1, the wake that hides the boot: the lamp glow warms up, the flag
  // catches the wind and flies in, the joke cracks, and the dedication lands
  // last with a touch of overshoot. The progress bar + status are held dark for
  // Act 2, so the real connect runs behind the animation. One-shot on first
  // boot; a later disconnected re-show keeps the settled state.
  if (h->status) lv_obj_set_style_opa(h->status, LV_OPA_TRANSP, 0);
  if (h->bar)    lv_obj_set_style_opa(h->bar, LV_OPA_TRANSP, 0);
  fade_rise(h->glow,        0,   0, 760, lv_anim_path_ease_out);
  fade_rise(h->flag,       14,   0, 560, lv_anim_path_ease_out);
  fade_rise(h->joke,        8, 380, 380, lv_anim_path_ease_out);
  fade_rise(h->dedication, 16, 760, 640, lv_anim_path_overshoot);
}

void boot_reveal_progress(BootHandles *h) {
  if (!h) return;
  // Act 2: the playful wait gives way to real loading. The joke fades out and
  // the legit progress bar + live status fade in, in the same band, at 60 fps.
  if (h->joke) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, h->joke);
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&a, intro_set_opa);
    lv_anim_start(&a);
  }
  fade_rise(h->status, 0, 340, 360, lv_anim_path_ease_out);
  fade_rise(h->bar,    0, 420, 360, lv_anim_path_ease_out);
}

} // namespace pono
