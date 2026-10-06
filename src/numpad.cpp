#include "numpad.h"
#include "pono_theme.h"
#include "pono_anim.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

enum { NP_1 = 0, NP_2, NP_3, NP_BKSP,
       NP_4, NP_5, NP_6, NP_PM,
       NP_7, NP_8, NP_9, NP_DOT,
       NP_ZERO, NP_OK };

static std::string fmt_num(double v, int decimals) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return std::string(buf);
}

void Numpad::_draw_part(lv_event_t *e) {
  lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
  if (dsc->part != LV_PART_ITEMS) return;
  if (dsc->id == NP_BKSP) {
    if (dsc->label_dsc) dsc->label_dsc->font = &lv_font_montserrat_14;
  } else if (dsc->id == (uint32_t)NP_OK &&
             !lv_btnmatrix_has_btn_ctrl(lv_event_get_target(e), NP_OK, LV_BTNMATRIX_CTRL_DISABLED)) {
    if (dsc->rect_dsc) dsc->rect_dsc->bg_color = pono::color_accent_primary;
    if (dsc->label_dsc) dsc->label_dsc->color = pono::color_surface_base;
  }
}

Numpad::Numpad(lv_obj_t *parent)
  : panel(lv_obj_create(parent))
  , title_label(lv_label_create(panel))
  , entry_label(lv_label_create(panel))
  , unit_label(lv_label_create(panel))
  , range_label(lv_label_create(panel))
  , was_label(lv_label_create(panel))
  , cancel_btn(lv_btn_create(panel))
  , kb(lv_btnmatrix_create(panel))
{
  lv_obj_remove_style_all(panel);
  lv_obj_set_pos(panel, 0, 0);
  lv_obj_set_size(panel, 480, 272);
  lv_obj_set_style_bg_color(panel, pono::color_surface_base, 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(panel, 2, 0);
  lv_obj_set_style_shadow_width(panel, 0, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

  lv_obj_set_pos(title_label, 16, 16);
  lv_obj_set_style_text_font(title_label, pono::font_micro, 0);
  lv_obj_set_style_text_color(title_label, pono::color_text_tertiary, 0);
  lv_obj_set_style_text_letter_space(title_label, 1, 0);

  lv_obj_set_pos(entry_label, 16, 44);
  lv_obj_set_style_text_font(entry_label, pono::font_num_large, 0);
  lv_obj_set_style_text_color(entry_label, pono::color_accent_primary, 0);

  lv_obj_set_pos(unit_label, 16, 88);
  lv_obj_set_style_text_font(unit_label, pono::font_body, 0);
  lv_obj_set_style_text_color(unit_label, pono::color_text_secondary, 0);

  lv_obj_set_pos(range_label, 16, 116);
  lv_obj_set_style_text_font(range_label, pono::font_caption, 0);
  lv_obj_set_style_text_color(range_label, pono::color_text_tertiary, 0);

  lv_obj_set_pos(was_label, 16, 136);
  lv_obj_set_style_text_font(was_label, pono::font_caption, 0);
  lv_obj_set_style_text_color(was_label, pono::color_text_tertiary, 0);

  lv_obj_set_pos(cancel_btn, 16, 212);
  lv_obj_set_size(cancel_btn, 160, 48);
  lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(cancel_btn, 1, 0);
  lv_obj_set_style_border_color(cancel_btn, pono::color_text_tertiary, 0);
  lv_obj_set_style_radius(cancel_btn, 2, 0);
  lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
  lv_obj_t *cancel_label = lv_label_create(cancel_btn);
  lv_label_set_text(cancel_label, "Cancel");
  lv_obj_set_style_text_font(cancel_label, pono::font_body, 0);
  lv_obj_set_style_text_color(cancel_label, pono::color_text_primary, 0);
  lv_obj_center(cancel_label);
  lv_obj_add_event_cb(cancel_btn, &Numpad::_handle_cancel, LV_EVENT_CLICKED, this);

  static const char *map[] = {
    "1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
    "4", "5", "6", "+/-", "\n",
    "7", "8", "9", ".", "\n",
    "0", "OK", "" };
  lv_obj_set_pos(kb, 184, 52);
  lv_obj_set_size(kb, 280, 212);
  lv_btnmatrix_set_map(kb, map);

  static lv_btnmatrix_ctrl_t ctrl_map[14];
  for (int i = 0; i < 14; i++) ctrl_map[i] = 1;  // LVGL 8.3: width units are the low 4 bits of the ctrl value
  ctrl_map[NP_ZERO] = 2;
  ctrl_map[NP_OK] = 2;
  lv_btnmatrix_set_ctrl_map(kb, ctrl_map);

  lv_obj_set_style_pad_gap(kb, 4, 0);
  lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(kb, 0, LV_PART_MAIN);

  lv_obj_set_style_bg_color(kb, pono::color_surface_raised, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(kb, pono::color_text_tertiary, LV_PART_ITEMS);
  lv_obj_set_style_border_opa(kb, pono::opa_border_subtle, LV_PART_ITEMS);
  lv_obj_set_style_radius(kb, 2, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_text_font(kb, pono::font_num_medium, LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, pono::color_text_primary, LV_PART_ITEMS);
  lv_obj_set_style_transition(kb, NULL, LV_PART_ITEMS);

  lv_obj_set_style_bg_color(kb, pono::color_surface_elevated, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_border_color(kb, pono::color_accent_primary, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_border_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
  // Disabled keys (OK while the entry is out of range) sit dark, so the
  // amber lamp only lights an action that will take.
  lv_obj_set_style_bg_color(kb, pono::color_surface_base, LV_PART_ITEMS | LV_STATE_DISABLED);
  lv_obj_set_style_text_color(kb, pono::color_text_tertiary, LV_PART_ITEMS | LV_STATE_DISABLED);

  lv_obj_add_event_cb(kb, &Numpad::_handle_key, LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(kb, &Numpad::_draw_part, LV_EVENT_DRAW_PART_BEGIN, this);
}

void Numpad::set_callback(std::function<void(double)> cb) {
  persistent_cb = cb;
}

void Numpad::foreground_reset() {
  NumpadSpec s;
  s.title = "";
  s.unit = "";
  s.value = NAN;
  s.min = 0;
  s.max = 0;
  s.decimals = 3;
  open(s, persistent_cb);
}

void Numpad::open(const NumpadSpec &new_spec, std::function<void(double)> on_ok) {
  spec = new_spec;
  on_ok_cb = on_ok;
  untouched = true;
  if (std::isnan(spec.value)) {
    typed = "";
  } else {
    typed = fmt_num(spec.value, spec.decimals);
  }

  lv_label_set_text(title_label, spec.title);
  lv_label_set_text(unit_label, spec.unit);

  bool bounded = spec.min != spec.max;
  if (bounded) {
    std::string r = fmt_num(spec.min, spec.decimals) + " to " + fmt_num(spec.max, spec.decimals) + " " + spec.unit;
    lv_label_set_text(range_label, r.c_str());
    lv_obj_clear_flag(range_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(range_label, LV_OBJ_FLAG_HIDDEN);
  }

  if (std::isnan(spec.value)) {
    lv_obj_add_flag(was_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    std::string w = "was " + fmt_num(spec.value, spec.decimals);
    lv_label_set_text(was_label, w.c_str());
    lv_obj_clear_flag(was_label, LV_OBJ_FLAG_HIDDEN);
  }

  update_keys();
  update_display();
  validate();

  lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
  pono::raise_overlay(panel);
}

void Numpad::update_keys() {
  if (spec.min < 0) {
    lv_btnmatrix_clear_btn_ctrl(kb, NP_PM, LV_BTNMATRIX_CTRL_HIDDEN);
  } else {
    lv_btnmatrix_set_btn_ctrl(kb, NP_PM, LV_BTNMATRIX_CTRL_HIDDEN);
  }
  if (spec.decimals > 0) {
    lv_btnmatrix_clear_btn_ctrl(kb, NP_DOT, LV_BTNMATRIX_CTRL_HIDDEN);
  } else {
    lv_btnmatrix_set_btn_ctrl(kb, NP_DOT, LV_BTNMATRIX_CTRL_HIDDEN);
  }
}

void Numpad::update_display() {
  lv_label_set_text(entry_label, typed.c_str());
  if (untouched) {
    lv_obj_set_style_text_color(entry_label, pono::color_text_secondary, 0);
  } else {
    lv_obj_set_style_text_color(entry_label, pono::color_accent_primary, 0);
  }
}

void Numpad::validate() {
  bool valid = false;
  double val = 0;
  if (!typed.empty() && typed != "-" && typed != "." && typed != "-.") {
    char *endp = nullptr;
    val = strtod(typed.c_str(), &endp);
    if (endp && *endp == '\0') valid = true;
  }

  bool bounded = spec.min != spec.max;
  bool in_range = true;
  if (valid && bounded) {
    in_range = (val >= spec.min && val <= spec.max);
  }

  if (bounded) {
    lv_obj_set_style_text_color(range_label,
      (valid && !in_range) ? pono::color_state_error : pono::color_text_tertiary, 0);
  }

  bool ok_enabled = valid && in_range;
  if (ok_enabled) {
    lv_btnmatrix_clear_btn_ctrl(kb, NP_OK, LV_BTNMATRIX_CTRL_DISABLED);
  } else {
    lv_btnmatrix_set_btn_ctrl(kb, NP_OK, LV_BTNMATRIX_CTRL_DISABLED);
  }
}

static void toggle_sign(std::string &s) {
  if (!s.empty() && s[0] == '-') {
    s.erase(0, 1);
  } else {
    s.insert(s.begin(), '-');
  }
}

void Numpad::handle_key(lv_event_t *e) {
  lv_obj_t *bm = lv_event_get_target(e);
  uint32_t id = lv_btnmatrix_get_selected_btn(bm);
  if (id == LV_BTNMATRIX_BTN_NONE) return;
  const char *txt = lv_btnmatrix_get_btn_text(bm, id);
  if (txt == nullptr) return;

  if (id == NP_OK) {
    if (untouched) {
      dismiss();
      return;
    }
    char *endp = nullptr;
    double val = strtod(typed.c_str(), &endp);
    if (!(endp && *endp == '\0')) return;
    auto cb = on_ok_cb;
    dismiss();
    if (cb) cb(val);
    return;
  }

  if (id == NP_BKSP) {
    if (untouched) {
      typed = "";
      untouched = false;
    } else if (!typed.empty()) {
      typed.erase(typed.size() - 1, 1);
    }
    update_display();
    validate();
    return;
  }

  if (id == NP_PM) {
    untouched = false;
    toggle_sign(typed);
    update_display();
    validate();
    return;
  }

  if (strcmp(txt, ".") == 0) {
    if (untouched) {
      typed = "0.";
      untouched = false;
    } else if (typed.find('.') == std::string::npos && typed.size() < 7) {
      if (typed.empty()) typed = "0";
      typed += ".";
    }
    update_display();
    validate();
    return;
  }

  // digit
  if (untouched) {
    typed = txt;
    untouched = false;
  } else {
    if (typed.size() >= 7) { update_display(); validate(); return; }
    size_t dot = typed.find('.');
    if (dot != std::string::npos) {
      int after = (int)(typed.size() - dot - 1);
      if (after >= spec.decimals) { update_display(); validate(); return; }
    }
    typed += txt;
  }
  update_display();
  validate();
}

void Numpad::dismiss() {
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

bool Numpad::is_open() const {
  return !lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN);
}
