#include "numpad.h"
#include "pono_theme.h"
#include "logger.h"
#include "pono_anim.h"  // raise_overlay: the keypad sits on lv_layer_top

#include <cstring>
#include <string>

// Button ids in the keypad map below (row-major). Cancel + OK are recolored
// in the draw hook and handled specially in the event callback.
enum { NP_DOT = 9, NP_ZERO = 10, NP_BKSP = 11, NP_CANCEL = 12, NP_OK = 13 };

// Recolor the Cancel (red) and OK (cyan) keys so they read as distinct
// actions, with dark text for contrast on the bright fills.
static void numpad_draw_cb(lv_event_t *e) {
  lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
  if (dsc->part != LV_PART_ITEMS) return;
  if (!dsc->rect_dsc) return;  // match the label_dsc guard below; no rect to recolor
  if (dsc->id == NP_OK) {
    dsc->rect_dsc->bg_color = pono::color_accent_primary;
    if (dsc->label_dsc) dsc->label_dsc->color = pono::color_surface_base;
  } else if (dsc->id == NP_CANCEL) {
    dsc->rect_dsc->bg_color = pono::color_state_error;
    if (dsc->label_dsc) dsc->label_dsc->color = pono::color_surface_base;
  }
}

Numpad::Numpad(lv_obj_t *parent)
  : scrim(lv_obj_create(parent))
  , edit_cont(lv_obj_create(parent))
  , input(lv_textarea_create(edit_cont))
  , kb(lv_btnmatrix_create(edit_cont))
  , ready_cb([](double v){})
{
  // Full-screen modal backdrop behind the card. Without it, taps outside
  // the keypad fall through lv_layer_top to the screen below and actuate
  // whatever sits there (e.g. an Expert Tune preset chip). Tap = cancel.
  lv_obj_remove_style_all(scrim);
  lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scrim, LV_OPA_40, 0);
  lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scrim, &Numpad::_handle_scrim, LV_EVENT_CLICKED, this);

  // Centered modal card (was a cramped 48% right-edge panel).
  lv_obj_add_flag(edit_cont, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(edit_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(edit_cont, 300, 250);
  lv_obj_align(edit_cont, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(edit_cont, pono::color_surface_raised, 0);
  lv_obj_set_style_bg_opa(edit_cont, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(edit_cont, 14, 0);
  lv_obj_set_style_border_color(edit_cont, pono::color_accent_primary, 0);
  lv_obj_set_style_border_width(edit_cont, 1, 0);
  lv_obj_set_style_border_opa(edit_cont, LV_OPA_70, 0);
  lv_obj_set_style_shadow_color(edit_cont, lv_color_black(), 0);
  lv_obj_set_style_shadow_width(edit_cont, 24, 0);
  lv_obj_set_style_shadow_opa(edit_cont, LV_OPA_50, 0);
  lv_obj_set_style_pad_all(edit_cont, 10, 0);
  lv_obj_set_flex_flow(edit_cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(edit_cont, 8, 0);
  lv_obj_move_background(edit_cont);

  // Value display (read-only; the keypad drives it).
  lv_obj_set_width(input, LV_PCT(100));
  lv_textarea_set_one_line(input, true);
  lv_obj_clear_flag(input, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_text_align(input, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_font(input, pono::font_num_medium, 0);
  lv_obj_set_style_text_color(input, pono::color_text_primary, 0);
  lv_obj_set_style_bg_color(input, pono::color_surface_base, 0);
  lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(input, 0, 0);
  lv_obj_set_style_radius(input, 8, 0);

  static const char *map[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    ".", "0", LV_SYMBOL_BACKSPACE, "\n",
    "Cancel", "OK", "" };
  lv_btnmatrix_set_map(kb, map);
  lv_obj_set_width(kb, LV_PCT(100));
  lv_obj_set_flex_grow(kb, 1);
  lv_obj_set_style_pad_all(kb, 0, 0);
  lv_obj_set_style_pad_gap(kb, 6, 0);
  lv_obj_set_style_bg_opa(kb, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
  // keys: dark neutral fill + light text, readable
  lv_obj_set_style_bg_color(kb, pono::color_surface_elevated, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, pono::color_text_primary, LV_PART_ITEMS);
  lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS);
  lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, LV_PART_ITEMS);  // carries digits + LV_SYMBOL glyphs (backspace)
  lv_obj_set_style_bg_color(kb, pono::color_accent_primary, LV_PART_ITEMS | LV_STATE_PRESSED);

  lv_obj_add_event_cb(kb, &Numpad::_handle_input, LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(kb, numpad_draw_cb, LV_EVENT_DRAW_PART_BEGIN, this);
}

Numpad::~Numpad() {
  if (edit_cont != NULL) {
    lv_obj_del(edit_cont);
    edit_cont = NULL;
  }
  if (scrim != NULL) {
    lv_obj_del(scrim);
    scrim = NULL;
  }
}

void Numpad::set_callback(std::function<void(double)> cb) {
  ready_cb = cb;
}

void Numpad::handle_input(lv_event_t *e) {
  lv_obj_t *bm = lv_event_get_target(e);
  uint32_t id = lv_btnmatrix_get_selected_btn(bm);
  const char *txt = lv_btnmatrix_get_btn_text(bm, id);
  if (txt == NULL) return;

  if (strcmp(txt, "Cancel") == 0) {
    dismiss();
    return;
  }
  if (strcmp(txt, "OK") == 0) {
    std::string value = std::string(lv_textarea_get_text(input));
    if (value.length() > 0) {
      try { ready_cb(std::stod(value)); } catch (...) {}
    }
    dismiss();
    return;
  }
  if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
    lv_textarea_del_char(input);
    return;
  }
  lv_textarea_add_text(input, txt);  // a digit or the decimal point
}

void Numpad::foreground_reset() {
  LOG_TRACE("resetting foreground");
  lv_textarea_set_text(input, "");
  lv_obj_clear_flag(scrim, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(edit_cont, LV_OBJ_FLAG_HIDDEN);
  pono::raise_overlay(scrim);      // backdrop above the page...
  pono::raise_overlay(edit_cont);  // ...card above the backdrop, E-STOP above both
}

void Numpad::dismiss() {
  lv_textarea_set_text(input, "");
  lv_obj_add_flag(edit_cont, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_background(edit_cont);
  lv_obj_move_background(scrim);
}
