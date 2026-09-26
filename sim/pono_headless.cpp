// SPDX-License-Identifier: GPL-3.0-only
// pono_headless.cpp - headless LVGL renderer for Pono Print UI iteration
//
// Renders a Pono screen to an off-screen framebuffer and dumps a 24-bit BMP -
// NO SDL, NO display hardware, NO X server. Links against only LVGL + the
// pono_theme/pono_home builders, so it compiles anywhere g++ + the in-repo
// LVGL submodule live (here: MinGW on Windows). This is the "on glass" loop:
// build a screen, render it, look at the pixels, before ever flashing.
//
// Usage:
//   pono-headless.exe <out.bmp> [advance_ms] [screen]
//     out.bmp     output path (default out.bmp)
//     advance_ms  ms of animation to advance before capture (default 700)
//     screen      which screen to build (default "home")

#include "lvgl.h"
#include "pono_theme.h"
#include "pono_home.h"
#include "pono_anim.h"
#include "prompt_layout.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define PW 480
#define PH 272

static lv_color_t g_fb[PW * PH]; // accumulated screen framebuffer

// LVGL here is built LV_TICK_CUSTOM=1 with SYS_TIME_EXPR = custom_tick_get().
// The real app defines that symbol; the headless harness supplies it as a
// counter we advance by hand to drive animations to a chosen frame.
static uint32_t g_tick_ms = 0;
extern "C" uint32_t custom_tick_get(void) { return g_tick_ms; }

// lv_extra.c initialises the extra/libs enabled in lv_conf (FS_STDIO, PNG).
// Those .c are excluded from the sim build (POSIX/3rd-party), so satisfy the
// linker with no-op stubs - the cockpit needs no filesystem nor PNG decode.
extern "C" void lv_fs_stdio_init(void) {}
extern "C" void lv_png_init(void) {}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  for (int y = area->y1; y <= area->y2; y++) {
    for (int x = area->x1; x <= area->x2; x++) {
      if (x >= 0 && x < PW && y >= 0 && y < PH) g_fb[y * PW + x] = *color_p;
      color_p++;
    }
  }
  lv_disp_flush_ready(drv);
}

