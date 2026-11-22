// SPDX-License-Identifier: GPL-3.0-only
// pono_keyboard.cpp - builds and drives the WiFi password sheet and its keyboard.
#include "pono_keyboard.h"

// The symbol page: with the stock lower and upper pages, every printable
// ASCII character (0x20 to 0x7E) has a key. 44 keys, 44 ctrl values.
static const char *kb_spec_map[] = {
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "~", LV_SYMBOL_BACKSPACE, "\n",
  "abc", "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "=", "+", "\n",
  "`", "|", "\\", "{", "}", "[", "]", "<", ">", ";", "'", "\"", "/", "?", "\n",
  LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const lv_btnmatrix_ctrl_t kb_spec_ctrl[] = {
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, LV_BTNMATRIX_CTRL_CHECKED | 3,
  LV_KEYBOARD_CTRL_BTN_FLAGS | 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  LV_KEYBOARD_CTRL_BTN_FLAGS | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6,
  LV_BTNMATRIX_CTRL_CHECKED | 2, LV_KEYBOARD_CTRL_BTN_FLAGS | 2
};

namespace pono {

void password_sheet_build(lv_obj_t *parent, PasswordSheet *ps) {
  if (!parent || !ps) return;

  lv_obj_t *s = lv_obj_create(parent);
  ps->sheet = s;
  lv_obj_add_flag(s, LV_OBJ_FLAG_FLOATING);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICK_FOCUSABLE);
  lv_obj_set_size(s, LV_PCT(100), LV_PCT(100));
  lv_obj_set_pos(s, 0, 0);
  lv_obj_set_style_pad_all(s, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(s, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, pono::color_surface_base, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *c = lv_label_create(s);
  ps->caption = c;
  lv_label_set_text(c, "");
  lv_label_set_long_mode(c, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(c, pono::font_body, LV_PART_MAIN);
  lv_obj_set_style_text_color(c, pono::color_text_primary, LV_PART_MAIN);
  lv_obj_set_pos(c, pw_caption_x, pw_caption_y);
  lv_obj_set_size(c, pw_caption_w, lv_font_get_line_height(pono::font_body));

  lv_obj_t *in = lv_textarea_create(s);
  ps->input = in;
  lv_textarea_set_one_line(in, true);  // resets height, so size comes after
  lv_obj_set_pos(in, pw_input_x, pw_input_y);
  lv_obj_set_size(in, pw_input_w, pw_input_h);
  lv_textarea_set_placeholder_text(in, "type the password");
  lv_obj_set_style_text_font(in, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(in, pono::color_text_primary, LV_PART_MAIN);
  lv_obj_set_style_bg_color(in, pono::color_surface_raised, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(in, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(in, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(in, pono::color_text_tertiary, LV_PART_MAIN);
  lv_obj_set_style_border_opa(in, pono::opa_border_medium, LV_PART_MAIN);
  lv_obj_set_style_radius(in, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_left(in, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_right(in, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_top(in, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(in, 8, LV_PART_MAIN);
  lv_obj_set_style_border_color(in, pono::color_accent_primary, LV_PART_MAIN | LV_STATE_FOCUSED);
  lv_obj_set_style_border_opa(in, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_FOCUSED);
  lv_obj_set_style_outline_width(in, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
  lv_obj_set_style_text_color(in, pono::color_text_secondary, LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_border_side(in, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(in, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
  lv_obj_set_style_border_color(in, pono::color_accent_primary, LV_PART_CURSOR | LV_STATE_FOCUSED);
  lv_obj_set_style_border_opa(in, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
  lv_obj_set_style_bg_opa(in, LV_OPA_TRANSP, LV_PART_CURSOR | LV_STATE_FOCUSED);

  lv_obj_t *kb = lv_keyboard_create(s);
  ps->kb = kb;
  lv_obj_set_size(kb, LV_PCT(100), pw_kb_h);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(kb, in);
  lv_keyboard_set_popovers(kb, false);
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, kb_spec_map, kb_spec_ctrl);
  keyboard_style(kb);
}

void keyboard_style(lv_obj_t *kb) {
  lv_obj_set_style_pad_all(kb, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_gap(kb, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(kb, pono::color_surface_base, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(kb, 0, LV_PART_MAIN);

  lv_obj_set_style_bg_color(kb, pono::color_surface_raised, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(kb, pono::color_text_tertiary, LV_PART_ITEMS);
  lv_obj_set_style_border_opa(kb, pono::opa_border_subtle, LV_PART_ITEMS);
  lv_obj_set_style_radius(kb, 2, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_text_font(kb, &lv_font_montserrat_16, LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, pono::color_text_primary, LV_PART_ITEMS);
  lv_obj_set_style_transition(kb, NULL, LV_PART_ITEMS);

  lv_obj_set_style_bg_color(kb, pono::color_surface_elevated, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_border_color(kb, pono::color_accent_primary, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_border_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);

  lv_obj_set_style_bg_color(kb, pono::color_surface_elevated, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(kb, pono::color_text_secondary, LV_PART_ITEMS | LV_STATE_CHECKED);
}

void password_sheet_open(PasswordSheet *ps, const char *caption) {
  if (!ps || !ps->sheet) return;
  lv_label_set_text(ps->caption, caption ? caption : "");
  if (password_sheet_is_open(ps)) return;  // keep what was typed
  lv_textarea_set_text(ps->input, "");
  lv_keyboard_set_mode(ps->kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_add_state(ps->input, LV_STATE_FOCUSED);  // the cursor style needs it
  lv_obj_clear_flag(ps->sheet, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(ps->sheet);
}

void password_sheet_close(PasswordSheet *ps) {
  if (!ps || !ps->sheet) return;
  lv_textarea_set_text(ps->input, "");
  lv_obj_clear_state(ps->input, LV_STATE_FOCUSED);
  lv_obj_add_flag(ps->sheet, LV_OBJ_FLAG_HIDDEN);
}

bool password_sheet_is_open(const PasswordSheet *ps) {
  return ps && ps->sheet && !lv_obj_has_flag(ps->sheet, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace pono
