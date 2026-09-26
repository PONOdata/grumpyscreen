#ifndef __MAIN_PANEL_H__
#define __MAIN_PANEL_H__

#include "websocket_client.h"
#include "notify_consumer.h"
#include "sensor_container.h"
#include "button_container.h"
#include "prompt_panel.h"
#include "numpad.h"
#include "homing_panel.h"
#include "extruder_panel.h"
#include "fan_panel.h"
#include "led_panel.h"
#include "console_panel.h"
#include "setting_panel.h"
#include "sysinfo_panel.h"
#include "spoolman_panel.h"
#include "pono_home.h"
#include "lvgl/lvgl.h"

#include <mutex>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <climits>      // INT_MIN: render-shadow sentinels (rend_* members below)

class MainPanel : public NotifyConsumer {
 public:
  MainPanel(KWebSocketClient &ws,
	    std::mutex &lv_lock,
	    SpoolmanPanel &sm);

  ~MainPanel();
  void consume(json &data);
  void init(json &data);
  void subscribe();
  void enable_spoolman();
  
  void create_panel();
  void show_home();                      // Pono: bring the native cockpit to front (on connect)
  void reset_overlay_state();            // Pono: clear busy/cal overlay tracking on link loss (InitPanel::disconnected)
  static void _home_tap(lv_event_t *e);  // Pono: cockpit tile -> existing control panels
  void create_sensors(json &temp_sensors);
  void create_fans(json &temp_fans);
  void create_leds(json &leds);
  void handle_homing_cb(lv_event_t *event);
  void handle_extrude_cb(lv_event_t *event);
  void handle_fanpanel_cb(lv_event_t *event);
  void handle_ledpanel_cb(lv_event_t *event);
  void handle_emergency_cb(lv_event_t *event);

  lv_obj_t *create_button(lv_obj_t *parent,
			  const void *btn_img,
			  const char* text,
			  lv_event_cb_t cb);

  lv_obj_t *create_heater_info(lv_obj_t *parent,
			       const void *heater_img,
			       const char* text,
			       lv_color_t color);
  
  static void _handle_homing_cb(lv_event_t *event) {
    MainPanel *panel = (MainPanel*)event->user_data;
    panel->handle_homing_cb(event);
  };

  static void _handle_extrude_cb(lv_event_t *event) {
    MainPanel *panel = (MainPanel*)event->user_data;
    panel->handle_extrude_cb(event);
  };

  static void _handle_fanpanel_cb(lv_event_t *event) {
    MainPanel *panel = (MainPanel*)event->user_data;
    panel->handle_fanpanel_cb(event);
  };

  static void _handle_ledpanel_cb(lv_event_t *event) {
    MainPanel *panel = (MainPanel*)event->user_data;
    panel->handle_ledpanel_cb(event);
  };

  static void _handle_emergency_cb(lv_event_t *event) {
      MainPanel *panel = (MainPanel*)event->user_data;
      panel->handle_emergency_cb(event);
    };

