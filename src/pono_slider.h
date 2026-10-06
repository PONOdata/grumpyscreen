#pragma once

// Fine-drag slider: no jump on touch, relative drag, finer gain the further
// the finger rolls away from the bar, and a live value bubble on the top
// layer. Built for a 480x272 resistive panel.

#include "lvgl.h"

namespace pono {

// Turn an lv_slider into a fine-drag slider. unit is shown beside the value
// in the bubble (copied; up to 7 characters). Per-slider config lives on the
// heap and is freed on LV_EVENT_DELETE. A tap under the drag slop still
// reaches the app's CLICKED / SHORT_CLICKED handlers; a drag does not.
void fine_slider_attach(lv_obj_t *slider, const char *unit);

}  // namespace pono
