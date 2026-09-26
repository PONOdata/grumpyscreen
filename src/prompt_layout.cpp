#include "prompt_layout.h"
#include "pono_theme.h"

// Why this file exists, measured on 0.1.15 hardware 2026-08-11.
//
// _CALIBRATE_ALL_STEP_1 emits three action:prompt_text lines. On the glass, two
// of the three were cut: line 1 lost "you will be prompted to save the updated
// configuration.", and line 2 stopped at "The extruder PID will be" so the
// hotend temperature (265C) never appeared at all. The operator was asked to
// confirm a run whose stated parameters were not on the screen.
//
// Two mechanisms, both in the geometry:
//
//   1. The card was a 3-row grid of LV_GRID_FR(1), i.e. hard thirds, and the
//      body was given its own percentage height (70% of the card) that did not
//      match its third. A non-STRETCH grid item keeps its own size, so the body
//      overflowed its cell into the header and footer rows, which are drawn
//      after it and therefore over it. That is the half-drawn row of glyph tops.
//
//   2. Every prompt_text label was created with a fixed 40px height AND
//      lv_obj_set_flex_grow(1). In a column flow, grow divides the container
//      among the children, so three labels each got about a third of the body
//      regardless of how tall their wrapped text actually was: room for two
//      wrapped rows at font_body (14px). Any line needing a third row lost it.
//
// The old check_height() could not see either one. It inspected only the LAST
// child, and because flex_grow makes children exactly fill the container, its
// test (child bottom past the container) is false by construction. The overflow
// was INSIDE each label, which it never measured. A guard that cannot observe
// the thing it guards reads as passing.
//
// So: the body sizes to its content and the card grows with it, bounded by the
// screen. Only when the content genuinely cannot fit does the body become a
// scrollable viewport. Text is never silently dropped either way.

namespace pono {

void prompt_layout_build(lv_obj_t *parent, PromptLayout *l) {
  l->cont   = lv_obj_create(parent);
  l->header = lv_label_create(l->cont);
  l->body   = lv_obj_create(l->cont);
  l->footer = lv_obj_create(l->cont);

  // The card: a vertical stack that is as tall as what it holds, capped by the
  // panel. No fixed row track, so nothing can overflow a neighbour's cell.
  lv_obj_set_style_pad_all(l->cont, 5, 0);
  lv_obj_set_style_pad_row(l->cont, 4, 0);
  lv_obj_set_style_radius(l->cont, pono::radius_sm, LV_PART_MAIN);
  lv_obj_set_style_bg_color(l->cont, pono::color_surface_raised, LV_PART_MAIN);
  lv_obj_set_style_border_width(l->cont, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_color(l->cont, pono::color_text_primary, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_opa(l->cont, pono::opa_border_medium, LV_PART_MAIN | LV_STATE_DEFAULT);
  // Held below the header row, where the E-STOP rides on every screen, with the
  // 12px outer margin under it.
  const lv_coord_t band_h = lv_obj_get_height(parent) - pono::estop_clear_y - 12;
  lv_obj_set_style_max_height(l->cont, band_h, 0);
  lv_obj_set_style_max_width(l->cont, lv_pct(94), 0);
  lv_obj_set_style_min_width(l->cont, lv_pct(60), 0);
  lv_obj_set_width(l->cont, lv_pct(90));
  lv_obj_set_height(l->cont, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(l->cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(l->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(l->cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(l->cont, LV_ALIGN_CENTER, 0, (pono::estop_clear_y - 12) / 2);  // centred in that band

  // The title is one line, ellipsized. LV_LABEL_LONG_DOT honours HEIGHT as well
  // as width, so leaving the height auto makes an overlong title wrap and spill
  // into the body instead of ellipsizing. Pin it to exactly one line.
  lv_obj_set_style_text_font(l->header, pono::font_body, LV_PART_MAIN);
  lv_obj_set_width(l->header, lv_pct(100));
  lv_obj_set_height(l->header, lv_font_get_line_height(pono::font_body));
  lv_label_set_long_mode(l->header, LV_LABEL_LONG_DOT);
  lv_label_set_text(l->header, "");

  // The body holds the prompt_text lines and any inline button group.
  lv_obj_set_width(l->body, lv_pct(100));
  lv_obj_set_height(l->body, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(l->body, 0, 0);
  lv_obj_set_style_pad_row(l->body, 3, 0);
  lv_obj_set_style_bg_opa(l->body, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(l->body, 0, 0);
  lv_obj_set_style_outline_width(l->body, 0, 0);
  lv_obj_set_flex_flow(l->body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(l->body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(l->body, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_set_width(l->footer, lv_pct(100));
  lv_obj_set_height(l->footer, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(l->footer, 0, 0);
  lv_obj_set_style_pad_column(l->footer, 6, 0);
  lv_obj_set_style_bg_opa(l->footer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(l->footer, 0, 0);
  lv_obj_set_style_outline_width(l->footer, 0, 0);
  lv_obj_set_flex_flow(l->footer, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(l->footer, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(l->footer, LV_OBJ_FLAG_SCROLLABLE);
}

void prompt_layout_reset(PromptLayout *l) {
  lv_obj_clean(l->body);
  lv_obj_clean(l->footer);
  // Undo any growth or scrolling the previous prompt needed, so a short prompt
  // after a tall one is not left in the tall one's shape.
  lv_obj_set_height(l->cont, LV_SIZE_CONTENT);
  lv_obj_set_height(l->body, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_right(l->body, 0, 0);
  lv_obj_clear_flag(l->body, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *prompt_layout_add_text(PromptLayout *l, const char *text) {
  lv_obj_t *line = lv_label_create(l->body);
  lv_obj_set_width(line, lv_pct(100));
  // Height follows the wrapped text. No fixed height and no flex_grow: either
  // one makes the box a size the text has to fit rather than the reverse.
  lv_obj_set_height(line, LV_SIZE_CONTENT);
  lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_outline_pad(line, 0, 0);
  lv_label_set_text(line, text);
  return line;
}

lv_obj_t *prompt_layout_add_button_row(PromptLayout *l) {
  lv_obj_t *row = lv_obj_create(l->body);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_pad_column(row, 6, 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_outline_width(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  return row;
}

void prompt_layout_fit(PromptLayout *l) {
  lv_obj_update_layout(l->cont);

  // The card is LV_SIZE_CONTENT bounded by max_height. If the content needed
  // more than the panel allows, the card is now clamped and its children hang
  // past the bottom by exactly scroll_bottom. Hand that overflow to the body as
  // a scroll viewport rather than letting it fall off the card.
  lv_coord_t over = lv_obj_get_scroll_bottom(l->cont);
  if (over > 0) {
    lv_coord_t body_h = lv_obj_get_height(l->body);
    lv_coord_t fit_h  = body_h - over;
    if (fit_h < 24) fit_h = 24;  // always leave a usable viewport
    lv_obj_set_height(l->body, fit_h);
    lv_obj_set_style_pad_right(l->body, 6, 0);  // room for the scrollbar
    lv_obj_add_flag(l->body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(l->body, LV_DIR_VER);
    lv_obj_scroll_to_y(l->body, 0, LV_ANIM_OFF);
    lv_obj_update_layout(l->cont);
  } else {
    lv_obj_clear_flag(l->body, LV_OBJ_FLAG_SCROLLABLE);
  }
}

}  // namespace pono
