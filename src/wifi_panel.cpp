#include "wifi_panel.h"
#include "utils.h"
#include "config.h"
#include "logger.h"
#include "pono_theme.h"  // pono::font_h2 + color tokens for the restyled header

#include <sstream>
#include <iostream>
#include <vector>
#include <utility>
#include <algorithm>

LV_IMG_DECLARE(back);
LV_IMG_DECLARE(refresh_img);

static void draw_part_event_cb(lv_event_t * e) {
  lv_obj_t * obj = lv_event_get_target(e);
  lv_obj_draw_part_dsc_t * dsc = lv_event_get_draw_part_dsc(e);
  if(dsc->part == LV_PART_ITEMS) {
    uint32_t row = dsc->id /  lv_table_get_col_cnt(obj);
    uint32_t col = dsc->id - row * lv_table_get_col_cnt(obj);

    if(col == 1) {
      dsc->label_dsc->align = LV_TEXT_ALIGN_RIGHT;
    }
  }
}

WifiPanel::WifiPanel(std::mutex &l)
  : lv_lock(l)
  , cont(lv_obj_create(lv_scr_act()))
  , spinner(lv_spinner_create(cont, 1000, 60))
  , top_cont(lv_obj_create(cont))
  , wifi_table(lv_table_create(top_cont))
  , wifi_right(lv_obj_create(top_cont))
  , prompt_cont(wifi_right)
  , wifi_label(lv_label_create(prompt_cont))
  , back_btn(cont, &back, "Back", &WifiPanel::_handle_back_btn, this)
  , refresh_btn(cont, &refresh_img, "Refresh", &WifiPanel::_handle_refresh_btn, this)
{
  lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_CLICKABLE);

  lv_obj_add_flag(spinner, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 0);

  lv_obj_add_flag(back_btn.get_container(), LV_OBJ_FLAG_FLOATING);  
  lv_obj_align(back_btn.get_container(), LV_ALIGN_BOTTOM_RIGHT, 0, -20);
  lv_obj_add_flag(refresh_btn.get_container(), LV_OBJ_FLAG_FLOATING);
  lv_obj_align(refresh_btn.get_container(), LV_ALIGN_BOTTOM_RIGHT, -100, -20);
  
  lv_obj_set_flex_grow(top_cont, 1);
  lv_obj_set_flex_flow(top_cont, LV_FLEX_FLOW_ROW);
  lv_obj_clear_flag(top_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(top_cont, 0, 0);
  lv_obj_set_width(top_cont, LV_PCT(100));
  
  lv_obj_set_height(wifi_table, LV_PCT(90));
  lv_obj_add_flag(wifi_table, LV_OBJ_FLAG_HIDDEN);

  auto screen_width = lv_disp_get_physical_hor_res(NULL) / 2 - 100;
  
  lv_table_set_col_width(wifi_table, 0, screen_width);
  lv_table_set_col_width(wifi_table, 1, 100);
  // montserrat carries the LV_SYMBOL glyphs (wifi/lock/ok); the theme's text
  // font does not, so the signal/lock cells were rendering as tofu boxes.
  lv_obj_set_style_text_font(wifi_table, &lv_font_montserrat_14, LV_PART_ITEMS);
  
  lv_obj_add_event_cb(wifi_table, &WifiPanel::_handle_callback, LV_EVENT_VALUE_CHANGED, this);
  lv_obj_add_event_cb(wifi_table, &WifiPanel::_handle_callback, LV_EVENT_SIZE_CHANGED, this);
  lv_obj_add_event_cb(wifi_table, &WifiPanel::_handle_callback, LV_EVENT_LONG_PRESSED, this);
  lv_obj_add_event_cb(wifi_table, draw_part_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

  lv_obj_set_scroll_dir(wifi_table, LV_DIR_TOP | LV_DIR_BOTTOM);

  lv_obj_set_style_border_width(wifi_right, 0, 0);
  lv_obj_set_flex_grow(wifi_right, 1);
  lv_obj_add_flag(wifi_right, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_CLICKABLE);

  lv_obj_add_flag(prompt_cont, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_size(prompt_cont, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(prompt_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(prompt_cont, 0, 0);
  
  lv_obj_align(wifi_label, LV_ALIGN_TOP_LEFT, 0, 10);
  lv_obj_move_background(cont);
  lv_obj_move_foreground(spinner);

  // ---- Pono restyle: the room + a real top header (back + title + refresh)
  // instead of the two floating image buttons hanging bottom-right ----
  lv_obj_set_style_bg_color(cont, pono::color_surface_base, 0);
  lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
  // pad_top 0 on cont keeps the floating header (back/title/refresh) in the
  // y=0..56 top band; the scrolling content is pushed below it by padding the
  // top of top_cont (the table+prompt row) instead. The old pad_top on cont
  // pushed BOTH header and content down, piling them on top of each other.
  // (margin styles are not compiled into this LVGL, so pad is the lever.)
  lv_obj_set_style_pad_top(cont, 0, 0);
  lv_obj_set_style_pad_top(top_cont, 64, 0);  // clear the back/refresh image+label buttons fully
  lv_obj_t *wifi_title = lv_label_create(cont);
  lv_label_set_text(wifi_title, "Wi-Fi");
  lv_obj_set_style_text_font(wifi_title, pono::font_h2, 0);
  lv_obj_set_style_text_color(wifi_title, pono::color_text_primary, 0);
  lv_obj_add_flag(wifi_title, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(wifi_title, LV_ALIGN_TOP_MID, 0, 16);   // centered: Back left, Wi-Fi center, Refresh right
  lv_obj_align(back_btn.get_container(), LV_ALIGN_TOP_LEFT, 8, 8);
  lv_obj_align(refresh_btn.get_container(), LV_ALIGN_TOP_RIGHT, -(pono::frame_width - pono::estop_clear_x) - 4, 8);  // right edge 392, left of the E-STOP's reach

  // The password sheet covers the panel while open, so nothing under it can
  // hide it mid-typing. OK joins; the keyboard key backs out.
  pono::password_sheet_build(cont, &pw);
  lv_obj_add_event_cb(pw.input, &WifiPanel::_handle_kb_input, LV_EVENT_READY, this);
  lv_obj_add_event_cb(pw.input, &WifiPanel::_handle_kb_input, LV_EVENT_CANCEL, this);

  wpa_event.register_callback("WifiPanel",
      [this](const std::string &event) { this->handle_wpa_event(event); });

  wpa_event.start();
}

WifiPanel::~WifiPanel() {
  if (cont != NULL) {
    lv_obj_del(cont);
    cont = NULL;
  }
}

void WifiPanel::foreground() {
  LOG_TRACE("wifi panel fg");
  lv_obj_move_foreground(cont);
  pono::password_sheet_close(&pw);  // no stale sheet over the list
  // fallback: no Wi-Fi adapter -> say so instead of spinning forever
  if (KUtils::get_wifi_interface().empty()) {
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_table, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(wifi_label, "No Wi-Fi adapter detected");
    lv_obj_clear_flag(prompt_cont, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_label_set_text(wifi_label, "Scanning for networks...");
  lv_obj_clear_flag(prompt_cont, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
  wpa_event.send_command("SCAN");
}

void WifiPanel::handle_back_btn(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_CLICKED) {
    LOG_TRACE("wifi panel bg");
    lv_obj_add_flag(wifi_table, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prompt_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(cont);
  }
}

void WifiPanel::handle_refresh_btn(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if(code == LV_EVENT_CLICKED) {
    LOG_INFO("Refreshing");
    foreground();
  }
}

void WifiPanel::remove_network(lv_event_t *e) {
  lv_obj_t * obj = lv_event_get_current_target(e);
  const std::string action = lv_msgbox_get_active_btn_text(obj);
  lv_msgbox_close(obj);

  if (action == "OK") {
    // A rescan on the wpa thread can clear list_networks between the long-press
    // and this confirm; don't dereference end().
    auto it = list_networks.find(selected_network);
    if (it == list_networks.end()) return;
    LOG_INFO("Removing network {}", selected_network);
    wpa_event.send_command(fmt::format("REMOVE_NETWORK {}", it->second));
    wpa_event.send_command("SAVE_CONFIG");
  }
}

void WifiPanel::handle_callback(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_LONG_PRESSED) {
    uint16_t row;
    uint16_t col;
    lv_table_get_selected_cell(wifi_table, &row, &col);
    if (row == LV_TABLE_CELL_NONE || col == LV_TABLE_CELL_NONE) {
      return;
    }
    selected_network = lv_table_get_cell_value(wifi_table, row, 0);
  }

  if (code == LV_EVENT_VALUE_CHANGED) {
    // we need to reload the current network so we have the right state to compare
    if (find_current_network()) {
      LOG_TRACE("handle callback - current network {}", cur_network);
    }

    if (cur_network.length() > 0 && cur_network == selected_network) {
      auto ip = KUtils::interface_ip(KUtils::get_wifi_interface());
      if (ip != "0.0.0.0") {
        lv_label_set_text(wifi_label, fmt::format("Connected to {}\n\nIP: {}",
						selected_network,
						ip).c_str());
      } else {
        lv_label_set_text(wifi_label, fmt::format("Connect to {}\n\nNo IP Address",
            selected_network).c_str());
      }
      pono::password_sheet_close(&pw);
    } else if (list_networks.count(selected_network)) {
      auto nid = list_networks.find(selected_network)->second;
      wpa_event.send_command(fmt::format("SELECT_NETWORK {}", nid));
      wpa_event.send_command("SAVE_CONFIG");
    } else {
      lv_label_set_text(wifi_label, fmt::format("Connect to {}", selected_network).c_str());
      pono::password_sheet_open(&pw, fmt::format("Password for {}", selected_network).c_str());
    }
    lv_obj_clear_flag(prompt_cont, LV_OBJ_FLAG_HIDDEN);
  } else if (code == LV_EVENT_LONG_PRESSED) {
    if (list_networks.count(selected_network)) {
      static const char *btns[] = {"OK", "Cancel"};
      lv_obj_t * mbox = lv_msgbox_create(NULL, "", fmt::format("Delete {}?", selected_network).c_str(), btns, false);
      lv_obj_set_width(mbox, LV_PCT(50));
      lv_obj_align(mbox, LV_ALIGN_TOP_MID, 0, 0);
      lv_obj_add_event_cb(mbox, _remove_network, LV_EVENT_VALUE_CHANGED, this);
      lv_obj_center(mbox);
    }
  }
}

void WifiPanel::handle_wpa_event(const std::string &event) {
  if (event.rfind("<3>CTRL-EVENT-SCAN-RESULTS", 0) == 0) {
    LOG_TRACE("got scan result event");
    std::istringstream f(wpa_event.send_command("SCAN_RESULTS"));
    std::string line;
    wifi_name_db.clear();
    uint32_t index = 0;

    // find_current_network() does blocking wpa socket I/O - run it WITHOUT the
    // render lock. Then take lv_lock only for the UI writes. (send_command is
    // serialized internally, so the socket itself is safe across threads.)
    bool have_current = find_current_network();
    std::lock_guard<std::mutex> lock(lv_lock);

    if (have_current) {
      LOG_TRACE("handle wpa event scan results - current network {}", cur_network);
    } else {
      lv_label_set_text(wifi_label, "");
    }

    while (std::getline(f, line)) {
      if (line.rfind("bss", 0) == 0) {
	      continue;
      }

      auto wifi_parts = KUtils::split(line, '\t');
      LOG_TRACE("wifi parts {}", join(wifi_parts, ", "));
      if (wifi_parts.size() == 5) {
        int signal = 0;
        try { signal = std::stoi(wifi_parts[2]); } catch (...) { continue; }  // skip malformed scan lines
        auto inserted = wifi_name_db.insert({wifi_parts[4], signal});
        if (inserted.second) {
          lv_table_set_cell_value(wifi_table, index, 0, wifi_parts[4].c_str());
          if (cur_network != wifi_parts[4]) {
            lv_table_set_cell_value(wifi_table, index, 1, LV_SYMBOL_WIFI);
          } else if (cur_network.length() > 0) {
            auto ip = KUtils::interface_ip(KUtils::get_wifi_interface());
            if (ip != "0.0.0.0") {
              lv_table_set_cell_value(wifi_table, index, 1, LV_SYMBOL_OK "    " LV_SYMBOL_WIFI);
              lv_label_set_text(wifi_label, fmt::format("Connected to {}\n\nIP: {}",
                          cur_network,
                          ip).c_str());
            } else {
              lv_label_set_text(wifi_label, fmt::format("Connect to {}\n\nNo IP Address", cur_network).c_str());
              lv_table_set_cell_value(wifi_table, index, 1, LV_SYMBOL_WARNING "    " LV_SYMBOL_WIFI);
            }
            lv_obj_clear_flag(prompt_cont, LV_OBJ_FLAG_HIDDEN);
          }
          index++;
        }
      }
    } // while
    // Trim any stale rows to the exact count (print_panel pattern). Doing this
    // at the END avoids the set_row_cnt(0)-then-regrow free/realloc churn every
    // scan, which segfaulted after a few rescans.
    if (index > 0) lv_table_set_row_cnt(wifi_table, index);
    lv_obj_scroll_to_y(wifi_table, 0, LV_ANIM_OFF);
    if (index == 0) {  // fallback: scan returned nothing
      lv_obj_add_flag(wifi_table, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(wifi_label, "No networks found - tap Refresh");
      lv_obj_clear_flag(prompt_cont, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(wifi_table, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
  } else if (event.rfind("<3>CTRL-EVENT-CONNECTED", 0) == 0) {
    if (find_current_network()) {
      LOG_TRACE("handle wpa event connected - current network {}", cur_network);
      std::vector<std::pair<std::string, int>> pairs;
      for (auto it = wifi_name_db.begin(); it != wifi_name_db.end(); ++it) {
	      pairs.push_back(*it);
      }
      
      std::sort(pairs.begin(), pairs.end(), [=](std::pair<std::string, int>& a,
						std::pair<std::string, int>& b) {
	      return a.second > b.second;
      });
      
      std::lock_guard<std::mutex> lock(lv_lock);

      uint32_t index = 0;
      for (const auto &wifi : pairs) {
        lv_table_set_cell_value(wifi_table, index, 0, wifi.first.c_str());
        if (cur_network != wifi.first) {
          lv_table_set_cell_value(wifi_table, index, 1, LV_SYMBOL_WIFI);
        } else if (cur_network.length() > 0) {
          auto ip = KUtils::interface_ip(KUtils::get_wifi_interface());
          if (ip != "0.0.0.0") {
            lv_table_set_cell_value(wifi_table, index, 1, LV_SYMBOL_OK "    " LV_SYMBOL_WIFI);
            lv_label_set_text(wifi_label, fmt::format("Connected to {}\n\nIP: {}",
                      cur_network,
                      ip).c_str());
          } else {
            lv_label_set_text(wifi_label, fmt::format("Connect to {}\n\nNo IP Address",
                      cur_network).c_str());
            lv_table_set_cell_value(wifi_table, index, 1, LV_SYMBOL_WARNING "    " LV_SYMBOL_WIFI);
          }
          lv_obj_clear_flag(prompt_cont, LV_OBJ_FLAG_HIDDEN);
        }
        index++;
      }

      if (index > 0) lv_table_set_row_cnt(wifi_table, index);  // trim stale rows; no per-scan free churn
      lv_obj_scroll_to_y(wifi_table, 0, LV_ANIM_OFF);
      lv_obj_clear_flag(wifi_table, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
      std::lock_guard<std::mutex> lock(lv_lock);  // wpa thread: lock like every sibling branch before touching the label
      lv_label_set_text(wifi_label, "");
    }
  } else if (event.find("CTRL-EVENT-SSID-TEMP-DISABLED") != std::string::npos ||
             event.find("4WAY-HANDSHAKE-FAILED") != std::string::npos ||
             event.find("reason=WRONG_KEY") != std::string::npos) {
    // fallback: auth/handshake failure -> say so (usually a wrong password)
    std::lock_guard<std::mutex> lock(lv_lock);
    lv_label_set_text(wifi_label, "Connection failed - check the password");
    lv_obj_clear_flag(prompt_cont, LV_OBJ_FLAG_HIDDEN);
    // reopen the sheet (closed on submit) for a retry; if it is still open the typed text stays
    if (!selected_network.empty())
      pono::password_sheet_open(&pw, fmt::format("Wrong password for {}? Try again", selected_network).c_str());
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
  }
}

void WifiPanel::handle_kb_input(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    const char *password = lv_textarea_get_text(pw.input);
    if (password == NULL || password[0] == 0) {
      return;
    }

    // connect reads the field's text, so it must run before close clears the field
    connect(password);
    pono::password_sheet_close(&pw);
    lv_label_set_text(wifi_label, fmt::format("Connecting to {} ...", selected_network).c_str());
  } else if (code == LV_EVENT_CANCEL) {
    pono::password_sheet_close(&pw);
    lv_label_set_text(wifi_label, "Please select your wifi network");
  }
}

void WifiPanel::connect(const char *password) {
  std::string nid = wpa_event.send_command("ADD_NETWORK");
  LOG_TRACE("add_nework {}", nid);
  if (nid.length() > 0) {
    wpa_event.send_command(fmt::format("SET_NETWORK {} ssid {:?}", nid, selected_network));
    wpa_event.send_command(fmt::format("SET_NETWORK {} psk {:?}", nid, password));
    wpa_event.send_command(fmt::format("ENABLE_NETWORK {}", nid));
    wpa_event.send_command(fmt::format("SELECT_NETWORK {}", nid));
    wpa_event.send_command("SAVE_CONFIG");
  }
}

bool WifiPanel::find_current_network() {
  list_networks.clear();
  std::string nets = wpa_event.send_command("LIST_NETWORKS");
  LOG_TRACE("nets = {}", nets);
  std::istringstream f(nets);
  std::string line;
  cur_network = ""; // reset it to nothing in case we deleted the network
  bool found = false;
  while (std::getline(f, line)) {
    auto wifi_parts = KUtils::split(line, '\t');
    if (wifi_parts.size() == 4 && line.find("[CURRENT]") != std::string::npos) {
      cur_network = wifi_parts[1];
      list_networks.insert({wifi_parts[1], wifi_parts[0]});
      found = true;
    }
    if (wifi_parts.size() > 1) {
      list_networks.insert({wifi_parts[1], wifi_parts[0]});
    }
  }
  return found;
}
