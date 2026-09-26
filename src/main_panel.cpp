#include "main_panel.h"
#include "state.h"
#include "lvgl/lvgl.h"
#include "logger.h"
#include "pono_theme.h"  // Phase A.4: surface + accent tokens for tab UI
#include "pono_anim.h"   // busy_show/busy_hide working overlay for blocking waits
#include "utils.h"       // KUtils::interface_ip for the System screen
#include <fstream>       // /etc/pono-version, /proc/uptime for the System screen

#include <string>
#include <cstdint>
#include <cstdio>        // popen: version check against the firmware host
#include <cstdlib>       // system: run update-pono-print from the Install chip
#include <thread>        // network work off the LVGL thread (labels updated under lv_lock)

LV_IMG_DECLARE(filament_img);
LV_IMG_DECLARE(light_img);
LV_IMG_DECLARE(move);
LV_IMG_DECLARE(extruder);
LV_IMG_DECLARE(bed);
LV_IMG_DECLARE(fan);
LV_IMG_DECLARE(heater);
LV_IMG_DECLARE(emergency);

LV_FONT_DECLARE(materialdesign_font_40);

#define INFO_SYMBOL    u8"\U000F02FD"
#define SETTING_SYMBOL u8"\U000F1064"
#define HOME_SYMBOL    u8"\U000F02DC"
#define CONSOLE_SYMBOL u8"\U000F018D"

MainPanel::MainPanel(KWebSocketClient &websocket,
		     std::mutex &lock,
		     SpoolmanPanel &sm)
  : NotifyConsumer(lock)
  , ws(websocket)
  , homing_panel(ws, lock)
  , fan_panel(ws, lock)
  , led_panel(ws, lock)    
  , tabview(lv_tabview_create(lv_scr_act(), LV_DIR_LEFT, 60))
  , main_tab(lv_tabview_add_tab(tabview, HOME_SYMBOL))
  , console_tab(lv_tabview_add_tab(tabview, CONSOLE_SYMBOL))
  , console_panel(ws, lock, console_tab)
  , setting_tab(lv_tabview_add_tab(tabview, SETTING_SYMBOL))
  , setting_panel(websocket, lock, setting_tab)
  , sysinfo_tab(lv_tabview_add_tab(tabview, INFO_SYMBOL))
  , sysinfo_panel(sysinfo_tab)
  , main_cont(lv_obj_create(main_tab))
  , numpad(Numpad(lv_layer_top()))  // top layer: keypad overlays every screen incl. Pono sub-screens
  , extruder_panel(ws, lock, numpad, sm)
  , prompt_panel(websocket, lock, main_cont)
  , spoolman_panel(sm)
  , temp_cont(lv_obj_create(main_cont))
  , temp_chart(lv_chart_create(main_cont))
  , homing_btn(main_cont, &move, "Homing", &MainPanel::_handle_homing_cb, this)
  , extrude_btn(main_cont, &filament_img, "Extrude", &MainPanel::_handle_extrude_cb, this)
  , action_btn(main_cont, &fan, "Fans", &MainPanel::_handle_fanpanel_cb, this)
  , led_btn(main_cont, &light_img, "LED", &MainPanel::_handle_ledpanel_cb, this)
  // "E-STOP", matching the persistent top-right button and the move screen.
  // This sends printer.emergency_stop, a full halt, not the calibration STOP
  // that only lands at a step boundary.
  , emergency_btn(main_cont, &emergency, "E-STOP", &MainPanel::_handle_emergency_cb, this,
  		  "Do you want to emergency stop?",
  		  [&websocket]() {
  		    LOG_DEBUG("emergency stop pressed");
  		    websocket.send_jsonrpc("printer.emergency_stop");
  		  })
{
    lv_style_init(&style);
    lv_style_set_img_recolor_opa(&style, LV_OPA_30);
    lv_style_set_img_recolor(&style, lv_color_black());
    lv_style_set_border_width(&style, 0);
    lv_style_set_bg_color(&style, pono::color_surface_base);

    ws.register_notify_update(this);

    lv_obj_add_event_cb(tabview, &MainPanel::_tabview_event_cb,
                            LV_EVENT_VALUE_CHANGED, this);

    // The stale watchdog (UI trust audit T6): an open websocket whose
    // notifies stopped (wedged Moonraker) used to leave temps, progress
    // and position rendering as live forever. A notify-driven check can
    // never fire when notifies stop, so a timer owns the question.
    stale_timer_ = lv_timer_create([](lv_timer_t *t) {
      static_cast<MainPanel *>(t->user_data)->check_stale();
    }, 5000, this);
}

MainPanel::~MainPanel() {
  ws.unregister_notify_update(this);  // drop the consumer before teardown (was a dangling-pointer UAF)
  if (stale_timer_ != nullptr) {
    lv_timer_del(stale_timer_);
    stale_timer_ = nullptr;
  }
  if (estop_keepalive_timer_ != nullptr) {
    lv_timer_del(estop_keepalive_timer_);
    estop_keepalive_timer_ = nullptr;
  }
  if (tabview != NULL) {
    lv_obj_del(tabview);
    tabview = NULL;
  }
  if (home_scr != nullptr) {       // Pono: lv_obj_del recurses children + deletes their anims
    lv_obj_del(home_scr);
    home_scr = nullptr;
  }

  sensors.clear();
}

void MainPanel::subscribe() {
  // The native Files screen populates on open (populate_files); there is no
  // standing file-list subscription to (re)establish. The legacy print_panel
  // file-list subscribe - and its lv_lock flag-race - retired with the panel.
}

void MainPanel::init(json &j) {
  std::lock_guard<std::mutex> lock(lv_lock);
  for (const auto &el : sensors) {
    auto target_value = j[json::json_pointer(fmt::format("/result/status/{}/target", el.first))];
    if (!target_value.is_null()) {
      int target = target_value.template get<int>();
      el.second->update_target(target);
    }

    auto temp_value = j[json::json_pointer(fmt::format("/result/status/{}/temperature", el.first))];
    if (!temp_value.is_null()) {
      int value = temp_value.template get<int>();
      el.second->update_series(value);
      el.second->update_value(value);
    }
  }
  { auto bm = j[json::json_pointer("/result/status/bed_mesh")]; if (!bm.is_null()) render_bed_mesh(bm); }  // initial heatmap
  read_tune(j, "/result/status");  // Expert Tune pills start from the machine, not the build defaults

  // Seed the Pono cockpit + Move screen from the initial full state. consume()
  // reads these same fields from /params/0 deltas; the subscribe reply nests them
  // under /result/status. Moonraker only sends a field in a delta when it CHANGES,
  // so without this a connect to an already-printing (or already-homed) machine
  // shows the idle layout, 0% progress, and "--" position until state next moves.
  // Gate on state_pill: it exists in BOTH layouts. The arc does not (the idle
  // hero is the dictionary entry), so an arc gate would dead-end the whole
  // live pipeline on any idle boot.
  if (home_h.state_pill) {
    auto V = [&](const char *p) { return j[json::json_pointer(p)]; };
    { auto v = V("/result/status/virtual_sdcard/progress");        if (!v.is_null()) home_progress_    = v.template get<double>(); }
    { auto v = V("/result/status/print_stats/print_duration");     if (!v.is_null()) home_duration_    = v.template get<double>(); }
    { auto v = V("/result/status/extruder/temperature");           if (!v.is_null()) home_nozzle_      = (int)v.template get<double>(); }
    { auto v = V("/result/status/extruder/target");                if (!v.is_null()) home_nozzle_set_  = (int)v.template get<double>(); }
    { auto v = V("/result/status/heater_bed/temperature");         if (!v.is_null()) home_bed_         = (int)v.template get<double>(); }
    { auto v = V("/result/status/heater_bed/target");              if (!v.is_null()) home_bed_set_     = (int)v.template get<double>(); }
    { auto v = V("/result/status/print_stats/info/current_layer"); if (!v.is_null()) home_layer_       = v.template get<int>(); }
    { auto v = V("/result/status/print_stats/info/total_layer");   if (!v.is_null()) home_layer_total_ = v.template get<int>(); }
    { auto v = V("/result/status/print_stats/filename");           if (!v.is_null()) home_job_         = v.template get<std::string>(); }
    { auto v = V("/result/status/toolhead/homed_axes");            if (!v.is_null()) move_homed_       = v.template get<std::string>(); }
    { auto v = V("/result/status/toolhead/position");
      if (v.is_array() && v.size() >= 3) { move_pos_[0]=v[0].template get<double>(); move_pos_[1]=v[1].template get<double>(); move_pos_[2]=v[2].template get<double>(); } }
    { auto v = V("/result/status/print_stats/state");
      if (!v.is_null()) { std::string s = v.template get<std::string>(); home_printing_ = (s == "printing"); home_paused_ = (s == "paused"); } }
    rebuild_home();   // reflect actual state (layout + seeded temps/progress/job) immediately

    if (move_h_.pos) {  // same seed for the Move screen position (else "--" until first jog)
      auto axis = [&](char up, char lo, double v) {
        return move_homed_.find(lo) != std::string::npos
          ? fmt::format("{} {:.1f}", up, v) : fmt::format("{} --", up);
      };
      lv_label_set_text(move_h_.pos, fmt::format("{}  {}  {}",
        axis('X','x',move_pos_[0]), axis('Y','y',move_pos_[1]), axis('Z','z',move_pos_[2])).c_str());
      lv_obj_center(move_h_.pos);
    }
  }
}

void MainPanel::check_stale() {
  // Runs inside lv_timer_handler, which the main loop already wraps in
  // lv_lock (guppyscreen.cpp): taking the same mutex here would deadlock,
  // so this callback must stay lock-free and LVGL-only.
  constexpr int64_t STALE_AFTER_MS = 10000;  // status deltas flow ~1Hz mid-print; 10s of silence on an open link is a wedge
  const bool live = home_h.state_pill && (home_printing_ || home_paused_ || busy_);
  const int64_t age = live ? ws.ms_since_status_update() : -1;
  if (live && age >= STALE_AFTER_MS) {
    pono::home_set_stale(&home_h, (int)(age / 1000));
    stale_shown_ = true;
  } else if (stale_shown_) {
    pono::home_set_stale(&home_h, -1);
    stale_shown_ = false;
  }
  // Make Pono lost-contact: during a run, the silence that wedges the dashboard
  // means the machine could be executing driverless (the 2026-06-11 freeze).
  // Flip the logbook to the alarm so the operator knows to hit STOP instead of a
  // frozen-cheerful banner; restore the live line the moment contact resumes.
  if (callog_shown_) {
    const int64_t cage = ws.ms_since_status_update();
    if (cage >= STALE_AFTER_MS) {
      std::string m = "no word from the machine for " + std::to_string((int)(cage / 1000)) + "s";
      pono::cal_log_show(m.c_str(), "tap STOP if it does not clear",
                         callog_done_, callog_total_, true, &MainPanel::_callog_stop, this);
      callog_fault_ = true;
    } else if (callog_fault_) {
      pono::cal_log_show(callog_now_.c_str(), callog_next_.empty() ? nullptr : callog_next_.c_str(),
                         callog_done_, callog_total_, false, &MainPanel::_callog_stop, this);
      callog_fault_ = false;
    }
  }
}

// Parse the cal step text "Make Pono X/N: now | next" (or "Full Cal X/N: now")
// into the logbook fields. done/total from the X/N; now = after ':' up to '|';
// next = after '|' (optional). Robust to a bare prefix with no count or steps.
static void parse_cal_msg(const std::string &m, std::string &now,
                          std::string &next, int &done, int &total) {
  done = total = 0; now.clear(); next.clear();
  size_t slash = m.find('/');
  if (slash != std::string::npos && slash > 0) {
    size_t a = slash;     while (a > 0 && m[a-1] >= '0' && m[a-1] <= '9') a--;
    size_t e = slash + 1; while (e < m.size() && m[e] >= '0' && m[e] <= '9') e++;
    if (a < slash && e > slash + 1) {
      try { done = std::stoi(m.substr(a, slash - a));
            total = std::stoi(m.substr(slash + 1, e - slash - 1)); } catch (...) {}
    }
  }
  size_t colon = m.find(':');
  if (colon == std::string::npos) return;
  std::string body = m.substr(colon + 1);
  auto trim = [](const std::string &s) -> std::string {
    size_t i = s.find_first_not_of(" \t");
    if (i == std::string::npos) return std::string();
    size_t jj = s.find_last_not_of(" \t");
    return s.substr(i, jj - i + 1);
  };
  size_t bar = body.find('|');
  if (bar != std::string::npos) { now = trim(body.substr(0, bar)); next = trim(body.substr(bar + 1)); }
  else now = trim(body);
}

