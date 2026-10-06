#ifndef __NUMPAD_H__
#define __NUMPAD_H__

#include "lvgl/lvgl.h"
#include <functional>
#include <cmath>
#include <string>

struct NumpadSpec {
  const char *title = "";
  const char *unit = "";
  double value = NAN;
  double min = 0, max = 0;  // min == max: unbounded
  int decimals = 0;
};

class Numpad {
 public:
  Numpad(lv_obj_t *parent);
  Numpad(const Numpad &) = delete;
  Numpad &operator=(const Numpad &) = delete;
  Numpad(Numpad &&) = delete;
  Numpad &operator=(Numpad &&) = delete;

  void open(const NumpadSpec &spec, std::function<void(double)> on_ok);
  void set_callback(std::function<void(double)> cb);
  void foreground_reset();
  void dismiss();
  bool is_open() const;

 private:
  void update_display();
  void update_keys();
  void validate();
  void handle_key(lv_event_t *e);

  static void _handle_key(lv_event_t *e) {
    Numpad *p = (Numpad *)lv_event_get_user_data(e);
    p->handle_key(e);
  }
  static void _handle_cancel(lv_event_t *e) {
    Numpad *p = (Numpad *)lv_event_get_user_data(e);
    p->dismiss();
  }
  static void _draw_part(lv_event_t *e);

  lv_obj_t *panel;
  lv_obj_t *title_label;
  lv_obj_t *entry_label;
  lv_obj_t *unit_label;
  lv_obj_t *range_label;
  lv_obj_t *was_label;
  lv_obj_t *cancel_btn;
  lv_obj_t *kb;

  NumpadSpec spec;
  std::string typed;
  bool untouched = true;
  std::function<void(double)> on_ok_cb;
  std::function<void(double)> persistent_cb;
};

#endif // __NUMPAD_H__
