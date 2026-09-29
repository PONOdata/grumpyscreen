#include "prompt_panel.h"
#include "prompt_layout.h"
#include "state.h"
#include "utils.h"
#include "logger.h"
#include "pono_theme.h"  // Kukui semantic tokens
#include "pono_anim.h"   // busy_hide: a prompt must not sit behind the cal/busy overlay

// uncomment for helper boxes
// #define DEBUG_LINES

static lv_style_t style_btn_grey;
static lv_style_t style_btn_blue;
static lv_style_t style_btn_red;
static lv_style_t style_btn_orange;
static lv_style_t style_btn_dark_grey;

PromptPanel::PromptPanel(KWebSocketClient &websocket_client, std::mutex &lock, lv_obj_t *parent)
    : NotifyConsumer(lock)
    , ws(websocket_client)
{
  button_group_cont = NULL;  // Pono: was never initialized -> a plain prompt_button (no group) read garbage

  // Geometry lives in prompt_layout.cpp so the headless harness can render the
  // same code (sim/pono_headless.cpp, screen "prompt"). The card sizes to its
  // content and scrolls only when the panel cannot hold it; the fixed 3-row
  // grid this replaced clipped two of the three lines of the calibrate-all
  // prompt on real hardware, losing the hotend temperature entirely.
  pono::prompt_layout_build(lv_scr_act(), &layout);
  prompt_cont = layout.cont;
  flex        = layout.body;
  header      = layout.header;
  footer_cont = layout.footer;

#ifdef DEBUG_LINES
  // for debugging
  lv_obj_set_style_border_width(header, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_color(header, pono::color_state_error, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(flex, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_color(flex, pono::color_state_warning, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(footer_cont, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_color(footer_cont, pono::color_accent_primary, LV_PART_MAIN | LV_STATE_DEFAULT);
#endif

  ws.register_notify_update(this);
  ws.register_method_callback("notify_gcode_response", "MainPanel",[this](json& d) { this->handle_macro_response(d); });

  // create header
  lv_label_set_text(header, "HEADER");

  // button styles
  // Phase A.4 pass 4: button-type dispatch (see line ~318) maps to Pono
  // semantic tokens. "secondary" = surface_elevated, "primary"/"info" =
  // accent_primary, "error" = state_error, "warning" = state_warning, default
  // fallback = text_tertiary (deepest neutral, visually distinct from secondary).
  lv_style_init(&style_btn_grey);
  lv_style_set_bg_color(&style_btn_grey, pono::color_surface_elevated);
  lv_style_set_bg_opa(&style_btn_grey, LV_OPA_COVER);
  lv_style_set_pad_all(&style_btn_grey, 0);

  lv_style_init(&style_btn_blue);
  lv_style_set_bg_color(&style_btn_blue, pono::color_accent_primary);
  lv_style_set_bg_opa(&style_btn_blue, LV_OPA_COVER);

  lv_style_init(&style_btn_red);
  lv_style_set_bg_color(&style_btn_red, pono::color_state_error);
  lv_style_set_bg_opa(&style_btn_red, LV_OPA_COVER);

  lv_style_init(&style_btn_orange);
  lv_style_set_bg_color(&style_btn_orange, pono::color_state_warning);
  lv_style_set_bg_opa(&style_btn_orange, LV_OPA_COVER);

  lv_style_init(&style_btn_dark_grey);
  lv_style_set_bg_color(&style_btn_dark_grey, pono::color_text_tertiary);
  lv_style_set_bg_opa(&style_btn_dark_grey, LV_OPA_COVER);

  background(); // hide ourselves
}

void PromptPanel::consume(json &j) {
}

PromptPanel::~PromptPanel() {
  if (prompt_cont != NULL) {
    lv_obj_del(prompt_cont);
    prompt_cont = NULL;
  }

  ws.unregister_notify_update(this);
}

void PromptPanel::foreground() {
  // shrink wrap
  pono::raise_overlay(prompt_cont);  // prompt_cont is on the screen, so background() hides it behind main_cont; the raise re-asserts the E-STOP
}

void PromptPanel::background() {
  lv_obj_move_background(prompt_cont);
}

// A prompt that was up when the link dropped never receives prompt_end, so
// showing_ would stay true and consume()'s gate (is_showing()) would suppress
// the calibration overlay for the rest of the session. Force the dialog down,
// clear the flag, and drop stale header text. Caller holds lv_lock.
void PromptPanel::reset() {
  background();
  showing_ = false;
  lv_label_set_text(header, "");
}

void PromptPanel::handle_callback(lv_event_t *event) {
  lv_obj_t *btn = lv_event_get_current_target(event);

  lv_obj_t *label = lv_obj_get_child(btn, 0);
  lv_obj_t *command = lv_obj_get_child(btn, 1);
  // check if btn in command map
  LOG_DEBUG("handle event");

  if (btn == NULL) {
    LOG_DEBUG("no button found");
  }

  if (label != NULL) {
    LOG_DEBUG("button: {}", lv_label_get_text(label));
  }

  if (command != NULL) {
    std::string cmd = lv_label_get_text(command);
    LOG_DEBUG("button: {}", cmd);
    ws.gcode_script(cmd);
  }
}

// The card sizes itself to its content; this settles the layout and hands any
// residual overflow to the body as a scroll viewport.
//
// The loop this replaced could not detect the clipping it existed to prevent.
// It measured only the LAST child of the body, and since every line carried
// flex_grow the children always exactly filled the body, so its test was false
// by construction. The overflow was inside each label, which it never looked at.
void PromptPanel::check_height() {
  pono::prompt_layout_fit(&layout);
}

void PromptPanel::handle_macro_response(json &j) {
  LOG_TRACE("macro response: {}", j.dump());
  auto &v = j["/params/0"_json_pointer];

  if (!v.is_null()) {
    LOG_TRACE("data found");
    std::string resp = v.template get<std::string>();
    std::lock_guard<std::mutex> lock(lv_lock);
    LOG_TRACE("data: {}", resp);

    if (resp.find("// action:", 0) == 0) {
      // it is an action
      std::string command = resp.substr(10);
      LOG_DEBUG("action: {}", command);

      if (command.find("prompt_begin") == 0) {
        std::string prompt_header = command.substr(13);
        LOG_DEBUG("PROMPT_BEGIN: {}", prompt_header);

        // drop the previous prompt's content and any growth/scroll it needed
        pono::prompt_layout_reset(&layout);
        button_group_cont = NULL;  // Pono: body children just deleted; drop dangling group ptr (else no-group prompt_button = use-after-free)

        // set header here
        lv_label_set_text(header, prompt_header.c_str());
      } else if (command.find("prompt_text") == 0) {
        std::string prompt_text = command.substr(12);
        LOG_DEBUG("PROMPT_TEXT: {}", prompt_text);
        // One body line, sized to its own wrapped text. The fixed 40px height +
        // flex_grow this replaced gave every line an equal share of the body
        // regardless of how tall its text was, which clipped anything past two
        // wrapped rows.
        lv_obj_t *textfield = pono::prompt_layout_add_text(&layout, prompt_text.c_str());
        (void)textfield;  // only read under DEBUG_LINES
#ifdef DEBUG_LINES
        lv_obj_set_style_border_width(textfield, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(textfield, pono::color_accent_secondary, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(textfield, pono::color_surface_raised, LV_PART_MAIN | LV_STATE_DEFAULT);
#endif
      // due to using find, order IS important!
      } else if (command.find("prompt_button_group_start") == 0) {
        LOG_DEBUG("Button group created");
        // create new button group in the body and mark active
        button_group_cont = pono::prompt_layout_add_button_row(&layout);

#ifdef DEBUG_LINES
        lv_obj_set_style_border_width(button_group_cont, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(button_group_cont, pono::color_state_intel, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(button_group_cont, pono::color_surface_raised, LV_PART_MAIN | LV_STATE_DEFAULT);
#endif
        // lv_obj_set_style_min_height(button_group_cont, lv_pct(5), 0);
      } else if (command.find("prompt_button_group_end") == 0) {
        // does nothing since start creates a new one
        LOG_DEBUG("Button group ended");
        button_group_cont = NULL;
      } else if (command.find("prompt_footer_button") == 0 || command.find("prompt_button") == 0) {
        int index_label = command.find("button", 0) + strlen("button");
        while (index_label < (int)command.size() && command[index_label] == ' ') index_label++;  // skip the space after "button" so the parsed label has no leading space
        int index_first = command.find("|", index_label);
        if (index_first < 0) { LOG_DEBUG("malformed prompt_button, no '|': {}", command); return; }  // no delimiter: skip rather than slice/substr on npos(-1)
        int index_second = command.find("|", index_first + 1);
        LOG_DEBUG("indexes: {} {} {}", index_label, index_first, index_second);
        std::string prompt_footer_button = command.substr(index_label, index_first - index_label);
        std::string prompt_button_command;
        std::string prompt_button_type = "none";
        LOG_DEBUG("button: {} |  {} | {}", prompt_footer_button, prompt_button_command, prompt_button_type);
        if (index_second > 0) {
          prompt_button_command = command.substr(index_first + 1, index_second - index_first - 1);
          prompt_button_type = command.substr(index_second + 1, command.length() - index_second - 1);
        } else {
          prompt_button_command = command.substr(index_first + 1);
        }
        LOG_DEBUG("PROMPT_FOOTER_BUTTON: {} CMD: {}, type {}", prompt_footer_button, prompt_button_command, prompt_button_type);
        lv_obj_t *btn = NULL;
        if (command.find("prompt_footer_button") == 0) {
          btn = lv_btn_create(footer_cont);
        } else {
          if (button_group_cont == NULL) {
            btn = lv_btn_create(flex);
          } else {
            btn = lv_btn_create(button_group_cont);
          }
        }
        if (btn) {
          lv_obj_set_size(btn, lv_pct(45), 32);
          lv_obj_set_style_max_width(btn, lv_pct(45), 0);
          lv_obj_set_style_min_width(btn, 32, 0);
          lv_obj_set_style_max_height(btn, 54, 0);
          lv_obj_set_style_min_height(btn, 42, 0);
          lv_obj_set_style_outline_pad(btn, 0, 0);
          lv_obj_center(btn);
          lv_obj_set_flex_grow(btn, 1);
          lv_obj_t *label = lv_label_create(btn);
          // a hidden label is abused to transfer the command and auto-clean it
          lv_obj_t *command = lv_label_create(btn);
          lv_obj_set_size(command, 1, 1);
          lv_obj_add_flag(command, LV_OBJ_FLAG_HIDDEN);
          lv_label_set_text(label, prompt_footer_button.c_str());
          lv_label_set_text(command, prompt_button_command.c_str());
          lv_obj_set_style_pad_all(btn, 2, 0);
          // lv_obj_set_style_max_width(label, lv_pct(45), 0);
          lv_obj_center(label);

          // Pick BOTH fill and text per type, as LOCAL styles. The theme's
          // button apply_cb sets a LOCAL accent bg on every lv_btn, and in
          // LVGL a local style outranks an *added* one -- so the style_btn_*
          // fills added below were silently ignored and every prompt button
          // rendered in the accent. For secondary/default that left light
          // text on a bright fill: unreadable. Setting bg_color locally here
          // overrides the theme and restores the intended per-type fills; text
          // is paired dark-on-bright / light-on-dark for contrast either way.
          lv_color_t btn_bg  = pono::color_accent_primary;  // the lamp (info/primary)
          lv_color_t btn_txt = pono::color_surface_base;    // dark text on bright fill
          if (!prompt_button_type.compare("secondary")) {
            LOG_DEBUG("type secondary");
            lv_obj_add_style(btn, &style_btn_grey, 0);
            btn_bg  = pono::color_surface_elevated;         // dark neutral surface
            btn_txt = pono::color_text_primary;             // light on dark surface
          } else if (!prompt_button_type.compare("warning")) {
            LOG_DEBUG("type warning");
            lv_obj_add_style(btn, &style_btn_orange, 0);
            btn_bg = pono::color_state_warning;             // amber, dark text
          } else if (!prompt_button_type.compare("error")) {
            LOG_DEBUG("type error");
            lv_obj_add_style(btn, &style_btn_red, 0);
            btn_bg = pono::color_state_error;               // red, dark text
          } else if (!prompt_button_type.compare("info")) {
            LOG_DEBUG("type info");
            lv_obj_add_style(btn, &style_btn_blue, 0);
            btn_bg = pono::color_accent_primary;            // lamp, dark text
          } else if (!prompt_button_type.compare("primary")) {
            LOG_DEBUG("type primary");
            lv_obj_add_style(btn, &style_btn_blue, 0);
            btn_bg = pono::color_accent_primary;            // lamp, dark text
          } else { // unspecified type -> neutral dark fill, light text
            LOG_DEBUG("type default");
            lv_obj_add_style(btn, &style_btn_dark_grey, 0);
            btn_bg  = pono::color_surface_elevated;         // dark neutral surface
            btn_txt = pono::color_text_primary;             // light on dark
          }
          lv_obj_set_style_bg_color(btn, btn_bg, 0);        // LOCAL: overrides theme cyan
          lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
          lv_obj_set_style_text_color(label, btn_txt, 0);
          lv_obj_add_event_cb(btn, _handle_callback, LV_EVENT_PRESSED, this);
        }
      } else if (command.find("prompt_show") == 0) {
        LOG_DEBUG("PROMPT_SHOW");
        check_height();
        foreground();
        showing_ = true;
        pono::busy_hide();  // prompt and the cal/busy overlay both live on lv_layer_top; drop the scrim so the dialog is visible/tappable (consume() also gates on is_showing() to not re-show it)
      } else if (command.find("prompt_end") == 0) {
        LOG_DEBUG("PROMPT_END");
        background();
        showing_ = false;
        lv_label_set_text(header, "");  // drop header so a later prompt_show without prompt_begin can't flash stale text

        // remove buttons + body content, and undo any growth/scroll
        pono::prompt_layout_reset(&layout);
        button_group_cont = NULL;  // body children just deleted; drop dangling group ptr
      } else {
        LOG_DEBUG("action {} --- not supported", command);
      }
    }
  }
}
