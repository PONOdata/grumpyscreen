#include "guppyscreen.h"

#include "config.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"
#include "logger.h"
#include "state.h"
#include "theme.h"
#include "pono_theme.h"  // Phase A.4: text_tertiary token for disabled imgbtn recolor
#ifdef GUPPY_CALIBRATE
#include <fstream>
#endif
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;

GuppyScreen *GuppyScreen::instance = NULL;
lv_style_t GuppyScreen::style_container;
lv_style_t GuppyScreen::style_imgbtn_default;
lv_style_t GuppyScreen::style_imgbtn_pressed;
lv_style_t GuppyScreen::style_imgbtn_disabled;
lv_theme_t GuppyScreen::th_new;

lv_obj_t *GuppyScreen::screen_saver = NULL;

KWebSocketClient GuppyScreen::ws(NULL);

std::mutex GuppyScreen::lv_lock;

GuppyScreen::GuppyScreen()
  : spoolman_panel(ws, lv_lock)
  , main_panel(ws, lv_lock, spoolman_panel)
  , init_panel(ws, main_panel, lv_lock)
{
  main_panel.create_panel();
}

GuppyScreen *GuppyScreen::get() {
  if (instance == NULL) {
    instance = new GuppyScreen();
  }

  return instance;
}

GuppyScreen *GuppyScreen::init(std::function<void(lv_color_t, lv_color_t)> hal_init) {
  hlog_disable();

  // config
  Config *conf = Config::get_instance();
  auto ll = conf->get<std::string>("/ui/log_level");
  set_log_level(ll);

  const std::string selected_theme = conf->get<std::string>("/ui/theme");
  auto theme_config = fs::path("/usr/share/grumpyscreen") / "themes" / (selected_theme + ".json");
  ThemeConfig *theme_conf = ThemeConfig::get_instance();
  theme_conf->init(theme_config);

  auto theme_primary_color = theme_conf->get<std::string>("/primary_color");
  auto theme_secondary_color = theme_conf->get<std::string>("/secondary_color");

  // A theme JSON that is present but omits/misformats these keys yields "" from
  // ThemeConfig::get, and std::stoul("") throws on the boot path; fall back to the
  // ThemeConfig defaults (blue / red) rather than aborting before the first paint.
  auto parse_hex = [](const std::string &s, unsigned long dflt) -> unsigned long {
    try { return std::stoul(s, nullptr, 16); }
    catch (const std::exception &) { return dflt; }
  };
  auto primary_color = lv_color_hex(parse_hex(theme_primary_color, 0x2196F3));
  auto secondary_color = lv_color_hex(parse_hex(theme_secondary_color, 0xF44336));

  LOG_INFO("GrumpyScreen Version: {}-{}", GUPPYSCREEN_BRANCH, GUPPYSCREEN_VERSION);

  LOG_INFO("DPI: {}", LV_DPI_DEF);
  /*LittlevGL init*/
  lv_init();

  /*Linux frame buffer device init*/
  fbdev_init();
  fbdev_unblank();

  hal_init(primary_color, secondary_color);
  lv_png_init();

  lv_style_init(&style_container);
  lv_style_set_border_width(&style_container, 0);
  lv_style_set_radius(&style_container, 0);

  lv_style_init(&style_imgbtn_pressed);
  lv_style_set_img_recolor_opa(&style_imgbtn_pressed, LV_OPA_100);
  lv_style_set_img_recolor(&style_imgbtn_pressed, primary_color);

  lv_style_init(&style_imgbtn_disabled);
  lv_style_set_img_recolor_opa(&style_imgbtn_disabled, LV_OPA_100);
  lv_style_set_img_recolor(&style_imgbtn_disabled, pono::color_text_tertiary);  // disabled imgbtn recolor

  /*Initia1ize the new theme from the current theme*/

  lv_theme_t *th_act = lv_disp_get_theme(NULL);
  th_new = *th_act;

  /*Set the parent theme and the style apply callback for the new theme*/
  lv_theme_set_parent(&th_new, th_act);
  lv_theme_set_apply_cb(&th_new, &GuppyScreen::new_theme_apply_cb);

  /*Assign the new theme to the current display*/
  lv_disp_set_theme(NULL, &th_new);

  ws.register_notify_update(State::get_instance());

  GuppyScreen *gs = GuppyScreen::get();
  // start initializing all guppy components
  std::string ws_url = fmt::format("ws://{}:{}/websocket",
                                   conf->get<std::string>("/moonraker/host"),
                                   conf->get<uint32_t>("/moonraker/port"));

  // Screen saver on the system layer, the one LVGL draws and hit-tests above
  // the top layer. On the active screen it sat under the top layer, so the
  // E-STOP drew over the saver and the tap that wakes the display landed on
  // the E-STOP. Up here the saver takes that tap itself. Built before the
  // socket opens so no ws callback can touch LVGL while it is being made.
  screen_saver = lv_obj_create(lv_layer_sys());
  lv_obj_remove_style_all(screen_saver);
  lv_obj_set_size(screen_saver, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(screen_saver, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(screen_saver, LV_OPA_COVER, 0);
  lv_obj_clear_flag(screen_saver, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(screen_saver, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);

  LOG_INFO("connecting to printer at {}", ws_url);
  gs->connect_ws(ws_url);

#ifdef GUPPY_CALIBRATE
  lv_obj_t *main_screen = lv_disp_get_scr_act(NULL);
  std::vector<float> c = GuppyScreen::load_calibration_coeff();
  if (c.empty()) {
    lv_tc_register_coeff_save_cb(&GuppyScreen::save_calibration_coeff);
    lv_obj_t *touch_calibrate_scr = lv_tc_screen_create();
    lv_disp_load_scr(touch_calibrate_scr);
    lv_tc_screen_start(touch_calibrate_scr);
    lv_obj_add_event_cb(touch_calibrate_scr, &GuppyScreen::handle_calibrated, LV_EVENT_READY, main_screen);
    LOG_INFO("running touch calibration");
  } else {
    // load calibration data
    lv_tc_coeff_t coeff = {true, c[0], c[1], c[2], c[3], c[4], c[5]};
    lv_tc_set_coeff(coeff, false);
    LOG_INFO("loaded calibration coefficients");
  }
#endif
  return gs;
}

void GuppyScreen::loop() {
  /*Handle LitlevGL tasks (tickless mode)*/
  std::atomic_bool is_sleeping(false);
  Config *conf = Config::get_instance();
  int32_t display_sleep = conf->get<int32_t>("/ui/display_sleep_sec") * 1000;

  while (1) {
    lv_lock.lock();
    lv_timer_handler();
    lv_lock.unlock();

    // display_sleep_sec was multiplied by 1000 above, so the -1 "never sleep"
    // sentinel is now -1000; treat any negative value as never-sleep.
    if (display_sleep >= 0) {
      if (lv_disp_get_inactive_time(NULL) > display_sleep) {
        if (!is_sleeping.load()) {
          LOG_DEBUG("putting display to sleeping");
          fbdev_blank();
          {
            std::lock_guard<std::mutex> lock(lv_lock);   // the ws thread writes LVGL too
            lv_obj_clear_flag(screen_saver, LV_OBJ_FLAG_HIDDEN);
          }
          // LOG_DEBUG("screen saver foreground");
          is_sleeping = true;
        }
      } else {
        if (is_sleeping.load()) {
          LOG_DEBUG("waking up display");
          fbdev_unblank();
          {
            std::lock_guard<std::mutex> lock(lv_lock);
            lv_obj_add_flag(screen_saver, LV_OBJ_FLAG_HIDDEN);
          }
          is_sleeping = false;
        }
      }
    }

    usleep(5000);
  }
}

std::mutex &GuppyScreen::get_lock() {
  return lv_lock;
}

void GuppyScreen::connect_ws(const std::string &url) {
  init_panel.set_message("Waiting for Klipper to start...");
  ws.connect(url.c_str(),
   [this]() { init_panel.connected(ws); },
   [this]() { init_panel.disconnected(ws); });
}

void GuppyScreen::new_theme_apply_cb(lv_theme_t *th, lv_obj_t *obj) {
  LV_UNUSED(th);

  if (lv_obj_check_type(obj, &lv_obj_class)) {
    lv_obj_add_style(obj, &style_container, 0);
  }

  if (lv_obj_check_type(obj, &lv_imgbtn_class)) {
    lv_obj_add_style(obj, &style_imgbtn_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(obj, &style_imgbtn_disabled, LV_STATE_DISABLED);
  }
}

#ifdef GUPPY_CALIBRATE
void GuppyScreen::handle_calibrated(lv_event_t *event) {
  LOG_INFO("finished calibration");
  lv_obj_t *main_screen = (lv_obj_t *)event->user_data;
  lv_disp_load_scr(main_screen);
}

std::vector<float> GuppyScreen::load_calibration_coeff() {
  std::string config_path = fs::canonical("/proc/self/exe").parent_path() / "calibration.json";
  std::ifstream f(config_path);
  if (!f.is_open()) {
    return {};
  }

  json j;
  f >> j;
  if (!j.is_array()) {
    return {};
  }

  std::vector<float> coeffs;
  coeffs.reserve(j.size());
  for (const auto& v : j) {
    coeffs.push_back(v.get<float>());
  }
  return coeffs;
}

void GuppyScreen::save_calibration_coeff(lv_tc_coeff_t coeff) {
  auto config_path = fs::canonical("/proc/self/exe").parent_path() / "calibration.json";
  json j = {coeff.a, coeff.b, coeff.c, coeff.d, coeff.e, coeff.f};
  std::ofstream f(config_path, std::ios::trunc);
  f << j.dump(2);
}
#endif

// Phase A.4 pass 4: refresh_theme() removed. It was dead-code: grepped zero
// callers across src/. The function re-initialized lv_theme_default_init
// from the legacy JSON primary/secondary, which pono::theme_init in main.cpp
// overrides at startup. If a future runtime theme switcher is needed for
// Phase G theme variants (Mainsail Blue / High-contrast / Pono Cyan-on-Black
// per docs/design/pono-print-ui-design.md sec 4.6), it lives in
// pono_theme.cpp as a sibling to theme_init, not here.

/*Set in lv_conf.h as `LV_TICK_CUSTOM_SYS_TIME_EXPR`*/
uint32_t custom_tick_get(void) {
  static uint64_t start_ms = 0;
  if (start_ms == 0) {
    struct timeval tv_start;
    gettimeofday(&tv_start, NULL);
    start_ms = (tv_start.tv_sec * 1000000 + tv_start.tv_usec) / 1000;
  }

  struct timeval tv_now;
  gettimeofday(&tv_now, NULL);
  uint64_t now_ms;
  now_ms = (tv_now.tv_sec * 1000000 + tv_now.tv_usec) / 1000;

  uint32_t time_ms = now_ms - start_ms;
  return time_ms;
}
