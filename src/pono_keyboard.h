// SPDX-License-Identifier: GPL-3.0-only
// pono_keyboard.h - the password sheet: a full-panel layer with a caption,
// a one-line field and a 4-row keyboard, laid out clear of the E-STOP.
#pragma once
#include "lvgl.h"
#include "pono_theme.h"

namespace pono {

constexpr lv_coord_t pw_caption_x = 12;
constexpr lv_coord_t pw_caption_y = 14;
constexpr lv_coord_t pw_caption_w = estop_clear_x - 12 - pw_caption_x;  // 372
constexpr lv_coord_t pw_input_x   = 8;
constexpr lv_coord_t pw_input_y   = estop_clear_y;                      // 44
constexpr lv_coord_t pw_input_w   = frame_width - 2 * pw_input_x;       // 464
constexpr lv_coord_t pw_input_h   = 40;
constexpr lv_coord_t pw_kb_y      = pw_input_y + pw_input_h + 8;        // 92
constexpr lv_coord_t pw_kb_h      = frame_height - pw_kb_y;             // 180

struct PasswordSheet {
  lv_obj_t *sheet = nullptr;
  lv_obj_t *caption = nullptr;
  lv_obj_t *input = nullptr;
  lv_obj_t *kb = nullptr;
};

// Build the sheet hidden on parent (FLOATING, so a flex parent ignores it).
void password_sheet_build(lv_obj_t *parent, PasswordSheet *ps);
// Show it with this caption. Already open: only the caption changes and the
// typed text stays. Fresh open: empty field, lower-case page, cursor on.
void password_sheet_open(PasswordSheet *ps, const char *caption);
// Hide it and clear the field, so a password never lingers in the widget.
void password_sheet_close(PasswordSheet *ps);
bool password_sheet_is_open(const PasswordSheet *ps);
// Pono key styling for an lv_keyboard (numpad look, symbol-capable font).
void keyboard_style(lv_obj_t *kb);

}  // namespace pono