void MainPanel::consume(json &j) {
  std::lock_guard<std::mutex> lock(lv_lock);
  if (stale_shown_) {
    // A notify just landed: the readout is moving again. Clear the flag
    // here rather than waiting out the watchdog period.
    pono::home_set_stale(&home_h, -1);
    stale_shown_ = false;
  }
  for (const auto &el : sensors) {
    auto target_value = j[json::json_pointer(fmt::format("/params/0/{}/target", el.first))];
    if (!target_value.is_null()) {
      int target = target_value.template get<int>();
      el.second->update_target(target);
    }

    auto temp_value = j[json::json_pointer(fmt::format("/params/0/{}/temperature", el.first))];
    if (!temp_value.is_null()) {
      int value = temp_value.template get<int>();
      el.second->update_series(value);
      el.second->update_value(value);
    }
  }

  read_tune(j, "/params/0");

  json pstat_state = j.value("/params/0/print_stats/state"_json_pointer, json());  // value(): read without INSERTing a null node on the hot path (non-const operator[] mutates the delta every time this key is absent, i.e. most deltas)
  if (!pstat_state.is_null()) {
    if (pstat_state.template get<std::string>() != "printing") {
      homing_btn.enable();
      extrude_btn.enable();
    } else {
      homing_btn.disable();
      extrude_btn.disable();
    }
  }

  // Legacy tabview LED button (sits behind the cockpit). Was re-set every ws
  // tick; skip the imgbtn redraw unless the icon actually changed.
  { const void *li = led_panel.get_main_button_image();
    if (li != rend_led_img_) { led_btn.set_image(li); rend_led_img_ = li; } }

  // --- Pono cockpit: cache live values, rebuild on the idle<->printing flip
  // (build_home is sim-verified for both states), update in place otherwise.
  // Gated on state_pill, present in both layouts (the arc is printing-only). ---
  if (home_h.state_pill) {
    auto V = [&](const char *p) { return j[json::json_pointer(p)]; };
    { auto v = V("/params/0/virtual_sdcard/progress");        if (!v.is_null()) home_progress_    = v.template get<double>(); }
    { auto v = V("/params/0/print_stats/print_duration");     if (!v.is_null()) home_duration_    = v.template get<double>(); }
    { auto v = V("/params/0/extruder/temperature");           if (!v.is_null()) home_nozzle_      = (int)v.template get<double>(); }
    { auto v = V("/params/0/extruder/target");                if (!v.is_null()) home_nozzle_set_  = (int)v.template get<double>(); }
    { auto v = V("/params/0/heater_bed/temperature");         if (!v.is_null()) home_bed_         = (int)v.template get<double>(); }
    { auto v = V("/params/0/heater_bed/target");              if (!v.is_null()) home_bed_set_     = (int)v.template get<double>(); }
    { auto v = V("/params/0/print_stats/info/current_layer"); if (!v.is_null()) home_layer_       = v.template get<int>(); }
    { auto v = V("/params/0/print_stats/info/total_layer");   if (!v.is_null()) home_layer_total_ = v.template get<int>(); }
    { auto v = V("/params/0/print_stats/filename");           if (!v.is_null()) home_job_         = v.template get<std::string>(); }
    { auto v = V("/params/0/idle_timeout/state");
      if (!v.is_null()) {
        bool nb = (v.template get<std::string>() == "Printing");
        if (busy_ && !nb) cal_msg_.clear();  // leaving an active op: drop stale cal step text so the overlay can't re-show on a later manual home
        busy_ = nb;  // print OR cal
      } }

    // Move screen: live toolhead position, each axis gated on homed_axes. Both
    // arrive as Moonraker deltas (only on change), so cache them and reformat
    // whenever either lands. Unhomed axes read "--" like the build_move default.
    {
      auto ha = V("/params/0/toolhead/homed_axes");
      if (!ha.is_null()) move_homed_ = ha.template get<std::string>();
      auto pp = V("/params/0/toolhead/position");
      if (pp.is_array() && pp.size() >= 3) {
        move_pos_[0] = pp[0].template get<double>();
        move_pos_[1] = pp[1].template get<double>();
        move_pos_[2] = pp[2].template get<double>();
      }
      if (move_h_.pos && (!ha.is_null() || pp.is_array())) {
        auto axis = [&](char up, char lo, double v) {
          return move_homed_.find(lo) != std::string::npos
            ? fmt::format("{} {:.1f}", up, v) : fmt::format("{} --", up);
        };
        lv_label_set_text(move_h_.pos,
          fmt::format("{}  {}  {}", axis('X', 'x', move_pos_[0]),
                      axis('Y', 'y', move_pos_[1]), axis('Z', 'z', move_pos_[2])).c_str());
        lv_obj_center(move_h_.pos);  // re-center as the text width changes
      }
    }

    std::string pst = pstat_state.is_null() ? std::string() : pstat_state.template get<std::string>();
    bool printing = pstat_state.is_null() ? home_printing_ : (pst == "printing");
    bool paused   = pstat_state.is_null() ? home_paused_   : (pst == "paused");

    if (printing != home_printing_ || paused != home_paused_) {
      home_printing_ = printing;
      home_paused_   = paused;
      rebuild_home();                 // swap to the matching layout (Ready / printing / paused)
      // D: a print (or pause) starting must not leave a flash+reboot chip live.
      // Hide it on the flip; it reappears when System is next opened idle.
      if ((home_printing_ || home_paused_) && system_h_.btn_install)
        lv_obj_add_flag(system_h_.btn_install, LV_OBJ_FLAG_HIDDEN);
    } else {
      // temps update in both states: big number + heat color, target "/ N" or "off"
      // Repaint temps only when a value actually changed (see render shadows):
      // consume() runs every ws tick, so the unconditional set_text + re-align
      // here was thrashing redraws on the main screen with identical values.
      if (home_nozzle_ != rend_nozzle_ || home_nozzle_set_ != rend_nozzle_set_ ||
          home_bed_ != rend_bed_ || home_bed_set_ != rend_bed_set_) {
        if (home_h.nozzle) {
          lv_label_set_text(home_h.nozzle, fmt::format("{}", home_nozzle_).c_str());
          // phosphor cool, lamp while heat is at work, alarm only past the
          // hotend's 280C rating (mirrors build_home's temp_card semantics)
          lv_obj_set_style_text_color(home_h.nozzle,
            home_nozzle_ >= 280 ? pono::color_state_error :
            home_nozzle_ >= 45  ? pono::color_accent_primary : pono::color_accent_secondary, 0);
        }
        if (home_h.nozzle_set) {
          lv_label_set_text(home_h.nozzle_set,
            home_nozzle_set_ > 0 ? fmt::format("/ {}", home_nozzle_set_).c_str() : "off");
          lv_obj_align(home_h.nozzle_set, LV_ALIGN_RIGHT_MID, -14, 0);
        }
        if (home_h.bed) {
          lv_label_set_text(home_h.bed, fmt::format("{}", home_bed_).c_str());
          lv_obj_set_style_text_color(home_h.bed,
            home_bed_ >= 100 ? pono::color_state_error :
            home_bed_ >= 45  ? pono::color_accent_primary : pono::color_accent_secondary, 0);
        }
        if (home_h.bed_set) {
          lv_label_set_text(home_h.bed_set,
            home_bed_set_ > 0 ? fmt::format("/ {}", home_bed_set_).c_str() : "off");
          lv_obj_align(home_h.bed_set, LV_ALIGN_RIGHT_MID, -14, 0);
        }
        // mirror live temps onto the Temperature sub-screen (current + target);
        // re-align after set_text so the centered values stay centered as they grow
        auto set_temp_lbl = [](lv_obj_t *o, const std::string &txt, lv_coord_t dy) {
          if (!o) return;
          lv_label_set_text(o, txt.c_str());
          lv_obj_align(o, LV_ALIGN_TOP_MID, 0, dy);
        };
        set_temp_lbl(temp_h_.nz_cur, fmt::format("{}", home_nozzle_), 28);
        set_temp_lbl(temp_h_.nz_tgt, home_nozzle_set_ > 0 ? fmt::format("set {}", home_nozzle_set_) : std::string("off"), 76);
        set_temp_lbl(temp_h_.bd_cur, fmt::format("{}", home_bed_), 28);
        set_temp_lbl(temp_h_.bd_tgt, home_bed_set_ > 0 ? fmt::format("set {}", home_bed_set_) : std::string("off"), 76);
        // mirror nozzle onto the Filament screen banner (was a static "-- / --")
        if (fil_h_.temp) {
          lv_label_set_text(fil_h_.temp, fmt::format("{} / {}", home_nozzle_,
            home_nozzle_set_ > 0 ? std::to_string(home_nozzle_set_) : std::string("off")).c_str());
          lv_obj_align(fil_h_.temp, LV_ALIGN_RIGHT_MID, -14, 0);
        }
        rend_nozzle_ = home_nozzle_; rend_nozzle_set_ = home_nozzle_set_;
        rend_bed_ = home_bed_; rend_bed_set_ = home_bed_set_;
      }
      // live fan speeds onto the Fans screen (5 fans; auto fans update the % only)
      {
        static const char *fp[5] = {
          "/params/0/fan/speed",
          "/params/0/fan_generic model_helper_fan/speed",
          "/params/0/fan_generic box_fan/speed",
          "/params/0/temperature_fan mainboard/speed",
          "/params/0/heater_fan extruder/speed",
        };
        for (int i = 0; i < 5; i++) {
          auto fv = V(fp[i]);
          if (fv.is_null()) continue;
          int fpct = (int)(fv.template get<double>() * 100.0 + 0.5);
          if (fpct == rend_fan_[i]) continue;   // unchanged -> no repaint this tick
          rend_fan_[i] = fpct;
          if (fan_h_.val[i])    lv_label_set_text(fan_h_.val[i], fmt::format("{}%", fpct).c_str());
          // don't fight a finger mid-drag: skip the programmatic set while the slider is held
          if (fan_h_.slider[i] && !lv_obj_has_state(fan_h_.slider[i], LV_STATE_PRESSED))
            lv_slider_set_value(fan_h_.slider[i], fpct, LV_ANIM_OFF);
        }
      }
      { auto bm = V("/params/0/bed_mesh"); if (!bm.is_null()) render_bed_mesh(bm); }  // heatmap on mesh change
      // Keep the panel awake during ANY active operation. print_stats.state is
      // "printing" only for real jobs; idle_timeout.state ("Printing") covers any
      // gcode incl. PID/mesh/shaper cal -- gating only on the former blanked the
      // screen mid-calibration (display_sleep_sec=600). Root cause of the cal blank.
      if (printing || busy_) lv_disp_trig_activity(NULL);
      if (printing) {  // progress + layer + ETA only while a job runs
        int pct = (int)(home_progress_ * 100.0 + 0.5);
        if (home_h.arc) lv_arc_set_value(home_h.arc, pct);  // idle layout has no arc (the entry lives there)
        if (home_h.pct) {
          lv_label_set_text(home_h.pct, fmt::format("{}%", pct).c_str());
          lv_obj_align_to(home_h.pct, home_h.arc, LV_ALIGN_CENTER, 0, 0);
        }
        if (home_h.layer) {
          lv_label_set_text(home_h.layer, fmt::format("layer {} / {}", home_layer_, home_layer_total_).c_str());
          lv_obj_align_to(home_h.layer, home_h.arc, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
        }
        if (home_h.eta && home_progress_ > 0.01 && home_duration_ > 1.0) {
          double remain = home_duration_ * (1.0 - home_progress_) / home_progress_;
          if (remain < 0.0) remain = 0.0;
          int mins = (int)(remain / 60.0 + 0.5);
          home_eta_ = mins >= 60 ? fmt::format("{}:{:02d} left", mins / 60, mins % 60)
                                 : fmt::format("{} min left", mins);
          lv_label_set_text(home_h.eta, home_eta_.c_str());
          if (home_h.layer) lv_obj_align_to(home_h.eta, home_h.layer, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
        }
      }
    }

    // --- Calibration progress overlay -------------------------------------
    // A cal (PID/mesh/shaper) runs as idle_timeout="Printing" while print_stats
    // stays "standby", so the cockpit shows its idle "Ready" layout and tuning
    // looks dead ("no UI showing progress, just the dashboard"). The cal macros
    // announce each step via SET_DISPLAY_TEXT (display_status.message); surface
    // it on the existing busy overlay so the operator sees progress. Gate on the
    // "Calibrating" prefix so a manual home/jog (also idle=Printing, not a real
    // print) never trips it.
    {
      auto dv = V("/params/0/display_status/message");
      if (!dv.is_null()) cal_msg_ = dv.template get<std::string>();
    }
    bool cal_active = busy_ && !printing && cal_msg_.rfind("Calibrating", 0) == 0 && !prompt_panel.is_showing();
    if (cal_active) {
      if (!cal_overlay_ || cal_msg_ != cal_overlay_text_) {
        pono::busy_show(cal_msg_.c_str());
        cal_overlay_ = true;
        cal_overlay_text_ = cal_msg_;
      }
    } else if (cal_overlay_) {
      pono::busy_hide();
      cal_overlay_ = false;
      cal_overlay_text_.clear();
    }

    // --- Make Pono narration ----------------------------------------------
    // The campaign (machine cals then test prints) announces each step via
    // SET_DISPLAY_TEXT as "Make Pono X/N: now | next" ("Full Cal X/N" from the
    // older macro is accepted too). Carry it on the honest logbook box through
    // the whole run, in either state, with an always-reachable STOP. Replaces
    // the thin OMEGA top banner.
    bool cal_run = (busy_ || printing) &&
                   (cal_msg_.rfind("Make Pono", 0) == 0 || cal_msg_.rfind("Full Cal", 0) == 0);
    if (cal_run) {
      if (!callog_shown_ || cal_msg_ != callog_text_) {
        parse_cal_msg(cal_msg_, callog_now_, callog_next_, callog_done_, callog_total_);
        pono::cal_log_show(callog_now_.c_str(), callog_next_.empty() ? nullptr : callog_next_.c_str(),
                           callog_done_, callog_total_, false, &MainPanel::_callog_stop, this);
        callog_shown_ = true;
        callog_text_ = cal_msg_;
        callog_fault_ = false;
      }
    } else if (callog_shown_) {
      pono::cal_log_hide();
      callog_shown_ = false;
      callog_text_.clear();
      callog_fault_ = false;
    }
  }
}

static void scroll_begin_event(lv_event_t * e) {
  /*Silky tab transitions: animate the tab-switch slide. It was forced to 0
   *(instant) when the panel ran at 33 fps; at the 60 fps refresh the slide
   *is smooth. Triggered when a tab button is clicked. */
  if (lv_event_get_code(e) == LV_EVENT_SCROLL_BEGIN) {
    lv_anim_t * a = (lv_anim_t*)lv_event_get_param(e);
    if(a)  a->time = 130;  // Pono: snappier tab switch; 260ms full-screen scroll felt sluggish on this sw-rendered SoC
  }
}

// this is just to ensure we refresh the IP addresses
void MainPanel::_tabview_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    auto *self = static_cast<MainPanel*>(lv_event_get_user_data(e));
    lv_obj_t *tv = lv_event_get_target(e);

    const uint16_t idx = lv_tabview_get_tab_act(tv);

    const uint16_t sysinfo_idx = lv_obj_get_index(self->sysinfo_tab);
    if (idx == sysinfo_idx) {
        self->sysinfo_panel.foreground();
    }
}

void MainPanel::create_panel() {
  lv_obj_clear_flag(lv_tabview_get_content(tabview), LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(lv_tabview_get_content(tabview), scroll_begin_event, LV_EVENT_SCROLL_BEGIN, NULL);
  
  lv_obj_t * tab_btns = lv_tabview_get_tab_btns(tabview);
  lv_obj_set_style_bg_color(tab_btns, pono::color_accent_primary, LV_STATE_CHECKED | LV_PART_ITEMS);  // active tab highlight
  lv_obj_set_style_outline_width(tab_btns, 0, LV_PART_ITEMS | LV_STATE_FOCUS_KEY | LV_STATE_FOCUS_KEY);
  lv_obj_set_style_border_side(tab_btns, 0, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_font(tab_btns, &materialdesign_font_40, LV_STATE_DEFAULT);

  lv_obj_set_style_pad_all(main_tab, 0, 0);
  lv_obj_set_style_pad_all(console_tab, 0, 0);
  lv_obj_set_style_pad_all(setting_tab, 0, 0);
  lv_obj_set_style_pad_all(sysinfo_tab, 0, 0);

  create_main(main_tab);

  // Pono native cockpit: full-screen on the active screen, over the tabview,
  // hidden until connect (show_home). The tabview stays behind as a safety net.
  home_scr = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(home_scr);
  lv_obj_set_size(home_scr, 480, 272);
  lv_obj_set_pos(home_scr, 0, 0);
  lv_obj_clear_flag(home_scr, LV_OBJ_FLAG_SCROLLABLE);
  pono::HomeModel hm{};
  hm.printing = false; hm.progress_pct = 0; hm.layer = 0; hm.layer_total = 0;
  hm.job_name = ""; hm.material = "PA-CF . 0.25 diamond"; hm.eta = "";
  hm.nozzle = 0; hm.nozzle_set = 0; hm.bed = 0; hm.bed_set = 0;
  pono::build_home(home_scr, hm, &home_h);
  attach_home_taps();
  create_pono_screens();   // native Move/Filament/Temps/Fans/Files/Tune overlays (hidden)
  lv_obj_add_flag(home_scr, LV_OBJ_FLAG_HIDDEN);  // revealed on connect
}

void MainPanel::show_home() {
  if (!home_scr) return;
  lv_obj_clear_flag(home_scr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(home_scr);
}

// The per-action blocking overlay (home/load/unload) and the cal-progress
// overlay drive the SAME g_busy singleton. When an action's gcode completes it
// must clear cal_overlay_ too, or consume()'s redundant-redraw guard would
// never re-show the cal overlay and cal progress goes invisible for the rest of
// the run. Caller holds lv_lock.
void MainPanel::hide_busy_overlay() {
  pono::busy_hide();
  cal_overlay_ = false;
  cal_overlay_text_.clear();
}

// Reset overlay tracking on a link loss. InitPanel::disconnected() (ws thread,
// holding lv_lock) calls this so a Klipper restart / network flap mid-cal can't
// strand the overlay: busy_ is recomputed from the next idle_timeout delta and
// the cal overlay re-shows on reconnect if the cal is still running.
void MainPanel::reset_overlay_state() {
  hide_busy_overlay();
  pono::cal_log_hide();       // a link drop mid-Make-Pono must not strand the logbook
  callog_shown_ = false;
  callog_text_.clear();
  callog_fault_ = false;
  prompt_panel.reset();       // a prompt up at link-loss never gets prompt_end -> showing_ would stay true and suppress the cal overlay all session
  busy_ = false;
}

// Make Pono STOP: stop the calibration. The exit stays frictionless (no confirm)
// - friction belongs on the dangerous action, not the operator's way out (the
// 2026-06-11 freeze had no way out at all).
//
// This used to send CANCEL_PRINT, which cancels a PRINT and does nothing during
// a machine calibration: print_stats.state is standby the whole time, so there
// is no job to cancel. The companion macro the old comment called "bench-gated"
// was never written, so the one control the narration box advertises was inert.
// PONO_CAL_STOP now exists in pono-print-os and this is its caller.
//
// The acknowledgement is local and immediate, and that is not cosmetic. Klipper
// runs one gcode at a time, so the stop is ACCEPTED instantly and EXECUTED when
// the running step returns, which for the bed mesh is around twenty minutes.
// Measured on the machine 2026-08-04. Without a local reply the operator taps
// STOP, sees the same screen for twenty minutes, and reasonably concludes the
// button is dead - which is the exact impression the old inert button gave.
// Saying how long is what keeps this honest rather than merely responsive.
void MainPanel::_callog_stop(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  auto *s = static_cast<MainPanel *>(lv_event_get_user_data(e));
  s->ws.gcode_script("PONO_CAL_STOP");
  s->notice("Stopping. The step now running has to finish first, and the bed mesh takes about twenty minutes. Nothing will be saved. To halt right now, use E-STOP.");
}

// Cockpit tile taps route to the existing (proven) control panels, which
// overlay above the cockpit and close back to it.
void MainPanel::_home_tap(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  auto *s = static_cast<MainPanel *>(lv_event_get_user_data(e));
  lv_obj_t *t = lv_event_get_target(e);
  pono::HomeHandles &h = s->home_h;
  if (t == h.btn_pausestop) {                           // Resume (paused) / Pause (printing) / Print->Files (idle)
    // Show the action in flight: PAUSE parks the head and RESUME unparks, each
    // a few seconds during which the button alone would read as a dead tap.
    if (s->home_paused_)        { pono::busy_show("Resuming..."); s->ws.gcode_script("RESUME", [s](json &) { std::lock_guard<std::mutex> lk(s->lv_lock); s->hide_busy_overlay(); }); }
    else if (s->home_printing_) { pono::busy_show("Pausing...");  s->ws.gcode_script("PAUSE",  [s](json &) { std::lock_guard<std::mutex> lk(s->lv_lock); s->hide_busy_overlay(); }); }
    else { s->populate_files(); s->show_pono(s->files_scr_); }
  }
  else if (t == h.btn_cancel) {                         // abort the running/paused job (confirmed)
    s->confirm("Cancel this print?", [s]{ s->ws.gcode_script("CANCEL_PRINT"); });
  }
  else if (t == h.qa[0]) s->show_pono(s->move_scr_);    // Move
  else if (t == h.qa[1]) s->show_pono(s->fil_scr_);     // Filament
  else if (t == h.qa[2]) { s->populate_files(); s->show_pono(s->files_scr_); }  // Files
  else if (t == h.qa[3]) s->show_pono(s->fan_scr_);     // Fans
  else if (t == h.tile_nozzle || t == h.tile_bed) s->show_pono(s->temp_scr_);   // temps
  else if (t == h.tile_tune) s->show_pono(s->tune_scr_);  // Tune
  else if (t == h.tile_more) s->show_pono(s->more_scr_);  // More menu
  else if (t == h.hero) {                               // the dictionary entry: next gloss
    if (s->gloss_ix_ < 0) s->gloss_ix_ = pono::gloss_today_index();
    s->gloss_ix_ = (s->gloss_ix_ + 1) % pono::gloss_count();
    pono::home_set_gloss(&h, s->gloss_ix_);
  }
}

// ---- Pono native sub-screen management ----

void MainPanel::create_pono_screens() {
  auto make = [&]() -> lv_obj_t * {
    lv_obj_t *s = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, 480, 272);
    lv_obj_set_pos(s, 0, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    return s;
  };
  move_scr_  = make(); pono::build_move(move_scr_, &move_h_);
  fil_scr_   = make(); pono::build_filament(fil_scr_, &fil_h_);
  temp_scr_  = make(); pono::build_temps(temp_scr_, &temp_h_);
  fan_scr_   = make(); pono::build_fans(fan_scr_, &fan_h_);
  files_scr_ = make(); pono::build_files(files_scr_, &files_h_);
  tune_scr_  = make(); pono::build_tune(tune_scr_, &tune_h_);
  more_scr_  = make(); pono::build_more(more_scr_, &more_h_);
  settings_scr_ = make(); pono::build_settings(settings_scr_, &settings_h_);
  mesh_scr_   = make(); pono::build_mesh(mesh_scr_, &mesh_h_);
  system_scr_ = make(); pono::build_system(system_scr_, &system_h_);
  power_scr_  = make(); pono::build_power(power_scr_, &power_h_);
  lights_scr_ = make(); pono::build_lights(lights_scr_, &lights_h_);

  lv_obj_t *taps[] = {
    move_h_.back, move_h_.xplus, move_h_.xminus, move_h_.yplus, move_h_.yminus,
    move_h_.zplus, move_h_.zminus, move_h_.home_xy, move_h_.home_all, move_h_.motors_off,
    move_h_.step[0], move_h_.step[1], move_h_.step[2], move_h_.step[3],
    fil_h_.back, fil_h_.load, fil_h_.unload, fil_h_.extrude, fil_h_.retract, fil_h_.temp,
    fil_h_.preset[0], fil_h_.preset[1], fil_h_.preset[2], fil_h_.cooldown,
    temp_h_.back, temp_h_.nz_preset[0], temp_h_.nz_preset[1], temp_h_.nz_preset[2], temp_h_.nz_off,
    temp_h_.bd_preset[0], temp_h_.bd_preset[1], temp_h_.bd_preset[2], temp_h_.bd_off,
    temp_h_.nz_minus, temp_h_.nz_plus, temp_h_.bd_minus, temp_h_.bd_plus,
    temp_h_.nz_cur, temp_h_.bd_cur,
    fan_h_.back,
    files_h_.back,
    tune_h_.back, tune_h_.standard, tune_h_.omega,
    tune_h_.cals[0], tune_h_.cals[1], tune_h_.cals[2], tune_h_.cals[3], tune_h_.cals[4],
    more_h_.back, more_h_.wifi, more_h_.expert, more_h_.mesh, more_h_.led, more_h_.system, more_h_.power,
    mesh_h_.back, system_h_.back, system_h_.btn_install, system_h_.integrity,
    lights_h_.back, lights_h_.case_off, lights_h_.case_50, lights_h_.case_full,
    lights_h_.hot_off, lights_h_.hot_50, lights_h_.hot_full,
    power_h_.back, power_h_.restart_klipper, power_h_.restart_fw, power_h_.reboot, power_h_.shutdown,
    settings_h_.back, settings_h_.speed, settings_h_.flow, settings_h_.zoff, settings_h_.pa, settings_h_.fan,
    settings_h_.speed_p[0], settings_h_.speed_p[1], settings_h_.speed_p[2],
    settings_h_.flow_p[0], settings_h_.flow_p[1], settings_h_.flow_p[2],
    settings_h_.fan_p[0], settings_h_.fan_p[1], settings_h_.fan_p[2],
    settings_h_.zoff_minus, settings_h_.zoff_plus,
  };
  for (lv_obj_t *t : taps) if (t) lv_obj_add_event_cb(t, &MainPanel::_sub_tap, LV_EVENT_CLICKED, this);
  for (int i = 0; i < 3; i++)
    if (fan_h_.slider[i]) lv_obj_add_event_cb(fan_h_.slider[i], &MainPanel::_fan_slider_cb, LV_EVENT_RELEASED, this);
  if (tune_h_.speed)      lv_obj_add_event_cb(tune_h_.speed, &MainPanel::_fan_slider_cb, LV_EVENT_RELEASED, this);
  // Load-length slider: VALUE_CHANGED so the mm readout tracks the finger live.
  if (fil_h_.len_slider)  lv_obj_add_event_cb(fil_h_.len_slider, &MainPanel::_fan_slider_cb, LV_EVENT_VALUE_CHANGED, this);
  pono::seg_highlight(fil_h_.preset, 3, fil_mat_);  // PA-CF preselected: this is a PA printer

  // Modal confirm dialog on the top layer (above every sub-screen).
  pono::build_confirm(lv_layer_top(), &confirm_h_);
  if (confirm_h_.cancel)  lv_obj_add_event_cb(confirm_h_.cancel,  &MainPanel::_confirm_tap, LV_EVENT_CLICKED, this);
  if (confirm_h_.confirm) lv_obj_add_event_cb(confirm_h_.confirm, &MainPanel::_confirm_tap, LV_EVENT_CLICKED, this);
  if (confirm_h_.scrim)   lv_obj_add_event_cb(confirm_h_.scrim,   &MainPanel::_confirm_tap, LV_EVENT_CLICKED, this);

  // Informational notice modal (B9): paragraph-length copy the confirm card
  // cannot hold (the unofficial-build notice). OK and the scrim both dismiss.
  pono::build_notice(lv_layer_top(), &notice_h_);
  if (notice_h_.ok)    lv_obj_add_event_cb(notice_h_.ok,    &MainPanel::_notice_tap, LV_EVENT_CLICKED, this);
  if (notice_h_.scrim) lv_obj_add_event_cb(notice_h_.scrim, &MainPanel::_notice_tap, LV_EVENT_CLICKED, this);

  // Persistent full-kill E-STOP on the top layer: above the cockpit AND every
  // sub-screen, surviving rebuild_home(). Built last so it sits on top; the
  // confirm dialog still raises above it (confirm() move_foreground) so the
  // kill is gated by a single yes/no.
  estop_btn_ = pono::build_estop(lv_layer_top());
  if (estop_btn_) {
    lv_obj_add_event_cb(estop_btn_, &MainPanel::_estop_tap, LV_EVENT_CLICKED, this);
    // Keep the kill switch reachable. The busy/cal/numpad scrims are full-screen
    // children of lv_layer_top() that move_foreground over the E-STOP, leaving it
    // untappable during exactly the motion (homing, filament load, cal, value
    // entry) when it is needed most. A light keepalive re-raises it above any
    // such overlay - but never above the confirm dialog, which is the E-STOP's
    // own yes/no and must stay on top to be answerable. Owned + cancelled in the
    // dtor so a torn-down panel cannot leave the timer dereferencing freed self.
    estop_keepalive_timer_ = lv_timer_create(&MainPanel::_estop_keepalive, 300, this);
  }
}

// Persistent E-STOP tap: gate the full kill behind one confirm, then fire
// Moonraker's printer.emergency_stop (halts motion + heaters). The firmware
// safety and the rear rocker remain the ultimate stop. Mirrors the legacy
// emergency_btn path, surfaced on the always-on-top cockpit button.
void MainPanel::_estop_tap(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  auto *s = static_cast<MainPanel *>(lv_event_get_user_data(e));
  s->confirm("Emergency stop the printer?", [s]{ s->ws.send_jsonrpc("printer.emergency_stop"); });
}

// Keep the E-STOP on top of any full-screen overlay (busy/cal/numpad scrim) so
// it stays tappable during motion. Skip while the confirm dialog is up, since
// that dialog must sit above the E-STOP to be answered. Cheap: only reorders
// (and redraws its small button) when something is actually covering it.
void MainPanel::_estop_keepalive(lv_timer_t *t) {
  auto *s = static_cast<MainPanel *>(t->user_data);
  if (!s->estop_btn_) return;
  if (s->confirm_h_.card && !lv_obj_has_flag(s->confirm_h_.card, LV_OBJ_FLAG_HIDDEN)) return;
  lv_obj_t *top = lv_layer_top();
  uint32_t n = lv_obj_get_child_cnt(top);
  if (n && lv_obj_get_child(top, n - 1) != s->estop_btn_)
    lv_obj_move_foreground(s->estop_btn_);
}

void MainPanel::confirm(const char *msg, std::function<void()> action) {
  pending_confirm_ = std::move(action);
  if (confirm_h_.msg) lv_label_set_text(confirm_h_.msg, msg);
  if (confirm_h_.scrim)  { lv_obj_clear_flag(confirm_h_.scrim, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(confirm_h_.scrim); }
  if (confirm_h_.card)   { lv_obj_clear_flag(confirm_h_.card,  LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(confirm_h_.card); }
}

void MainPanel::notice(const char *msg) {
  if (notice_h_.msg) lv_label_set_text(notice_h_.msg, msg);
  if (notice_h_.scrim) { lv_obj_clear_flag(notice_h_.scrim, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(notice_h_.scrim); }
  if (notice_h_.card)  { lv_obj_clear_flag(notice_h_.card,  LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(notice_h_.card); }
}

void MainPanel::_notice_tap(lv_event_t *e) {
  auto *s = static_cast<MainPanel *>(lv_event_get_user_data(e));
  if (s->notice_h_.card)  { lv_obj_add_flag(s->notice_h_.card,  LV_OBJ_FLAG_HIDDEN); lv_obj_move_background(s->notice_h_.card); }
  if (s->notice_h_.scrim) { lv_obj_add_flag(s->notice_h_.scrim, LV_OBJ_FLAG_HIDDEN); lv_obj_move_background(s->notice_h_.scrim); }
}

void MainPanel::_confirm_tap(lv_event_t *e) {
  auto *s = static_cast<MainPanel *>(lv_event_get_user_data(e));
  lv_obj_t *t = lv_event_get_target(e);
  bool ok = (t == s->confirm_h_.confirm);
  if (s->confirm_h_.card)  { lv_obj_add_flag(s->confirm_h_.card,  LV_OBJ_FLAG_HIDDEN); lv_obj_move_background(s->confirm_h_.card); }
  if (s->confirm_h_.scrim) { lv_obj_add_flag(s->confirm_h_.scrim, LV_OBJ_FLAG_HIDDEN); lv_obj_move_background(s->confirm_h_.scrim); }
  if (ok && s->pending_confirm_) s->pending_confirm_();
  s->pending_confirm_ = nullptr;
}

void MainPanel::show_pono(lv_obj_t *scr) {
  lv_obj_t *all[] = {move_scr_, fil_scr_, temp_scr_, fan_scr_, files_scr_, tune_scr_, more_scr_, settings_scr_, mesh_scr_, system_scr_, power_scr_, lights_scr_};
  for (lv_obj_t *s : all) if (s) lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
  if (home_scr) lv_obj_add_flag(home_scr, LV_OBJ_FLAG_HIDDEN);
  if (scr) { lv_obj_clear_flag(scr, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(scr); }
}

void MainPanel::back_to_home() {
  lv_obj_t *all[] = {move_scr_, fil_scr_, temp_scr_, fan_scr_, files_scr_, tune_scr_, more_scr_, settings_scr_, mesh_scr_, system_scr_, power_scr_, lights_scr_};
  for (lv_obj_t *s : all) if (s) lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
  show_home();
}

void MainPanel::render_bed_mesh(const json &bm) {
  // bm is a const delta. operator[](json_pointer) on a const json THROWS when a
  // field is absent, and a bed_mesh delta that changes only profile_name carries
  // no probed_matrix. find() keeps a partial delta a no-op instead of a throw the
  // ws thread has to catch (which would drop the rest of that status update).
  auto pm_it = bm.find("probed_matrix");
  if (pm_it != bm.end() && pm_it->is_array() && pm_it->size() > 0 && (*pm_it)[0].is_array()) {
    const auto &pm = *pm_it;
    int rows = (int)pm.size(), cols = (int)pm[0].size();
    mesh_z_.clear();
    float zmin = 1e9f, zmax = -1e9f;
    for (auto &rowj : pm)
      for (auto &cj : rowj) {
        float z = (float)cj.template get<double>();
        mesh_z_.push_back(z);
        if (z < zmin) zmin = z;
        if (z > zmax) zmax = z;
      }
    if (mesh_h_.grid && (int)mesh_z_.size() == rows * cols) {
      pono::mesh_render(mesh_h_.grid, mesh_z_.data(), rows, cols, zmin, zmax);
      if (mesh_h_.range)
        lv_label_set_text(mesh_h_.range, fmt::format("Range: {:.2f} .. {:.2f} mm", zmin, zmax).c_str());
    }
  }
  auto pn_it = bm.find("profile_name");
  if (pn_it != bm.end() && pn_it->is_string() && mesh_h_.profile)
    lv_label_set_text(mesh_h_.profile, fmt::format("Profile: {}", pn_it->template get<std::string>()).c_str());
}

void MainPanel::populate_system() {
  std::string ver = "unknown";
  { std::ifstream f("/etc/pono-version"); std::string line;
    while (std::getline(f, line)) {
      const std::string key = "DISTRO_VERSION=";
      if (line.rfind(key, 0) == 0) {
        ver = line.substr(key.size());
        if (!ver.empty() && ver.front() == '"') ver.erase(0, 1);
        if (!ver.empty() && ver.back() == '"') ver.pop_back();
        break;
      }
    }
  }
  if (system_h_.version) lv_label_set_text(system_h_.version, ver.c_str());

  // Firmware-integrity badge (three states, fail closed): the OS boot check
  // writes /run/pono-integrity with "signed" (verified signed install) or
  // "modified" (flashed some other way, e.g. owner-open FEL/USB). Anything
  // else - absent, unreadable, unrecognized - renders unknown, NEVER
  // official. An unofficial image also gets the friendly notice, once per
  // UI run here plus any time the badge is tapped.
  { std::ifstream f("/run/pono-integrity"); std::string st; std::getline(f, st);
    integrity_ = pono::integrity_state_from_wire(st.c_str());
    pono::system_set_integrity(&system_h_, integrity_);
    if (integrity_ == pono::IntegrityState::Unofficial && !unofficial_notice_shown_) {
      unofficial_notice_shown_ = true;
      notice(pono::kUnofficialBuildNotice);
    } }

  std::string host = "pono-print";
  { std::ifstream f("/proc/sys/kernel/hostname"); std::getline(f, host); }
  if (system_h_.host) lv_label_set_text(system_h_.host, host.c_str());

  // Pick the interface that actually holds a routable IPv4. The old code asked
  // only the wifi interface, which reads 0.0.0.0 on this wired-only printer.
  // Prefer ethernet, then wifi, then any other non-loopback link.
  std::string ip;
  auto good = [](const std::string &s) { return !s.empty() && s != "0.0.0.0"; };
  std::vector<std::string> cand{"eth0"};
  std::string wifi_if = KUtils::get_wifi_interface();
  if (!wifi_if.empty()) cand.push_back(wifi_if);
  for (const auto &nm : KUtils::get_interfaces()) cand.push_back(nm);
  for (const auto &nm : cand) {
    if (nm == "lo") continue;
    std::string c = KUtils::interface_ip(nm);
    if (good(c)) { ip = c; break; }
  }
  if (system_h_.ip) lv_label_set_text(system_h_.ip, ip.empty() ? "--" : ip.c_str());

  { std::ifstream f("/proc/uptime"); double up = 0; f >> up;
    int hh = (int)up / 3600, mm = ((int)up % 3600) / 60;
    if (system_h_.uptime) lv_label_set_text(system_h_.uptime, fmt::format("{}h {}m", hh, mm).c_str()); }

  { std::ifstream f("/sys/class/thermal/thermal_zone0/temp"); long mdeg = -1; f >> mdeg;
    if (system_h_.mcu) lv_label_set_text(system_h_.mcu, mdeg > 0 ? fmt::format("{:.1f}C", mdeg / 1000.0).c_str() : "--"); }

  check_update();
}

// B9 friendly notice: update-pono-print classifies a swupdate refusal and
// writes /run/pono-update-refused ("signature" on line 1, the notice text
// after) ONLY when the SWU failed signature verification - an unofficial
// build, never a corrupted download of an official one (that case keeps the
// generic retry path). Returns the notice text; empty means no signature
// refusal, or any read problem - either way the caller falls through to
// today's plain-failure behavior, so this can never block anything.
static std::string read_update_refusal() {
  std::ifstream f("/run/pono-update-refused");
  std::string cls;
  if (!std::getline(f, cls) || cls != "signature") return "";
  std::string text, line;
  while (std::getline(f, line)) { if (!text.empty()) text += "\n"; text += line; }
  // The OS file normally carries the copy; if it ever arrives bare, fall
  // back to the twin constant vendored here (byte-identical by contract).
  return text.empty() ? std::string(pono::kUnofficialSwuRefusedNotice) : text;
}

// Run a shell command, return trimmed stdout (empty on failure).
static std::string sh_capture(const std::string &cmd) {
  std::string out;
  FILE *p = popen(cmd.c_str(), "r");
  if (!p) return out;
  char buf[256];
  while (fgets(buf, sizeof buf, p)) out += buf;
  pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
    out.pop_back();
  return out;
}

// Compare the firmware host's published build for this device's update channel
// against /etc/pono-version, async. Network runs on a worker thread; label
// writes take lv_lock. The System screen's objects live for the app's
// lifetime (screens are built once, never deleted), so the detached thread's
// handles cannot dangle.
void MainPanel::check_update() {
  if (update_checking_) return;
  update_checking_ = true;
  if (system_h_.update_status) {
    lv_label_set_text(system_h_.update_status, "checking...");
    lv_obj_set_style_text_color(system_h_.update_status, pono::color_text_secondary, 0);
    lv_obj_align(system_h_.update_status, LV_ALIGN_RIGHT_MID, -12, 0);
  }
  if (system_h_.btn_install) lv_obj_add_flag(system_h_.btn_install, LV_OBJ_FLAG_HIDDEN);

  std::thread([this] {
    std::string lane = sh_capture("config-manager update release 2>/dev/null");
    if (lane.empty()) lane = "nightly";

    std::string current;
    { std::ifstream f("/etc/pono-version"); std::string line;
      const std::string key = "DISTRO_VERSION=";
      while (std::getline(f, line)) {
        if (line.rfind(key, 0) == 0) {
          current = line.substr(key.size());
          if (!current.empty() && current.front() == '"') current.erase(0, 1);
          if (!current.empty() && current.back() == '"') current.pop_back();
          break;
        }
      }
    }

    std::string latest, status;
    bool avail = false;
    if (lane == "nightly" || lane == "stable") {
      // version.txt: "pono-print-0.0.1-alpha.172 @ b0ebeff (run 172)".
      // Canonical host first, the fallback host serves the same bucket.
      std::string vt = sh_capture(
        "curl -sf -m 10 https://dl.pono-print.com/pono-print/" + lane + "/version.txt"
        " || curl -sf -m 10 https://dl.ponodata.com/pono-print/" + lane + "/version.txt");
      const std::string pfx = "pono-print-";
      if (vt.rfind(pfx, 0) == 0) {
        latest = vt.substr(pfx.size());
        size_t sp = latest.find(' ');
        if (sp != std::string::npos) latest = latest.substr(0, sp);
      }
      if (latest.empty())         status = "check failed (" + lane + ")";
      else if (latest == current) status = "up to date (" + lane + ")";
      else                        { status = latest + " available"; avail = true; }
    } else {
      // an explicit tag pin: the operator chose a version, don't second-guess it
      status = "pinned: " + lane;
    }

    std::lock_guard<std::mutex> lk(lv_lock);
    update_avail_ = avail ? latest : "";
    if (system_h_.update_status) {
      lv_label_set_text(system_h_.update_status, status.c_str());
      lv_obj_set_style_text_color(system_h_.update_status,
        avail ? pono::color_accent_primary : pono::color_text_secondary, 0);
      // shift left of the INSTALL chip when it is shown
      lv_obj_align(system_h_.update_status, LV_ALIGN_RIGHT_MID, avail ? -106 : -12, 0);
    }
    if (system_h_.btn_install) {
      // D: never offer a firmware flash + reboot mid-print. Show the Install
      // chip only when an update is available AND the machine is idle. Read of
      // home_printing_/paused_ is safe here: this runs under lv_lock, the same
      // lock consume() takes when it writes them.
      if (avail && !home_printing_ && !home_paused_)
        lv_obj_clear_flag(system_h_.btn_install, LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(system_h_.btn_install, LV_OBJ_FLAG_HIDDEN);
    }
    update_checking_ = false;
  }).detach();
}

void MainPanel::_sub_tap(lv_event_t *e) {
  auto *s = static_cast<MainPanel *>(lv_event_get_user_data(e));
  lv_obj_t *t = lv_event_get_target(e);
  pono::MoveHandles &mv = s->move_h_;
  pono::FilamentHandles &fl = s->fil_h_;
  pono::TempsHandles &tp = s->temp_h_;
  pono::FansHandles &fn = s->fan_h_;
  pono::TuneHandles &tu = s->tune_h_;
  // back chips
  if (t == mv.back || t == fl.back || t == tp.back || t == fn.back ||
      t == s->files_h_.back || t == tu.back ||
      t == s->more_h_.back || t == s->settings_h_.back ||
      t == s->mesh_h_.back || t == s->system_h_.back || t == s->power_h_.back ||
      t == s->lights_h_.back) { s->back_to_home(); return; }
  // Move jog (relative). Belt to apply_move_gates(): even if a tap lands
  // before the disable repaints (state flip races the finger), motion
  // commands never dispatch while actively printing, and motors-off never
  // dispatches while printing or paused.
  {
    const bool printing = s->home_printing_ && !s->home_paused_;
    const bool held = s->home_printing_ || s->home_paused_;
    const bool is_motion = (t == mv.xplus || t == mv.xminus || t == mv.yplus ||
                            t == mv.yminus || t == mv.zplus || t == mv.zminus ||
                            t == mv.home_xy || t == mv.home_all);
    if ((printing && is_motion) || (held && t == mv.motors_off)) return;
  }
  double st = s->move_step_;
  if (t == mv.xplus)  { s->ws.gcode_script(fmt::format("G91\nG1 X{} F6000\nG90", st)); return; }
  if (t == mv.xminus) { s->ws.gcode_script(fmt::format("G91\nG1 X-{} F6000\nG90", st)); return; }
  if (t == mv.yplus)  { s->ws.gcode_script(fmt::format("G91\nG1 Y{} F6000\nG90", st)); return; }
  if (t == mv.yminus) { s->ws.gcode_script(fmt::format("G91\nG1 Y-{} F6000\nG90", st)); return; }
  if (t == mv.zplus)  { s->ws.gcode_script(fmt::format("G91\nG1 Z{} F600\nG90", st)); return; }
  if (t == mv.zminus) { s->ws.gcode_script(fmt::format("G91\nG1 Z-{} F600\nG90", st)); return; }
  if (t == mv.home_xy)    { pono::busy_show("Homing X / Y"); s->ws.gcode_script("G28 X Y", [s](json &) { std::lock_guard<std::mutex> lk(s->lv_lock); s->hide_busy_overlay(); }); return; }
  if (t == mv.home_all)   { pono::busy_show("Homing all axes"); s->ws.gcode_script("G28", [s](json &) { std::lock_guard<std::mutex> lk(s->lv_lock); s->hide_busy_overlay(); }); return; }
  if (t == mv.motors_off) { s->ws.gcode_script("M84"); return; }
  for (int i = 0; i < 4; i++) if (t == mv.step[i]) {
    static const double vals[4] = {0.1, 1.0, 10.0, 100.0};
    s->move_step_ = vals[i];
    for (int k = 0; k < 4; k++) {
      if (!mv.step[k]) continue;
      bool on = (k == i);
      lv_obj_set_style_bg_color(mv.step[k], on ? pono::color_accent_primary : pono::color_surface_elevated, 0);
      lv_obj_t *l = lv_obj_get_child(mv.step[k], 0);
      if (l) lv_obj_set_style_text_color(l, on ? pono::color_surface_base : pono::color_text_secondary, 0);
    }
    return;
  }
  // Filament
  // Load/Unload run at the selected material's temp; Load also carries the
  // slider's purge length (LOAD_FILAMENT chunks it under the 120mm/move cap).
  static const int kFilTemp[3] = {220, 240, 260};  // PLA / PETG / PA-CF
  static const int kMinExtrudeTemp = 170;          // Klipper min_extrude_temp floor
  if (t == fl.load)    { pono::busy_show(fmt::format("Loading {}mm at {}C", s->fil_len_, kFilTemp[s->fil_mat_]).c_str()); s->ws.gcode_script(fmt::format("LOAD_FILAMENT EXTRUDER_TEMP={} LENGTH={}", kFilTemp[s->fil_mat_], s->fil_len_), [s](json &) { std::lock_guard<std::mutex> lk(s->lv_lock); s->hide_busy_overlay(); }); return; }
  if (t == fl.unload)  { pono::busy_show(fmt::format("Unloading at {}C", kFilTemp[s->fil_mat_]).c_str()); s->ws.gcode_script(fmt::format("UNLOAD_FILAMENT EXTRUDER_TEMP={}", kFilTemp[s->fil_mat_]), [s](json &) { std::lock_guard<std::mutex> lk(s->lv_lock); s->hide_busy_overlay(); }); return; }
  // Extrude/Retract are raw G1 E with no temperature of their own: gate on a hot
  // nozzle so a stray tap can't grind cold filament / strip the drive gear, and
  // show that it is running (the move takes several seconds).
  if (t == fl.extrude) {
    if (s->home_nozzle_ < kMinExtrudeTemp) { s->confirm(fmt::format("Heat the nozzle to at least {}C before extruding.", kMinExtrudeTemp).c_str(), []{}); return; }
    pono::busy_show("Extruding 25mm");
    s->ws.gcode_script("M83\nG1 E25 F300", [s](json &) { std::lock_guard<std::mutex> lk(s->lv_lock); s->hide_busy_overlay(); });
    return;
  }
  if (t == fl.retract) {
    if (s->home_nozzle_ < kMinExtrudeTemp) { s->confirm(fmt::format("Heat the nozzle to at least {}C before retracting.", kMinExtrudeTemp).c_str(), []{}); return; }
    pono::busy_show("Retracting 25mm");
    s->ws.gcode_script("M83\nG1 E-25 F1800", [s](json &) { std::lock_guard<std::mutex> lk(s->lv_lock); s->hide_busy_overlay(); });
    return;
  }
  // Material segments: select (drives Load/Unload temps) + preheat in one tap.
  for (int i = 0; i < 3; i++) {
    if (t == fl.preset[i]) {
      s->fil_mat_ = i;
      pono::seg_highlight(fl.preset, 3, i);
      s->ws.gcode_script(fmt::format("SET_HEATER_TEMPERATURE HEATER=extruder TARGET={}", kFilTemp[i]));
      return;
    }
  }
  if (t == fl.cooldown)  { s->ws.gcode_script("TURN_OFF_HEATERS"); return; }
  // Tap the nozzle readout to type an exact target (the presets stay; this is
  // the manual override). Mirrors the Temps keypad, clamped to the same cap.
  if (t == fl.temp) { s->numpad.set_callback([s](double v){ int n=(int)(v+0.5); n=n<0?0:(n>300?300:n); s->ws.gcode_script(fmt::format("SET_HEATER_TEMPERATURE HEATER=extruder TARGET={}", n)); }); s->numpad.foreground_reset(); return; }
  // Temps
  if (t == tp.nz_preset[0]) { s->ws.gcode_script("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=220"); return; }
  if (t == tp.nz_preset[1]) { s->ws.gcode_script("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=240"); return; }
  if (t == tp.nz_preset[2]) { s->ws.gcode_script("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=260"); return; }
  if (t == tp.nz_off)       { s->ws.gcode_script("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0"); return; }
  if (t == tp.bd_preset[0]) { s->ws.gcode_script("SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=60"); return; }
  if (t == tp.bd_preset[1]) { s->ws.gcode_script("SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=80"); return; }
  // 45 to match the shipped Sunlu Easy-PA profile (hot_plate_temp 45) and the
  // "PA 45" chip label in build_temps. These two must move together.
  if (t == tp.bd_preset[2]) { s->ws.gcode_script("SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=45"); return; }
  if (t == tp.bd_off)       { s->ws.gcode_script("SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0"); return; }
  // Temps manual steppers: nudge the live target by 5 C (clamped to safe range)
  {
    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    if (t == tp.nz_minus) { s->ws.gcode_script(fmt::format("SET_HEATER_TEMPERATURE HEATER=extruder TARGET={}",  clampi(s->home_nozzle_set_ - 5, 0, 300))); return; }
    if (t == tp.nz_plus)  { s->ws.gcode_script(fmt::format("SET_HEATER_TEMPERATURE HEATER=extruder TARGET={}",  clampi(s->home_nozzle_set_ + 5, 0, 300))); return; }
    if (t == tp.bd_minus) { s->ws.gcode_script(fmt::format("SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET={}", clampi(s->home_bed_set_ - 5, 0, 120))); return; }
    if (t == tp.bd_plus)  { s->ws.gcode_script(fmt::format("SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET={}", clampi(s->home_bed_set_ + 5, 0, 120))); return; }
  }
  // Temps keypad: tap the big number to type an exact target (clamped to heater limits)
  if (t == tp.nz_cur) { s->numpad.set_callback([s](double v){ int n=(int)(v+0.5); n=n<0?0:(n>300?300:n); s->ws.gcode_script(fmt::format("SET_HEATER_TEMPERATURE HEATER=extruder TARGET={}",  n)); }); s->numpad.foreground_reset(); return; }
  if (t == tp.bd_cur) { s->numpad.set_callback([s](double v){ int n=(int)(v+0.5); n=n<0?0:(n>120?120:n); s->ws.gcode_script(fmt::format("SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET={}", n)); }); s->numpad.foreground_reset(); return; }
  // Fans (quick)
  // Tune
  // Safety interlock (the 2026-06-11 Full Cal incident, audit 2026-06-15): never
  // start a calibration on top of a live job. Standard/Make Pono and the Bed Mesh
  // + Input Shaper tiles all home, move, and heat; firing them mid-print would be
  // a crash. Refuse on the device trigger - the gap the host watch cannot close.
  if ((t == tu.standard || t == tu.omega || t == tu.cals[0] || t == tu.cals[3]) &&
      (s->home_printing_ || s->home_paused_)) {
    s->confirm("A job is loaded. Finish or cancel it before calibrating.", []{});
    return;
  }
  // Fire-and-navigate: a run parks the operator on the cockpit, where the cal
  // overlay / OMEGA banner carry progress. Staying on this selector reads as
  // "nothing happened" for the minutes before the runner's first announce.
  // Show a provisional overlay the instant we fire: the cockpit's cal overlay
  // only appears on the macro's first SET_DISPLAY_TEXT, so without this the
  // glass reads "Ready" for the seconds the bed heats and the head homes.
  if (t == tu.standard) { pono::busy_show("Starting calibration..."); s->ws.gcode_script("PONO_CAL_STANDARD"); s->back_to_home(); return; }
  // Make Pono, the tile whose subtitle reads FULL CALIBRATION, runs the full
  // calibration. It used to run PONO_CAL_OMEGA, which writes a request file for
  // a runner on pono-pi that has been disabled since 2026-06-01 (measured
  // 2026-08-04: disabled and inactive), then reported success. So the hero
  // action on this screen calibrated nothing and said it had. PONO_CAL_FULL is
  // the same proven on-device chain as Standard with the load cell forced
  // rather than skipped, which is what makes "full" mean anything.
  if (t == tu.omega)    { pono::busy_show("Starting calibration..."); s->ws.gcode_script("PONO_CAL_FULL"); s->back_to_home(); return; }
  // Individual calibrations (tiles: Bed Mesh, Pressure Adv, Flow, Input Shaper,
  // Z-Offset). Mesh + shaper are real one-shot machine cals: run the proven
  // CALIBRATE_ALL fragments inline, reusing the firmware's GUI safety net
  // (_SHAPER_CAL_GUI_RESTORE) + save prompt (_SAVE_CONFIG_PROMPT). Pressure
  // advance, flow and z-offset have no standalone machine cal on this load-cell
  // printer (PA/flow are vision-graded by OMEGA; z-offset rides the load-cell
  // probe), so those tiles open the live Expert Tune surface that adjusts those
  // exact values -- a real destination, replacing the old placeholder where all
  // five tiles silently ran the full standard cal.
  if (t == tu.cals[0]) {  // Bed Mesh
    s->confirm("Calibrate bed mesh? Heats the bed to 60C and probes the surface.", [s]{
      // TOCTOU: the tap-time job interlock is stale here; a job can start while
      // the confirm is open. Re-check at dispatch so G28 + probe never lands on
      // a live print (the 2026-06-11 Full Cal crash class).
      if (s->home_printing_ || s->home_paused_) { s->notice("A job started. Calibration cancelled."); return; }
      s->ws.gcode_script("SET_DISPLAY_TEXT MSG=\"Calibrating Bed Mesh\"\nG28\nG90\nG1 X128 Y128 F6000\nBED_MESH_CALIBRATE_WITH_WIPE BED_TEMP=60\n_SAVE_CONFIG_PROMPT"); });
    return;
  }
  if (t == tu.cals[3]) {  // Input Shaper
    s->confirm("Calibrate input shaper? The screen pauses while it measures resonance.", [s]{
      if (s->home_printing_ || s->home_paused_) { s->notice("A job started. Calibration cancelled."); return; }
      s->ws.gcode_script("SET_DISPLAY_TEXT MSG=\"Calibrating Input Shaper - screen pauses briefly\"\nG4 P1500\nUPDATE_DELAYED_GCODE ID=_SHAPER_CAL_GUI_RESTORE DURATION=180\nRUN_SHELL_COMMAND CMD=CAMERA_STOP\nRUN_SHELL_COMMAND CMD=GUI_STOP\nG28\nM400\nSHAPER_CALIBRATE AXIS=X\nG4 P1000\nSHAPER_CALIBRATE AXIS=Y\nG4 P1000\nUPDATE_DELAYED_GCODE ID=_SHAPER_CAL_GUI_RESTORE DURATION=0\nRUN_SHELL_COMMAND CMD=CAMERA_START\nRUN_SHELL_COMMAND CMD=GUI_START\n_SAVE_CONFIG_PROMPT"); });
    return;
  }
  if (t == tu.cals[1] || t == tu.cals[2] || t == tu.cals[4]) {  // Pressure Adv / Flow / Z-Offset -> live Expert Tune
    s->show_pono(s->settings_scr_);
    return;
  }
  // More menu rows
  if (t == s->more_h_.wifi)    { s->setting_panel.show_wifi(); return; }       // reuse the wpa scan/connect panel
  if (t == s->more_h_.expert)  { s->show_pono(s->settings_scr_); return; }     // Expert Tune surface
  if (t == s->more_h_.mesh)    { s->show_pono(s->mesh_scr_); return; }         // Bed mesh heatmap
  if (t == s->more_h_.system)  { s->populate_system(); s->show_pono(s->system_scr_); return; }
  if (t == s->more_h_.power)   { s->show_pono(s->power_scr_); return; }
  if (t == s->more_h_.led)     {
    lv_obj_t *cb[3] = {s->lights_h_.case_off, s->lights_h_.case_50, s->lights_h_.case_full};
    lv_obj_t *hb[3] = {s->lights_h_.hot_off,  s->lights_h_.hot_50,  s->lights_h_.hot_full};
    pono::seg_highlight(cb, 3, s->led_case_level_);
    pono::seg_highlight(hb, 3, s->led_hot_level_);
    s->show_pono(s->lights_scr_); return;
  }
  // Firmware badge tap (B9): on an unofficial image, re-read the friendly
  // notice. Any other state, the tap is inert.
  if (t == s->system_h_.integrity) {
    if (s->integrity_ == pono::IntegrityState::Unofficial)
      s->notice(pono::kUnofficialBuildNotice);
    return;
  }
  // Update install: confirm, then run the device's own update path. On
  // success update-pono-print flashes the spare slot and reboots the machine
  // itself, so the busy overlay is honestly the last thing this boot shows.
  if (t == s->system_h_.btn_install) {
    if (s->update_avail_.empty()) return;
    if (s->home_printing_ || s->home_paused_) return;  // D safety belt: never flash + reboot mid-print (the chip is already hidden while printing)
    std::string msg = "Install " + s->update_avail_ +
                      "? The printer flashes the spare slot and reboots itself.";
    s->confirm(msg.c_str(), [s]{
      // TOCTOU: the tap-time gate above is stale by the time this fires. A print
      // can start (remotely, over Moonraker) while the confirm is open, so
      // re-validate at dispatch before flashing + rebooting. This runs on the UI
      // thread under lv_lock, the same lock consume() writes the flags under.
      if (s->home_printing_ || s->home_paused_) { s->notice("A print started. Update cancelled."); return; }
      pono::busy_show("Updating - do not power off");
      std::thread([s]{
        int rc = system("update-pono-print >>/tmp/pono-update-ui.log 2>&1");
        // Reached only on failure (success ends in reboot). Surface the fault.
        // B9: a signature-class refusal (unofficial SWU) gets the friendly
        // notice instead of the bare failure line; read the classification
        // before taking the lock, and fall through to the plain path if it
        // yields nothing.
        std::string refused = rc == 0 ? std::string() : read_update_refusal();
        std::lock_guard<std::mutex> lk(s->lv_lock);
        pono::busy_hide();
        if (s->system_h_.update_status) {
          lv_label_set_text(s->system_h_.update_status,
            rc == 0 ? "rebooting..."
                    : (!refused.empty() ? "not an official build" : "update failed (see log)"));
          lv_obj_set_style_text_color(s->system_h_.update_status,
            rc == 0 ? pono::color_accent_primary
                    : (!refused.empty() ? pono::color_state_warning : pono::color_state_error), 0);
          lv_obj_align(s->system_h_.update_status, LV_ALIGN_RIGHT_MID, -12, 0);
        }
        if (rc != 0 && s->system_h_.btn_install)
          lv_obj_add_flag(s->system_h_.btn_install, LV_OBJ_FLAG_HIDDEN);
        if (!refused.empty()) s->notice(refused.c_str());
      }).detach();
    });
    return;
  }
  // Power actions
  // These all drop the link, so the operator would otherwise see nothing
  // happen until the screen reconnects. Show what's underway; reset_overlay on
  // disconnect (or the watchdog) clears it.
  // Power actions all abort a running job and drop motion control. They are
  // operator-initiated, so the confirm stands, but warn plainly when a print is
  // live so the tap is informed (the message copies into the label at once, so a
  // temporary is safe).
  auto pw = [s](const char *m) {
    return (s->home_printing_ || s->home_paused_)
             ? std::string("A print is running - this stops it. ") + m : std::string(m);
  };
  if (t == s->power_h_.restart_klipper) { s->confirm(pw("Restart Klipper?").c_str(),       [s]{ pono::busy_show("Restarting Klipper..."); s->ws.gcode_script("RESTART"); }); return; }
  if (t == s->power_h_.restart_fw)      { s->confirm(pw("Restart firmware?").c_str(),      [s]{ pono::busy_show("Restarting firmware..."); s->ws.gcode_script("FIRMWARE_RESTART"); }); return; }
  if (t == s->power_h_.reboot)          { s->confirm(pw("Reboot the printer?").c_str(),    [s]{ pono::busy_show("Rebooting..."); s->ws.send_jsonrpc("machine.reboot"); }); return; }
  if (t == s->power_h_.shutdown)        { s->confirm(pw("Shut down the printer?").c_str(), [s]{ pono::busy_show("Shutting down..."); s->ws.send_jsonrpc("machine.shutdown"); }); return; }
  // Lights (SET_LED white channel) - fire the command AND move the highlight
  // to the chosen level so the active selection is visible.
  pono::LightsHandles &li = s->lights_h_;
  lv_obj_t *cb[3] = {li.case_off, li.case_50, li.case_full};
  lv_obj_t *hb[3] = {li.hot_off,  li.hot_50,  li.hot_full};
  if (t == li.case_off)  { s->ws.gcode_script("SET_LED LED=case WHITE=0");    s->led_case_level_ = 0; pono::seg_highlight(cb, 3, 0); return; }
  if (t == li.case_50)   { s->ws.gcode_script("SET_LED LED=case WHITE=0.5");  s->led_case_level_ = 1; pono::seg_highlight(cb, 3, 1); return; }
  if (t == li.case_full) { s->ws.gcode_script("SET_LED LED=case WHITE=1.0");  s->led_case_level_ = 2; pono::seg_highlight(cb, 3, 2); return; }
  if (t == li.hot_off)   { s->ws.gcode_script("SET_LED LED=hotend WHITE=0");   s->led_hot_level_ = 0; pono::seg_highlight(hb, 3, 0); return; }
  if (t == li.hot_50)    { s->ws.gcode_script("SET_LED LED=hotend WHITE=0.5"); s->led_hot_level_ = 1; pono::seg_highlight(hb, 3, 1); return; }
  if (t == li.hot_full)  { s->ws.gcode_script("SET_LED LED=hotend WHITE=1.0"); s->led_hot_level_ = 2; pono::seg_highlight(hb, 3, 2); return; }
  // Expert Tune (live): value pills open the keypad, presets apply directly,
  // z-offset uses live babystep. Every control writes straight to Klipper and
  // shows its request dimmed; the pill goes solid when the readback confirms it
  // (read_tune), or returns to the machine's value if it never does.
  //
  // Z uses MOVE=1 once Z is homed, so the nozzle moves now. MOVE=0 only shifts
  // the coordinate frame (pono-kalico gcode_move.py cmd_SET_GCODE_OFFSET), so
  // mid-print the change waited for the next move naming Z, the next layer:
  // a first-layer correction landed one layer late. Unhomed, MOVE=1 would
  // error, so it stays a pure offset there. The keypad clamps to +/-2 mm,
  // because with MOVE=1 a mistyped value is a move, not a setting.
  pono::SettingsHandles &se = s->settings_h_;
  if (t == se.speed) { s->numpad.set_callback([s](double v){ int sp=(int)(v+0.5); sp=sp<10?10:(sp>300?300:sp); s->ws.gcode_script(fmt::format("M220 S{}", sp)); s->tune_request(TUNE_SPEED, fmt::format("{}%", sp)); }); s->numpad.foreground_reset(); return; }
  if (t == se.flow)  { s->numpad.set_callback([s](double v){ int fl=(int)(v+0.5); fl=fl<50?50:(fl>200?200:fl); s->ws.gcode_script(fmt::format("M221 S{}", fl)); s->tune_request(TUNE_FLOW, fmt::format("{}%", fl)); }); s->numpad.foreground_reset(); return; }
  if (t == se.pa)    { s->numpad.set_callback([s](double v){ double a=v<0?0:(v>1.0?1.0:v); s->ws.gcode_script(fmt::format("SET_PRESSURE_ADVANCE ADVANCE={:.3f}", a)); s->tune_request(TUNE_PA, fmt::format("{:.3f}", a)); }); s->numpad.foreground_reset(); return; }
  if (t == se.zoff)  { s->numpad.set_callback([s](double v){ double z=v<-2.0?-2.0:(v>2.0?2.0:v); s->tune_zoff_ask_=z; s->ws.gcode_script(fmt::format("SET_GCODE_OFFSET Z={:.3f} MOVE={}", z, s->z_move())); s->tune_request(TUNE_ZOFF, fmt::format("{:.3f}", z)); }); s->numpad.foreground_reset(); return; }
  if (t == se.fan)   { s->numpad.set_callback([s](double v){ int p=(int)(v+0.5); p = p<0?0:(p>100?100:p); s->ws.gcode_script(fmt::format("M106 S{}", p*255/100)); s->tune_request(TUNE_FAN, fmt::format("{}%", p)); }); s->numpad.foreground_reset(); return; }
  if (t == se.speed_p[0]) { s->ws.gcode_script("M220 S50");  s->tune_request(TUNE_SPEED, "50%");  return; }
  if (t == se.speed_p[1]) { s->ws.gcode_script("M220 S100"); s->tune_request(TUNE_SPEED, "100%"); return; }
  if (t == se.speed_p[2]) { s->ws.gcode_script("M220 S150"); s->tune_request(TUNE_SPEED, "150%"); return; }
  if (t == se.flow_p[0])  { s->ws.gcode_script("M221 S95");  s->tune_request(TUNE_FLOW, "95%");  return; }
  if (t == se.flow_p[1])  { s->ws.gcode_script("M221 S100"); s->tune_request(TUNE_FLOW, "100%"); return; }
  if (t == se.flow_p[2])  { s->ws.gcode_script("M221 S105"); s->tune_request(TUNE_FLOW, "105%"); return; }
  if (t == se.fan_p[0])   { s->ws.gcode_script("M106 S0");   s->tune_request(TUNE_FAN, "0%");   return; }
  if (t == se.fan_p[1])   { s->ws.gcode_script("M106 S128"); s->tune_request(TUNE_FAN, "50%");  return; }
  if (t == se.fan_p[2])   { s->ws.gcode_script("M106 S255"); s->tune_request(TUNE_FAN, "100%"); return; }
  if (t == se.zoff_minus || t == se.zoff_plus) {
    double d = (t == se.zoff_plus) ? 0.01 : -0.01;
    // Step from the pending value while one is in flight, so fast taps add up
    // on the glass the way Z_ADJUST adds them up on the machine.
    double base = s->tune_ask_[TUNE_ZOFF].empty() ? s->tune_zoff_ : s->tune_zoff_ask_;
    s->tune_zoff_ask_ = base + d;
    s->ws.gcode_script(fmt::format("SET_GCODE_OFFSET Z_ADJUST={:.2f} MOVE={}", d, s->z_move()));
    s->tune_request(TUNE_ZOFF, fmt::format("{:.3f}", s->tune_zoff_ask_));
    return;
  }
}

lv_obj_t *MainPanel::tune_pill(int i) {
  lv_obj_t *p[TUNE_N] = {settings_h_.speed, settings_h_.flow, settings_h_.zoff, settings_h_.pa, settings_h_.fan};
  return (i >= 0 && i < TUNE_N) ? p[i] : nullptr;
}

// Expert Tune readback from a full status (init, root /result/status) or a
// Moonraker delta (consume, root /params/0). A delta carries a field only when
// it changes, so an absent field keeps its last value. A pending request clears
// only when the machine reports that same value. Caller holds lv_lock.
void MainPanel::read_tune(json &j, const char *root) {
  auto V = [&](const char *p) { return j.value(json::json_pointer(fmt::format("{}/{}", root, p)), json()); };
  auto got = [&](int i, const std::string &txt) {
    tune_txt_[i] = txt;
    if (tune_ask_[i].empty() || tune_ask_[i] == txt) { tune_ask_[i].clear(); pono::pill_set(tune_pill(i), txt.c_str()); }
  };
  auto pct = [](double f) { return (int)(f * 100.0 + 0.5); };  // factors are never negative
  { auto v = V("gcode_move/speed_factor");
    if (v.is_number()) {
      int sp = pct(v.template get<double>());
      got(TUNE_SPEED, fmt::format("{}%", sp));
      // The Tune screen's speed slider mirrors the same factor, unless a finger is on it.
      if (tune_h_.speed && !lv_obj_has_state(tune_h_.speed, LV_STATE_PRESSED)) {
        lv_slider_set_value(tune_h_.speed, sp, LV_ANIM_OFF);
        if (tune_h_.speed_val) lv_label_set_text(tune_h_.speed_val, fmt::format("{}%", sp).c_str());
      }
    } }
  { auto v = V("gcode_move/extrude_factor");
    if (v.is_number()) got(TUNE_FLOW, fmt::format("{}%", pct(v.template get<double>()))); }
  { auto v = V("gcode_move/homing_origin");
    if (v.is_array() && v.size() >= 3 && v[2].is_number()) { tune_zoff_ = v[2].template get<double>(); got(TUNE_ZOFF, fmt::format("{:.3f}", tune_zoff_)); } }
  { auto v = V("extruder/pressure_advance");
    if (v.is_number()) got(TUNE_PA, fmt::format("{:.3f}", v.template get<double>())); }
  // Kalico's fan reports value (the request, what M106 S/255 set) and speed
  // (value times max_power). The pill confirms the request, so it reads value.
  { auto v = V("fan/value");
    if (v.is_number()) got(TUNE_FAN, fmt::format("{}%", pct(v.template get<double>()))); }
}

void MainPanel::tune_request(int i, const std::string &txt) {
  tune_ask_[i] = txt;
  pono::pill_pending(tune_pill(i), txt.c_str());
  if (!tune_settle_) tune_settle_ = lv_timer_create(&MainPanel::_tune_settle, 1500, this);
  lv_timer_reset(tune_settle_);
  lv_timer_resume(tune_settle_);
}

// A request the machine never confirmed gives way to what the machine holds.
// Runs inside lv_timer_handler, which the main loop already wraps in lv_lock,
// so it must not relock.
void MainPanel::_tune_settle(lv_timer_t *t) {
  auto *s = static_cast<MainPanel *>(t->user_data);
  lv_timer_pause(t);
  for (int i = 0; i < TUNE_N; i++) {
    if (s->tune_ask_[i].empty()) continue;
    s->tune_ask_[i].clear();
    if (!s->tune_txt_[i].empty()) pono::pill_set(s->tune_pill(i), s->tune_txt_[i].c_str());
  }
}

void MainPanel::_fan_slider_cb(lv_event_t *e) {
  auto *s = static_cast<MainPanel *>(lv_event_get_user_data(e));
  lv_obj_t *t = lv_event_get_target(e);
  int v = lv_slider_get_value(t);
  if (t == s->fil_h_.len_slider) {             // load length: state + readout only, no gcode
    s->fil_len_ = v;
    if (s->fil_h_.len_val) lv_label_set_text(s->fil_h_.len_val, fmt::format("{} mm", v).c_str());
    return;
  }
  if (t == s->tune_h_.speed) {                 // Expert Tune feedrate (M220), not a fan
    s->ws.gcode_script(fmt::format("M220 S{}", v));
    if (s->tune_h_.speed_val) lv_label_set_text(s->tune_h_.speed_val, fmt::format("{}%", v).c_str());
    return;
  }
  // Per-fan sliders: 0 part-cooling (M106), 1 model fan, 2 box fan (generics).
  if (t == s->fan_h_.slider[0])       s->ws.gcode_script(fmt::format("M106 S{}", (int)(v * 255 / 100)));
  else if (t == s->fan_h_.slider[1])  s->ws.gcode_script(fmt::format("SET_FAN_SPEED FAN=model_helper_fan SPEED={:.2f}", v / 100.0));
  else if (t == s->fan_h_.slider[2])  s->ws.gcode_script(fmt::format("SET_FAN_SPEED FAN=box_fan SPEED={:.2f}", v / 100.0));
  else return;
  for (int i = 0; i < 3; i++)
    if (t == s->fan_h_.slider[i] && s->fan_h_.val[i])
      lv_label_set_text(s->fan_h_.val[i], fmt::format("{}%", v).c_str());
}

void MainPanel::_file_row_cb(lv_event_t *e) {
  auto *s = static_cast<MainPanel *>(lv_event_get_user_data(e));
  lv_obj_t *row = lv_event_get_target(e);
  size_t idx = (size_t)(uintptr_t)lv_obj_get_user_data(row);
  if (idx < s->files_names_.size()) {
    std::string fn = s->files_names_[idx];
    std::string base = fn.substr(fn.find_last_of('/') + 1);  // npos+1 == 0 -> whole string
    if (base.size() > 38) base = base.substr(0, 36) + "..";
    s->confirm(fmt::format("Print {}?", base).c_str(), [s, fn]{
      json p = {{"filename", fn}};
      // Don't claim success blindly: return home only when the start is
      // accepted; on a Moonraker error (file gone, not ready, already
      // printing) stay put and show why, instead of cheerfully going to an
      // idle-looking home as if the print began.
      s->ws.send_jsonrpc("printer.print.start", p, [s](json &resp) {
        std::lock_guard<std::mutex> lk(s->lv_lock);
        if (resp.contains("error")) {
          auto m = resp["/error/message"_json_pointer];
          s->confirm(m.is_string() ? m.template get<std::string>().c_str() : "Could not start print", []{});
        } else {
          s->back_to_home();
        }
      });
    });
  }
}

void MainPanel::populate_files() {
  if (!files_h_.list) return;
  json p = {{"root", "gcodes"}};
  ws.send_jsonrpc("server.files.list", p, [this](json &j) {
    std::lock_guard<std::mutex> lock(this->lv_lock);
    if (!this->files_h_.list) return;
    lv_obj_clean(this->files_h_.list);
    this->files_names_.clear();
    uint32_t gen = ++this->files_gen_;  // any async metadata from a prior populate is now stale
    auto &res = j["/result"_json_pointer];
    if (res.is_array()) {
      int n = 0;
      for (auto &f : res) {
        if (n++ >= 40) break;
        std::string path = f.value("path", std::string());
        if (path.empty()) continue;
        this->files_names_.push_back(path);
        double sz = f.value("size", 0.0);  // bytes (server.files.list carries size)
        std::string meta = sz >= 1048576.0 ? fmt::format("{:.1f} MB", sz / 1048576.0)
                         : sz >= 1024.0     ? fmt::format("{:.0f} KB", sz / 1024.0)
                                            : fmt::format("{:.0f} B", sz);
        pono::files_add_row(this->files_h_.list, path.c_str(), meta.c_str());
        lv_obj_t *row = lv_obj_get_child(this->files_h_.list,
                                         lv_obj_get_child_cnt(this->files_h_.list) - 1);
        if (row) {
          lv_obj_set_user_data(row, (void *)(uintptr_t)(this->files_names_.size() - 1));
          lv_obj_add_event_cb(row, &MainPanel::_file_row_cb, LV_EVENT_CLICKED, this);
          // async per-file metadata: thumbnail + est time + filament type
          json mp = {{"filename", path}};
          this->ws.send_jsonrpc("server.files.metadata", mp, [this, gen, row, path](json &meta) {
            std::lock_guard<std::mutex> mlock(this->lv_lock);
            if (gen != this->files_gen_) return;  // Files list was rebuilt; row is stale
            auto thumb = KUtils::get_thumbnail(path, meta, 0.17);
            std::string ipath = thumb.first.empty() ? "" : ("A:" + thumb.first);
            int zoom = thumb.second > 0 ? (int)(46 * 256 / (int)thumb.second) : 0;
            std::string m;
            auto et = meta["/result/estimated_time"_json_pointer];
            if (et.is_number()) {
              int secs = (int)et.template get<double>();
              m = secs >= 3600 ? fmt::format("{}h {}m", secs / 3600, (secs % 3600) / 60)
                               : fmt::format("{}m", secs / 60);
            }
            auto ft = meta["/result/filament_type"_json_pointer];
            if (ft.is_string()) {
              auto t = ft.template get<std::string>();
              m = m.empty() ? t : m + "  .  " + t;
            }
            pono::files_apply_meta(row, ipath.c_str(), zoom, m.c_str());
          });
        }
      }
    }
    if (this->files_names_.empty())
      pono::files_add_row(this->files_h_.list, "No gcode files", "upload via Mainsail");
  });
}

void MainPanel::attach_home_taps() {
  lv_obj_t *taps[] = { home_h.btn_pausestop, home_h.btn_cancel, home_h.qa[0], home_h.qa[1],
                       home_h.qa[2], home_h.qa[3], home_h.tile_nozzle, home_h.tile_bed,
                       home_h.tile_tune, home_h.tile_more, home_h.hero };
  for (lv_obj_t *t : taps) if (t) lv_obj_add_event_cb(t, &MainPanel::_home_tap, LV_EVENT_CLICKED, this);
}

// Rebuild the cockpit in the layout matching the current state. build_home is
// sim-verified for both idle and printing, so a rebuild guarantees the right
// layout rather than swapping fonts/labels/colors in place. Called only on the
// idle<->printing flip (rare), so the cost is irrelevant.
void MainPanel::rebuild_home() {
  if (!home_scr) return;
  pono::HomeModel m{};
  m.printing = home_printing_ || home_paused_;   // printing layout for both (frozen while paused)
  m.paused = home_paused_;
  m.progress_pct = (int)(home_progress_ * 100.0 + 0.5);
  m.layer = home_layer_; m.layer_total = home_layer_total_;
  m.job_name = home_job_.c_str();
  m.material = "PA-CF . 0.25 diamond";
  m.nozzle = home_nozzle_; m.nozzle_set = home_nozzle_set_;
  m.bed = home_bed_; m.bed_set = home_bed_set_;
  if (m.printing && home_progress_ > 0.01 && home_duration_ > 1.0) {
    double remain = home_duration_ * (1.0 - home_progress_) / home_progress_;
    if (remain < 0.0) remain = 0.0;
    int mins = (int)(remain / 60.0 + 0.5);
    home_eta_ = mins >= 60 ? fmt::format("{}:{:02d} left", mins / 60, mins % 60)
                           : fmt::format("{} min left", mins);
  } else {
    home_eta_.clear();
  }
  m.eta = home_eta_.c_str();
  lv_obj_clean(home_scr);                  // drop old children + their anims
  pono::build_home(home_scr, m, &home_h);  // repopulates home_h with fresh handles
  attach_home_taps();
  // keep the operator's advanced gloss across layout flips (a print ends, the
  // entry returns where they left it; -1 = the day's gloss, already built in)
  if (!m.printing && gloss_ix_ >= 0) pono::home_set_gloss(&home_h, gloss_ix_);
  home_pulsing_ = home_printing_ && !home_paused_;  // build_home pulses only while actively printing
  // fresh label handles -> force the next consume() to repaint temps/fans into them
  rend_nozzle_ = rend_nozzle_set_ = rend_bed_ = rend_bed_set_ = INT_MIN;
  for (int i = 0; i < 5; i++) rend_fan_[i] = INT_MIN;
  apply_move_gates();  // print state changed: re-gate the Move screen with it
}

// Mid-print motion is the one tap that wrecks a job from the glass: G28 drags
// the head through the part, a 100mm jog ditto, M84 drops the steppers and
// loses position. Jog + home are disabled while actively PRINTING (jogging
// while PAUSED is a real workflow - inspection, filament - and RESUME
// restores position). Motors-off gates while printing OR paused: position
// loss makes the resume wrong either way. Belt and suspenders with the
// _sub_tap guard; the dim makes the gate visible on the glass.
void MainPanel::apply_move_gates() {
  pono::MoveHandles &mv = move_h_;
  const bool printing = home_printing_ && !home_paused_;
  const bool held = home_printing_ || home_paused_;
  lv_obj_t *motion[] = {mv.xplus, mv.xminus, mv.yplus, mv.yminus,
                        mv.zplus, mv.zminus, mv.home_xy, mv.home_all};
  for (lv_obj_t *o : motion) {
    if (!o) continue;
    lv_obj_set_style_opa(o, LV_OPA_40, LV_PART_MAIN | LV_STATE_DISABLED);
    if (printing) lv_obj_add_state(o, LV_STATE_DISABLED);
    else          lv_obj_clear_state(o, LV_STATE_DISABLED);
  }
  if (mv.motors_off) {
    lv_obj_set_style_opa(mv.motors_off, LV_OPA_40, LV_PART_MAIN | LV_STATE_DISABLED);
    if (held) lv_obj_add_state(mv.motors_off, LV_STATE_DISABLED);
    else      lv_obj_clear_state(mv.motors_off, LV_STATE_DISABLED);
  }
}

void MainPanel::handle_homing_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    LOG_TRACE("clicked homing");
    homing_panel.foreground();
  }
}

void MainPanel::handle_extrude_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    LOG_TRACE("clicked extruder");
    extruder_panel.foreground();
  }
}

void MainPanel::handle_fanpanel_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    LOG_TRACE("clicked fan panel");
    fan_panel.foreground();
  }
}

void MainPanel::handle_ledpanel_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    LOG_TRACE("clicked led panel");
    led_panel.activate();
    led_btn.set_image(led_panel.get_main_button_image());
  }
}

void MainPanel::handle_emergency_cb(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    LOG_TRACE("clicked emergency");
  }
}

void MainPanel::create_main(lv_obj_t * parent) {
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW_WRAP);

  static lv_coord_t grid_main_row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
  static lv_coord_t grid_main_col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
    LV_GRID_TEMPLATE_LAST};

  lv_obj_clear_flag(main_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_height(main_cont, LV_PCT(100));

  lv_obj_set_flex_grow(main_cont, 1);
  lv_obj_set_grid_dsc_array(main_cont, grid_main_col_dsc, grid_main_row_dsc);

  lv_obj_set_grid_cell(homing_btn.get_container(), LV_GRID_ALIGN_CENTER, 2, 1, LV_GRID_ALIGN_CENTER, 0, 1);
  lv_obj_set_grid_cell(extrude_btn.get_container(), LV_GRID_ALIGN_CENTER, 3, 1, LV_GRID_ALIGN_CENTER, 0, 1);
  lv_obj_set_grid_cell(action_btn.get_container(), LV_GRID_ALIGN_CENTER, 2, 1, LV_GRID_ALIGN_CENTER, 1, 1);
  lv_obj_set_grid_cell(led_btn.get_container(), LV_GRID_ALIGN_CENTER, 3, 1, LV_GRID_ALIGN_CENTER, 1, 1);
  lv_obj_set_grid_cell(emergency_btn.get_container(), LV_GRID_ALIGN_CENTER, 3, 1, LV_GRID_ALIGN_CENTER, 2, 1);

  lv_obj_clear_flag(temp_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(temp_cont, LV_PCT(50), LV_PCT(50));
  lv_obj_set_style_pad_all(temp_cont, 0, 0);

  lv_obj_set_flex_flow(temp_cont, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_grid_cell(temp_cont, LV_GRID_ALIGN_START, 0, 2, LV_GRID_ALIGN_CENTER, 0, 2);

  lv_obj_align(temp_chart, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_size(temp_chart, LV_PCT(45), LV_PCT(40));
  lv_obj_set_style_size(temp_chart, 0, LV_PART_INDICATOR);

  lv_chart_set_range(temp_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 300);
  lv_obj_set_grid_cell(temp_chart, LV_GRID_ALIGN_END, 0, 2, LV_GRID_ALIGN_END, 2, 1);
  lv_chart_set_axis_tick(temp_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 0, 6, 5, true, 50);

  lv_chart_set_div_line_count(temp_chart, 3, 8);
  lv_chart_set_point_count(temp_chart, 5000);
  lv_chart_set_zoom_x(temp_chart, 5000);
  lv_obj_scroll_to_x(temp_chart, LV_COORD_MAX, LV_ANIM_OFF);
}

void MainPanel::create_sensors(json &temp_sensors) {
  std::lock_guard<std::mutex> lock(lv_lock);
  sensors.clear();
  for (auto &sensor : temp_sensors.items()) {
    std::string key = sensor.key();
    bool controllable = sensor.value().value("controllable", false);  // absent -> false, never throw out of the ws connect path

    // Temp-sensor accent color path. String-keyed presets map to fixed Pono
    // tokens; numeric int config falls through to color_for_palette() with
    // NONE sentinel + out-of-range safe default in the getter.
    lv_color_t color_code = pono::color_state_warning;  // default
    if (sensor.value().value("color", json()).is_string()) {
      std::string color = sensor.value()["color"].template get<std::string>();
      if (color == "red") {
	      color_code = pono::color_state_error;
      } else if (color == "purple") {
	      color_code = pono::color_state_intel;
      } else if (color == "blue") {
	      color_code = pono::color_accent_primary;
      }
    } else if (sensor.value().value("color", json()).is_number()) {
      color_code = pono::color_for_palette((lv_palette_t)sensor.value().value("color", json()).template get<int>());
    }  // absent/unexpected color type -> keep the default (no throw)

    std::string display_name = sensor.value().value("display_name", key);  // absent -> key, never throw

    const void* sensor_img = &heater;
    if (key == "extruder") {
      sensor_img = &extruder;
    } else if (key == "heater_bed") {
      sensor_img = &bed;
    }

    lv_chart_series_t *temp_series =
      lv_chart_add_series(temp_chart, color_code, LV_CHART_AXIS_PRIMARY_Y);

    sensors.insert({key, std::make_shared<SensorContainer>(ws, temp_cont, sensor_img, 150,
			   display_name.c_str(), color_code, controllable, false, numpad, key,
        		   temp_chart, temp_series)});
  }
}

void MainPanel::create_fans(json &fans) {
  fan_panel.create_fans(fans);
}

void MainPanel::create_leds(json &leds) {
  if (leds.is_array() && !leds.empty()) {
    led_btn.enable();
  } else {
    led_btn.disable();
  }
  led_panel.init(leds);
  led_btn.set_image(led_panel.get_main_button_image());
}

void MainPanel::enable_spoolman() {
  spoolman_panel.init();
  extruder_panel.enable_spoolman();
}