 private:
  void create_main(lv_obj_t *parent);
  void rebuild_home();        // Pono: rebuild the cockpit in the current state's layout (idle<->printing flip)
  void check_stale();         // Pono: T6 watchdog body (runs under the main loop's lv_lock; must not relock)
  void apply_move_gates();    // Pono: gate Move-screen motion on print state (mid-print G28/jog/M84 wrecks the job)
  void attach_home_taps();    // Pono: wire cockpit tap targets to _home_tap (reused after rebuild)
  // ---- Pono native sub-screens (replace the legacy panels) ----
  void create_pono_screens();             // build Move/Filament/Temps/Fans/Files/Tune into hidden overlays
  void show_pono(lv_obj_t *scr);          // hide cockpit + others, reveal scr
  void back_to_home();                    // hide all sub-screens, show the cockpit
  void hide_busy_overlay();               // Pono: drop the shared busy/cal overlay + clear cal_overlay_ (so consume() can re-show cal after a manual action)
  void populate_system();                 // fill the System screen (version/ip/uptime)
  void check_update();                    // async: compare the firmware host's published build to /etc/pono-version
  void confirm(const char *msg, std::function<void()> action);  // modal confirm before destructive actions
  static void _confirm_tap(lv_event_t *e);
  void notice(const char *msg);            // informational modal: paragraph copy + OK (B9 unofficial notice)
  static void _notice_tap(lv_event_t *e);  // OK / scrim -> dismiss the notice
  static void _estop_tap(lv_event_t *e);   // persistent E-STOP -> confirm -> printer.emergency_stop
  static void _estop_keepalive(lv_timer_t *t);  // re-raise E-STOP above busy/cal/numpad scrims (never above the confirm)
  void render_bed_mesh(const json &bm);   // draw the heatmap from a bed_mesh status object
  void populate_files();                  // query Moonraker, fill the Files list
  static void _sub_tap(lv_event_t *e);    // sub-screen button -> gcode action
  static void _callog_stop(lv_event_t *e);  // Make Pono STOP -> CANCEL_PRINT (frictionless exit)
  static void _fan_slider_cb(lv_event_t *e);
  void read_tune(json &j, const char *root);          // Expert Tune readback from a status (init) or delta (consume)
  lv_obj_t *tune_pill(int i);
  void tune_request(int i, const std::string &txt);   // show a sent value as pending, arm the settle timer
  static void _tune_settle(lv_timer_t *t);
  int z_move() const { return move_homed_.find('z') != std::string::npos ? 1 : 0; }
  static void _file_row_cb(lv_event_t *e);
  static void _tabview_event_cb(lv_event_t *e);
  KWebSocketClient &ws;
  HomingPanel homing_panel;
  FanPanel fan_panel;
  LedPanel led_panel;
  lv_obj_t *tabview;
  lv_obj_t *main_tab;
  lv_obj_t *console_tab;
  ConsolePanel console_panel;
  lv_obj_t *setting_tab;
  SettingPanel setting_panel;
  lv_obj_t *sysinfo_tab;
  SysInfoPanel sysinfo_panel;
  lv_obj_t *main_cont;
  lv_obj_t *home_scr = nullptr;   // Pono native cockpit, full-screen over the tabview
  pono::HomeHandles home_h;       // live handles into the cockpit
  bool home_printing_ = false;    // last-known printing state (ETA + pulse gating)
  bool home_paused_ = false;      // last-known paused state (Resume vs Pause button)
  bool home_pulsing_ = false;     // is the state-dot pulse currently running
  lv_timer_t *stale_timer_ = nullptr;  // T6: periodic readout-staleness check (notifies can stop; a timer cannot)
  lv_timer_t *estop_keepalive_timer_ = nullptr;  // re-raises the E-STOP above full-screen scrims; owned here, cancelled in the dtor
  bool stale_shown_ = false;           // is the stale flag currently visible (touched only under lv_lock)
  int gloss_ix_ = -1;             // dictionary-entry position once the operator advances it (-1 = today's)
  bool update_checking_ = false;  // a version check is in flight (guard re-entry)
  std::string update_avail_;      // newer published version ("0.0.1-alpha.NNN"), empty = none
  bool busy_ = false;             // idle_timeout.state=="Printing" (print OR cal) -> keep panel awake
  std::string cal_msg_;           // display_status.message (cal step text via SET_DISPLAY_TEXT)
  std::string cal_overlay_text_;  // text currently on the cal overlay (skip redundant redraws)
  bool cal_overlay_ = false;      // is the cal progress overlay currently shown
  bool omega_banner_ = false;     // is the OMEGA print banner currently shown (Phase B/C prints)
  std::string omega_banner_text_; // text currently on the OMEGA banner (skip redundant redraws)
  bool callog_shown_ = false;       // is the Make Pono narration logbook currently shown
  std::string callog_text_;         // raw step text on the logbook now (skip redundant redraws)
  std::string callog_now_, callog_next_;    // last parsed NOW/NEXT (watchdog restores them on resume)
  int callog_done_ = 0, callog_total_ = 0;  // last parsed count (kept on the lost-contact alarm)
  bool callog_fault_ = false;       // is the logbook currently showing the lost-contact alarm
  double home_progress_ = 0.0;    // last-seen virtual_sdcard progress (survives Moonraker deltas)
  double home_duration_ = 0.0;    // last-seen print_stats.print_duration (survives Moonraker deltas)
  int home_nozzle_ = 0, home_nozzle_set_ = 0;   // cached temps (survive deltas; feed the rebuild model)
  int home_bed_ = 0, home_bed_set_ = 0;
  int home_layer_ = 0, home_layer_total_ = 0;
  std::string home_job_;          // current filename (stable storage for the rebuild model)
  std::string home_eta_;          // computed ETA string (stable storage for the rebuild model)
  std::string move_homed_;        // cached toolhead.homed_axes for the Move position readout
  double move_pos_[3] = {0, 0, 0};// cached toolhead X/Y/Z (survive Moonraker deltas)
  // Per-tick render shadows: consume() fires on every Moonraker delta, so the
  // cockpit/temp/fan labels were re-set + re-aligned several times a second even
  // when unchanged -- wasted redraws competing with the 60fps anims on this
  // software-rendered SoC. Repaint only when the value actually moves.
  int rend_nozzle_ = INT_MIN, rend_nozzle_set_ = INT_MIN;
  int rend_bed_ = INT_MIN, rend_bed_set_ = INT_MIN;
  int rend_fan_[5] = { INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN };
  const void *rend_led_img_ = nullptr;
  // ---- Pono native sub-screens ----
  lv_obj_t *move_scr_ = nullptr, *fil_scr_ = nullptr, *temp_scr_ = nullptr;
  lv_obj_t *fan_scr_ = nullptr, *files_scr_ = nullptr, *tune_scr_ = nullptr;
  lv_obj_t *more_scr_ = nullptr, *settings_scr_ = nullptr;
  pono::MoveHandles move_h_;
  pono::FilamentHandles fil_h_;
  pono::TempsHandles temp_h_;
  pono::FansHandles fan_h_;
  pono::FilesHandles files_h_;
  pono::TuneHandles tune_h_;
  pono::MoreHandles more_h_;
  pono::SettingsHandles settings_h_;
  double tune_zoff_ = 0.0;         // machine Z offset, gcode_move.homing_origin[2]
  // Expert Tune readback: the pills show what Klipper holds (gcode_move,
  // extruder, fan), not what was sent. A tap shows its request dimmed until the
  // readback matches; the settle timer puts the machine's value back if the
  // request never lands (refused, clamped, or changed by a macro or another client).
  enum { TUNE_SPEED, TUNE_FLOW, TUNE_ZOFF, TUNE_PA, TUNE_FAN, TUNE_N };
  std::string tune_txt_[TUNE_N];   // machine value, formatted as its pill shows it
  std::string tune_ask_[TUNE_N];   // pending request text, empty when none
  double tune_zoff_ask_ = 0.0;     // pending Z offset, the base for the next babystep
  int tune_speed_ = 100, tune_speed_ask_ = 100;  // machine and pending speed %, the base for the next step
  int rend_melt_ = INT_MIN;        // melt readout shadow, tenths of mm3/s
  lv_timer_t *tune_settle_ = nullptr;
  int fil_mat_ = 2;                // selected material segment (0 PLA / 1 PETG / 2 PA-CF)
  int fil_len_ = 100;              // load purge length in mm (slider, used by Load)
  lv_obj_t *mesh_scr_ = nullptr, *system_scr_ = nullptr, *power_scr_ = nullptr, *lights_scr_ = nullptr;
  pono::MeshHandles mesh_h_;
  pono::SystemHandles system_h_;
  pono::PowerHandles power_h_;
  pono::LightsHandles lights_h_;
  // 0=off, 1=50%, 2=full. These seed the Lights screen highlight, and nothing
  // reads the LEDs back, so a wrong seed makes the screen assert a state the
  // hardware is not in. machine.cfg gives [led case] initial_WHITE: 1 and gives
  // [led hotend] no initial at all, so at boot case is full and hotend is OFF.
  // Confirmed on the live machine: led case color_data W=1.0, led hotend W=0.0.
  int led_case_level_ = 2;
  int led_hot_level_ = 0;
  pono::ConfirmHandles confirm_h_;            // modal confirm dialog (on lv_layer_top)
  std::function<void()> pending_confirm_;     // action to run if the user confirms
  pono::NoticeHandles notice_h_;              // informational notice modal (on lv_layer_top)
  pono::IntegrityState integrity_ = pono::IntegrityState::Unknown;  // badge state (drives the badge tap)
  bool unofficial_notice_shown_ = false;      // auto-show the unofficial notice once per UI run
  lv_obj_t *estop_btn_ = nullptr;             // persistent full-kill E-STOP on lv_layer_top()
  std::vector<float> mesh_z_;      // flattened probed_matrix for the heatmap
  double move_step_ = 1.0;        // selected jog step (mm)
  std::vector<std::string> files_names_;  // index -> gcode filename for row taps
  uint32_t files_gen_ = 0;                 // bumped on each Files rebuild; async metadata checks it before touching a row
  Numpad numpad;
  ExtruderPanel extruder_panel;
  PromptPanel prompt_panel;
  SpoolmanPanel &spoolman_panel;
  
  lv_style_t style;

  lv_obj_t *temp_cont;
  lv_obj_t *temp_chart;

  std::map<std::string, std::shared_ptr<SensorContainer>> sensors;
  
  ButtonContainer homing_btn;
  ButtonContainer extrude_btn;
  ButtonContainer action_btn;
  ButtonContainer led_btn;
  ButtonContainer emergency_btn;
};
#endif // __MAIN_PANEL_H__