static void put_le32(unsigned char *p, unsigned int v) {
  p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static int write_bmp(const char *path) {
  const int rowsize = (PW * 3 + 3) & ~3;
  const int datasize = rowsize * PH;
  const int filesize = 54 + datasize;
  unsigned char hdr[54];
  memset(hdr, 0, sizeof hdr);
  hdr[0] = 'B'; hdr[1] = 'M';
  put_le32(hdr + 2, filesize);
  put_le32(hdr + 10, 54);
  put_le32(hdr + 14, 40);
  put_le32(hdr + 18, PW);
  put_le32(hdr + 22, PH);
  hdr[26] = 1; hdr[28] = 24;
  put_le32(hdr + 34, datasize);
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  fwrite(hdr, 1, 54, f);
  unsigned char *row = (unsigned char *)calloc(rowsize, 1);
  for (int y = PH - 1; y >= 0; y--) { // BMP rows are bottom-up
    for (int x = 0; x < PW; x++) {
      lv_color32_t c;
      c.full = lv_color_to32(g_fb[y * PW + x]);
      row[x * 3 + 0] = c.ch.blue;
      row[x * 3 + 1] = c.ch.green;
      row[x * 3 + 2] = c.ch.red;
    }
    fwrite(row, 1, rowsize, f);
  }
  free(row);
  fclose(f);
  return 0;
}

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : "out.bmp";
  int advance_ms = argc > 2 ? atoi(argv[2]) : 700;
  std::string screen = argc > 3 ? argv[3] : "home";

  lv_init();

  static lv_disp_draw_buf_t dbuf;
  static lv_color_t buf1[PW * PH];
  lv_disp_draw_buf_init(&dbuf, buf1, NULL, PW * PH);

  static lv_disp_drv_t ddrv;
  lv_disp_drv_init(&ddrv);
  ddrv.hor_res = PW;
  ddrv.ver_res = PH;
  ddrv.flush_cb = flush_cb;
  ddrv.draw_buf = &dbuf;
  lv_disp_t *disp = lv_disp_drv_register(&ddrv);

  pono::theme_init(disp);

  if (screen == "boot") {
    // The real connecting screen (build_boot, shared with init_panel). Drive it
    // to a mid-load stage so the bar + status read as they will on the device.
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);   // as init_panel's cont: the flag's wake must not show scrollbars
    static pono::BootHandles bh;
    pono::build_boot(scr, &bh);
    // Seed the longest joke so the wrap region is stress-tested (the app cycles
    // the book; here we just render one).
    if (bh.joke)
      lv_label_set_text(bh.joke,
        "Overhangs with no supports are like cliff jumping at Black Rock: all confidence and good cooling.");
    pono::boot_set_progress(&bh, 4, "Waiting for Klipper to start...");
    pono::boot_play_intro(&bh);   // Act 1; step the clock to scan the wake
    // Act 2 scripted with one-shot timers (mirrors init_panel's connect): reveal
    // the progress at ~1.6s, then step the real stages so the GIF scans both acts.
    lv_timer_t *r = lv_timer_create([](lv_timer_t *t) {
      auto *h = (pono::BootHandles *)t->user_data;
      pono::boot_reveal_progress(h);
      pono::boot_set_progress(h, 22, "Reading the printer...");
    }, 1600, &bh);
    lv_timer_set_repeat_count(r, 1);
    lv_timer_t *s2 = lv_timer_create([](lv_timer_t *t) {
      pono::boot_set_progress((pono::BootHandles *)t->user_data, 55, "Loading printer state...");
    }, 2200, &bh);
    lv_timer_set_repeat_count(s2, 1);
    lv_timer_t *s3 = lv_timer_create([](lv_timer_t *t) {
      pono::boot_set_progress((pono::BootHandles *)t->user_data, 100, "Ready");
    }, 2800, &bh);
    lv_timer_set_repeat_count(s3, 1);
  } else if (screen == "boot_fault" || screen == "boot_error") {
    // The fault view init_panel shows when printer.info reports shutdown or
    // error: Klipper's own reason and the Restart firmware action. boot_error
    // carries a long config error to prove the two-line reason wrap.
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    static pono::BootHandles bh;
    static bool err = (screen == "boot_error");
    pono::build_boot(scr, &bh);
    pono::boot_set_progress(&bh, 4, "Waiting for Klipper to start...");
    pono::boot_play_intro(&bh);
    lv_timer_t *f = lv_timer_create([](lv_timer_t *t) {
      auto *h = (pono::BootHandles *)t->user_data;
      pono::boot_reveal_progress(h);
      if (err)
        pono::boot_show_fault(h, "Klipper can't start",
          "Option 'max_accel' in section 'printer' must have minimum of 1.0 but the value read from printer.cfg was 0.0");
      else
        pono::boot_show_fault(h, "Klipper stopped", "Shutdown due to M112 command");
    }, 1600, &bh);
    lv_timer_set_repeat_count(f, 1);
  } else if (screen == "move") {
    pono::build_move(lv_scr_act());
  } else if (screen == "filament") {
    pono::build_filament(lv_scr_act());
  } else if (screen == "temps" || screen == "temp") {
    pono::build_temps(lv_scr_act());
  } else if (screen == "fans") {
    static pono::FansHandles fh;
    pono::build_fans(lv_scr_act(), &fh);
    int demo[5] = {65, 100, 40, 38, 100};
    for (int i = 0; i < 5; i++) {
      char b[8]; snprintf(b, sizeof b, "%d%%", demo[i]);
      if (fh.val[i]) lv_label_set_text(fh.val[i], b);
      if (fh.slider[i]) lv_slider_set_value(fh.slider[i], demo[i], LV_ANIM_OFF);
    }
  } else if (screen == "files") {
    pono::build_files(lv_scr_act());
  } else if (screen == "tune") {
    pono::build_tune(lv_scr_act());
  } else if (screen == "settings" || screen == "expert") {
    pono::build_settings(lv_scr_act());
  } else if (screen == "expert_z") {
    // The same sheet scrolled to the Z rows, which sit below the fold.
    pono::SettingsHandles sh;
    pono::build_settings(lv_scr_act(), &sh);
    lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(sh.zoff));
    lv_obj_clear_flag(sh.reset, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, lv_obj_get_y(lv_obj_get_parent(sh.zstep[0])) - 60, LV_ANIM_OFF);
    fprintf(stderr, "expert_z: zstep row y=%d, scrolled to %d, %d below\n",
            (int)lv_obj_get_y(lv_obj_get_parent(sh.zstep[0])), (int)lv_obj_get_scroll_y(list),
            (int)lv_obj_get_scroll_bottom(list));
  } else if (screen == "more") {
    pono::build_more(lv_scr_act());
  } else if (screen == "mesh") {
    pono::build_mesh(lv_scr_act());
  } else if (screen == "system") {
    pono::SystemHandles sh; pono::build_system(lv_scr_act(), &sh);
    pono::system_set_integrity(&sh, pono::integrity_state_from_wire("signed"));
  } else if (screen == "system_modified") {
    pono::SystemHandles sh; pono::build_system(lv_scr_act(), &sh);
    pono::system_set_integrity(&sh, pono::integrity_state_from_wire("modified"));
  } else if (screen == "notice") {
    // B9 unofficial-build notice modal over the System screen, as the app
    // shows it on an unofficial image (auto once per run + badge tap).
    pono::SystemHandles sh; pono::build_system(lv_scr_act(), &sh);
    pono::system_set_integrity(&sh, pono::integrity_state_from_wire("modified"));
    pono::NoticeHandles nh;
    pono::build_notice(lv_scr_act(), &nh);
    lv_label_set_text(nh.msg, pono::kUnofficialBuildNotice);
    lv_obj_clear_flag(nh.scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(nh.card, LV_OBJ_FLAG_HIDDEN);
  } else if (screen == "notice_refused") {
    // The SWU-refusal flavor (the longer copy the update row surfaces).
    pono::SystemHandles sh; pono::build_system(lv_scr_act(), &sh);
    pono::NoticeHandles nh;
    pono::build_notice(lv_scr_act(), &nh);
    lv_label_set_text(nh.msg, pono::kUnofficialSwuRefusedNotice);
    lv_obj_clear_flag(nh.scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(nh.card, LV_OBJ_FLAG_HIDDEN);
  } else if (screen == "power") {
    pono::build_power(lv_scr_act());
  } else if (screen == "lights") {
    pono::build_lights(lv_scr_act());
  } else if (screen == "confirm") {
    pono::ConfirmHandles ch;
    pono::build_confirm(lv_scr_act(), &ch);
    lv_label_set_text(ch.msg, "Shut down the printer?");
    lv_obj_clear_flag(ch.scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ch.card, LV_OBJ_FLAG_HIDDEN);
  } else if (screen == "idle") {
    pono::build_home(lv_scr_act(), pono::demo_home_idle_model());
  } else if (screen == "paused") {
    pono::build_home(lv_scr_act(), pono::demo_home_paused_model());
  } else if (screen == "stale") {
    // T6 demo: mid-print with the readout-stale flag up. The app's watchdog
    // raises this when the status stream goes quiet on an open socket.
    static pono::HomeHandles hh;
    pono::build_home(lv_scr_act(), pono::demo_home_model(), &hh);
    pono::home_set_stale(&hh, 23);
  } else if (screen == "spinner") {
    // Canned-animation demo: a baked comet spinner over a heating wait state.
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, pono::color_surface_base, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_t *sp = pono::spinner_create(scr, 1000);
    lv_obj_align(sp, LV_ALIGN_CENTER, 0, -18);
    lv_obj_t *lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl, pono::color_text_primary, 0);
    lv_obj_set_style_text_font(lbl, pono::font_body, 0);
    lv_label_set_text(lbl, "Heating nozzle");
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 52);
    lv_obj_t *sub = lv_label_create(scr);
    lv_obj_set_style_text_color(sub, pono::color_text_secondary, 0);
    lv_obj_set_style_text_font(sub, pono::font_caption, 0);
    lv_label_set_text(sub, "248 / 250");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 72);
  } else if (screen == "omega") {
    // OMEGA print banner demo: the thin amber status strip carrying the live
    // step + running grade over a running print (the Phase B/C case the cal
    // overlay can't cover because it is gated to !printing).
    pono::build_home(lv_scr_act(), pono::demo_home_model());
    pono::omega_status_show("OMEGA 8/29: Bridging -> OK (8 OK)");
  } else if (screen == "busy") {
    // Working overlay demo: scrim + comet spinner + status over the idle home.
    pono::build_home(lv_scr_act(), pono::demo_home_idle_model());
    pono::busy_show("Homing all axes");
  } else if (screen == "makepono") {
    // Make Pono narration box: the honest NOW/NEXT logbook + a true bar + STOP.
    pono::build_home(lv_scr_act(), pono::demo_home_idle_model());
    pono::cal_log_show("Finding true Z", "next  bed mesh, then input shaping", 5, 40, false, nullptr, nullptr);
  } else if (screen == "prompt" || screen == "prompt_long") {
    // The real calibrate-all dialog: the three action:prompt_text lines
    // _CALIBRATE_ALL_STEP_1 emits, verbatim, carrying the live 45C / 265C from
    // _PONO_PRINT_SETTINGS. "prompt_long" appends two more lines to exercise the
    // scroll fallback, because four is not an unusual count and other flows emit
    // more. Renders the SAME prompt_layout the app uses, not a copy of it.
    pono::build_home(lv_scr_act(), pono::demo_home_idle_model());
    static pono::PromptLayout pl;
    pono::prompt_layout_build(lv_scr_act(), &pl);
    pono::prompt_layout_reset(&pl);
    // The long case also carries an overlong title on purpose: LV_LABEL_LONG_DOT
    // honours height, so an unpinned header wraps and spills into the body
    // instead of ellipsizing. This renders that regression if the pin is lost.
    lv_label_set_text(pl.header, (screen == "prompt_long")
      ? "Calibrate all, a deliberately overlong title to prove LONG_DOT ellipsizes instead of wrapping"
      : "Calibrate all");

    static const char *kLines[] = {
      "The screen and camera will be disabled during calibration. When the calibration is complete, you will be prompted to save the updated configuration.",
      "The bed mesh will be calibrated with a bed temperature of 45C. The extruder PID will be calibrated with a hotend temperature of 265C.",
      "Please clear out the chamber and bed of any foreign objects and click Ready.",
      "The load cell will be re-zeroed against the current resting reading. It needs no mass on the bed and prompts for none.",
      "Input shaping runs on both axes and takes about four minutes per axis.",
    };
    const int n = (screen == "prompt_long") ? 5 : 3;
    lv_obj_t *lbl[5];
    for (int i = 0; i < n; i++) lbl[i] = pono::prompt_layout_add_text(&pl, kLines[i]);

    static const char *kBtns[] = {"Back", "Ready"};
    for (int i = 0; i < 2; i++) {
      lv_obj_t *b = lv_btn_create(pl.footer);
      lv_obj_set_size(b, lv_pct(45), 32);
      lv_obj_set_style_max_height(b, 54, 0);
      lv_obj_set_style_min_height(b, 42, 0);
      lv_obj_set_flex_grow(b, 1);
      lv_obj_t *t = lv_label_create(b);
      lv_label_set_text(t, kBtns[i]);
      lv_obj_center(t);
    }

    pono::prompt_layout_fit(&pl);
    lv_obj_move_foreground(pl.cont);
    lv_obj_update_layout(pl.cont);

    // The measurement. A picture can be squinted at; these numbers cannot.
    // For each line: the label's box against the height its wrapped text needs.
    fprintf(stderr, "-- prompt fit (%d lines) --\n", n);
    fprintf(stderr, "card=%dx%d of %dx%d  body h=%d  body_overflow=%d\n",
            lv_obj_get_width(pl.cont), lv_obj_get_height(pl.cont), PW, PH,
            lv_obj_get_height(pl.body), lv_obj_get_scroll_bottom(pl.body));
    int clipped = 0;
    for (int i = 0; i < n; i++) {
      lv_point_t need;
      lv_txt_get_size(&need, kLines[i],
                      lv_obj_get_style_text_font(lbl[i], LV_PART_MAIN),
                      lv_obj_get_style_text_letter_space(lbl[i], LV_PART_MAIN),
                      lv_obj_get_style_text_line_space(lbl[i], LV_PART_MAIN),
                      lv_obj_get_content_width(lbl[i]), LV_TEXT_FLAG_NONE);
      bool fits = lv_obj_get_height(lbl[i]) >= need.y;
      if (!fits) clipped++;
      fprintf(stderr, "line %d: box=%dx%d text_needs_h=%d  %s\n", i + 1,
              lv_obj_get_width(lbl[i]), lv_obj_get_height(lbl[i]), need.y,
              fits ? "FITS" : "CLIPPED");
    }
    fprintf(stderr, "VERDICT: %d/%d lines clipped; reachable_by_scroll=%s\n",
            clipped, n,
            lv_obj_has_flag(pl.body, LV_OBJ_FLAG_SCROLLABLE) ? "yes" : "n/a (all visible)");
  } else if (screen == "makepono_fault") {
    // The lost-contact state: no word from the machine -> alarm + STOP stays put.
    pono::build_home(lv_scr_act(), pono::demo_home_idle_model());
    pono::cal_log_show("no word from the machine for 14s", "tap STOP if it does not clear", 5, 40, true, nullptr, nullptr);
  } else {
    pono::build_home(lv_scr_act(), pono::demo_home_model());
  }

  // advance time so animations settle (ocean drift, glow pulse)
  for (int t = 0; t < advance_ms; t += 16) {
    g_tick_ms += 16;
    lv_timer_handler();
  }

  // hide the perf-monitor overlay (sys layer) so it does not blemish captures
  lv_obj_t *sys = lv_disp_get_layer_sys(disp);
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(sys); i++) {
    lv_obj_add_flag(lv_obj_get_child(sys, i), LV_OBJ_FLAG_HIDDEN);
  }
  lv_refr_now(disp);

  if (write_bmp(out) != 0) {
    fprintf(stderr, "failed to write %s\n", out);
    return 1;
  }
  fprintf(stderr, "wrote %s (%s, %dms)\n", out, screen.c_str(), advance_ms);
  return 0;
}
