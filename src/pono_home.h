// SPDX-License-Identifier: GPL-3.0-only
// pono_home.h - Pono Print home cockpit builder
//
// The home screen redesign (task #33). Pure LVGL + pono_theme: NO websocket,
// state, Moonraker, fmt or libhv deps, so it links into the headless sim
// (sim/pono_headless.cpp) AND the real app (main_panel.cpp) from one source.
//
// The sim feeds a demo HomeModel; MainPanel feeds live Moonraker values and
// wires the real callbacks. Layout mirrors docs/pono-home-mockup.svg frame 1.
#pragma once
#include "lvgl.h"

namespace pono {

// Plain data the home renders. No LVGL, no app types: trivially constructed
// by the sim and populated from Moonraker status in the real app.
struct HomeModel {
  bool printing;         // a job is loaded (printing OR paused) -> printing layout
  bool paused;           // job paused: primary button shows Resume instead of Pause
  int  progress_pct;     // 0..100
  int  layer;
  int  layer_total;
  const char *job_name;  // "DA OMEGA CUBE"
  const char *material;  // "PA-CF . 0.25 diamond"
  int  nozzle;           // current C
  int  nozzle_set;       // target C
  int  bed;              // current C
  int  bed_set;          // target C
  const char *eta;       // "1:12 left"
};

// The mockup's values, for sim render + first-boot placeholder.
HomeModel demo_home_model();

// Idle-state values (no print running) - the state the printer sits in most.
HomeModel demo_home_idle_model();

// Paused-state values (job loaded, paused) - Resume + Cancel layout.
HomeModel demo_home_paused_model();

// ---- boot / connecting screen ----------------------------------------------
// The screen shown while grumpyscreen waits for Klipper + loads printer state.
// The Hawaii flag flies here (Jack, 2026-06-13), with a cycling island joke, a
// real progress bar the app drives from the live connect stages (NOT a
// decorative fake), and the dedication. Pure layout + theme, so it links into
// the sim too; the app (init_panel) cycles the joke + calls boot_set_progress().
struct BootHandles {
  lv_obj_t *glow = nullptr;       // amber lamp glow behind the flag (the wake)
  lv_obj_t *flag = nullptr;       // flying Hawaii flag (lv_animimg, pono_flag_boot_frames)
  lv_obj_t *wordmark = nullptr;   // retired 2026-06-10 (flag-only boot); stays null
  lv_obj_t *joke = nullptr;       // cycling island joke; app fills + rotates the book
  lv_obj_t *status = nullptr;     // stage line ("Connecting to Moonraker...")
  lv_obj_t *bar = nullptr;        // legit progress bar (0..100, app-driven)
  lv_obj_t *dedication = nullptr; // "For Elio and Io"; the intro fades it in last
  lv_obj_t *spinner = nullptr;    // retired 2026-06-11 (same order); stays null
  lv_obj_t *reason = nullptr;     // fault only: Klipper's own reason, under the headline
  lv_obj_t *action = nullptr;     // fault only: the one recovery button (app wires the tap)
};
void build_boot(lv_obj_t *parent, BootHandles *h = nullptr);

// The loading layout: status + bar + dedication, fault widgets hidden. Idempotent,
// so every stage update can call it to leave a fault view. NULL-safe.
void boot_show_loading(BootHandles *h);

// The fault layout, for a Klipper that stopped (shutdown) or cannot start
// (error). The headline takes the status line, Klipper's own reason sits under
// it, and the recovery button takes the dedication's place; the bar hides,
// because nothing is loading. reason may be empty. NULL-safe.
void boot_show_fault(BootHandles *h, const char *headline, const char *reason);

// Act 1: the one-shot boot wake. The flag, joke, and dedication fade and rise
// in, eased, the dedication landing last; the progress bar + status are held
// dark for Act 2. Called once on first boot (and by the sim to capture it);
// LVGL property anims at 60 fps over the flying flag. NULL-safe on every handle.
void boot_play_intro(BootHandles *h);

// Act 2: phase the legit progress in. The joke crossfades out and the live
// status + progress bar fade in, in the same band, showing what is actively
// loading. Call once when real connect stages begin. NULL-safe.
void boot_reveal_progress(BootHandles *h);

// Set the boot progress bar + status line together (one honest stage update).
// pct 0..100; stage is the short status text. The bar animates forward only: a
// lower value (the cover re-raised after Ready) snaps, so it never runs
// backwards on the glass. NULL-safe on every handle.
void boot_set_progress(BootHandles *h, int pct, const char *stage);

// Live handles into a built home. The app keeps these to update values in
// place (no rebuild) and to attach tap callbacks. NULL-safe: every field may
// be null, callers must guard. The sim ignores it (passes nullptr).
struct HomeHandles {
  // live-updated values
  lv_obj_t *arc = nullptr;         // progress arc (printing layout only; null when idle)
  lv_obj_t *pct = nullptr;         // "47%"
  lv_obj_t *layer = nullptr;       // "layer 84 / 180"
  lv_obj_t *job = nullptr;         // job name
  lv_obj_t *material = nullptr;    // material/profile line (top bar)
  lv_obj_t *eta = nullptr;         // "1:12 left"
  lv_obj_t *nozzle = nullptr;      // nozzle current-temp number
  lv_obj_t *nozzle_set = nullptr;  // nozzle target "/250"
  lv_obj_t *bed = nullptr;         // bed current-temp number
  lv_obj_t *bed_set = nullptr;     // bed target "/60"
  lv_obj_t *state_pill = nullptr;  // PRINTING pill (hide when idle)
  lv_obj_t *state_dot = nullptr;   // pulsing beat inside the pill (anim gated to printing)
  lv_obj_t *stale = nullptr;       // readout-stale flag (printing layout only; hidden while fresh)
  // the dictionary entry (idle layout only; null when printing)
  lv_obj_t *hero = nullptr;        // the entry panel, tappable: advance the gloss
  lv_obj_t *def = nullptr;         // gloss text ("in perfect order")
  lv_obj_t *defpos = nullptr;      // sense tag ("nvs." / "vs.")
  lv_obj_t *defn = nullptr;        // counter ("27 / 43")
  // tappable launchers (app attaches event cbs)
  lv_obj_t *tile_tune = nullptr;
  lv_obj_t *tile_nozzle = nullptr;
  lv_obj_t *tile_bed = nullptr;
  lv_obj_t *btn_pausestop = nullptr;  // Pause (printing) / Resume (paused) / Print (idle)
  lv_obj_t *btn_cancel = nullptr;     // printing/paused layout: abort the job (app confirms)
  lv_obj_t *qa[4] = {nullptr, nullptr, nullptr, nullptr}; // Move/Filament/Files/Fans
  lv_obj_t *tile_more = nullptr;  // 5th nav tile -> More menu
};

// ---- The 43 ------------------------------------------------------------
// Pukui-Elbert senses 1 and 2 of pono, exactly 43 glosses: the theme of every
// Pono surface, one meaning at a time. The idle cockpit hero is the dictionary
// entry: today's gloss (day-seeded), tap to advance, with the NN / 43 counter.
int gloss_count();
int gloss_today_index();              // day-seeded: days-since-epoch % 43
const char *gloss_text(int ix);
const char *gloss_pos(int ix);        // "nvs." (senses 1) or "vs." (sense 2)
// Update the entry's three labels for index ix (wraps modulo). Null-safe.
void home_set_gloss(HomeHandles *h, int ix);

// Build the full 480x272 home cockpit into `parent` (a screen-sized object).
// Returns the arc; if `out` is non-null, fills it with live handles + tap
// targets so the app can update values and wire callbacks. Sim passes nullptr.
lv_obj_t *build_home(lv_obj_t *parent, const HomeModel &m, HomeHandles *out = nullptr);

// Persistent full-kill E-STOP. Build on lv_layer_top() so it rides above the
// cockpit and every sub-screen and survives rebuild_home(). Returns the button;
// the app wires the tap to a confirm -> printer.emergency_stop.
lv_obj_t *build_estop(lv_obj_t *parent);

// Start/stop the small "alive" pulse on the PRINTING pill's dot. Call only on
// the idle<->printing transition; a per-frame restart would stutter the beat.
// Keeps the always-on idle dashboard at zero animation cost.
void set_state_pulse(lv_obj_t *dot, bool on);

// Show or hide the readout-stale flag (instrument-glass law: stale flags
// itself). age_s >= 0 shows "READOUT STALE Ns" over the ring; negative
// hides it. Null-safe; a no-op on the idle layout, which has no flag.
void home_set_stale(HomeHandles *h, int age_s);

// ---- Native sub-screen handles (the app wires actions + live values) ----
struct MoveHandles {
  lv_obj_t *back = nullptr, *pos = nullptr;
  lv_obj_t *xplus = nullptr, *xminus = nullptr, *yplus = nullptr, *yminus = nullptr;
  lv_obj_t *zplus = nullptr, *zminus = nullptr;
  lv_obj_t *home_xy = nullptr, *home_all = nullptr, *motors_off = nullptr;
  lv_obj_t *step[4] = {nullptr, nullptr, nullptr, nullptr};  // 0.1 / 1 / 10 / 100 mm
};

// Build the native Move (jog) screen: XY cross, Z jog, step selector, home/off,
// and a live position readout. The app wires taps to relative moves + homing.
void build_move(lv_obj_t *parent, MoveHandles *h = nullptr);

struct FilamentHandles {
  lv_obj_t *back = nullptr, *temp = nullptr;
  lv_obj_t *load = nullptr, *unload = nullptr, *extrude = nullptr, *retract = nullptr;
  lv_obj_t *preset[3] = {nullptr, nullptr, nullptr};  // PLA / PETG / PA-CF: material select + preheat
  lv_obj_t *cooldown = nullptr;                       // "Off" segment: heaters off
  lv_obj_t *len_slider = nullptr, *len_val = nullptr; // load length slider + live "N mm" readout
};
void build_filament(lv_obj_t *parent, FilamentHandles *h = nullptr);

struct TempsHandles {
  lv_obj_t *back = nullptr;
  lv_obj_t *nz_cur = nullptr, *nz_tgt = nullptr, *bd_cur = nullptr, *bd_tgt = nullptr;
  lv_obj_t *nz_preset[3] = {nullptr, nullptr, nullptr}, *nz_off = nullptr;
  lv_obj_t *bd_preset[3] = {nullptr, nullptr, nullptr}, *bd_off = nullptr;
  lv_obj_t *nz_minus = nullptr, *nz_plus = nullptr;  // manual -/+ step (nozzle)
  lv_obj_t *bd_minus = nullptr, *bd_plus = nullptr;  // manual -/+ step (bed)
};
void build_temps(lv_obj_t *parent, TempsHandles *h = nullptr);

// More menu: the 5th cockpit tile. Lists secondary tools (Wi-Fi, Bed Mesh,
// Expert Tune, Lights, System, Power) as tappable rows. The app wires each
// row to its action.
struct MoreHandles {
  lv_obj_t *back = nullptr;
  lv_obj_t *wifi = nullptr, *expert = nullptr, *mesh = nullptr, *led = nullptr,
           *system = nullptr, *power = nullptr;
};
void build_more(lv_obj_t *parent, MoreHandles *h = nullptr);

struct FansHandles {
  lv_obj_t *back = nullptr;
  // One row per fan the Centauri exposes. Index: 0 part-cooling (fan),
  // 1 model fan, 2 box fan (all user-settable -> slider); 3 mainboard temp
  // fan, 4 hotend heat-break fan (Klipper-managed -> AUTO pill, no slider).
  lv_obj_t *val[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};     // live % label
  lv_obj_t *slider[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};  // null for the auto fans
};
void build_fans(lv_obj_t *parent, FansHandles *h = nullptr);

struct FilesHandles {
  lv_obj_t *back = nullptr, *list = nullptr;
};
void build_files(lv_obj_t *parent, FilesHandles *h = nullptr);
// Append a file row to the Files list (the app populates from Moonraker).
void files_add_row(lv_obj_t *list, const char *name, const char *meta);
// Apply async per-file metadata: swap glyph -> thumbnail + refresh the meta line.
void files_apply_meta(lv_obj_t *row, const char *thumb_path, int zoom, const char *meta);

struct TuneHandles {
  lv_obj_t *back = nullptr, *standard = nullptr, *omega = nullptr;
  lv_obj_t *speed = nullptr, *speed_val = nullptr;
  lv_obj_t *cals[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};  // Bed Mesh/PA/Flow/Shaper/Z-Offset
  lv_obj_t *orbit = nullptr;  // the Make Pono porch-lamp orbit (ambient, 60fps)
};

// Build the Tune screen into `parent`: the two calibrate tiers (Standard +
// the enhanced OMEGA), a grid of individual calibrations (bed mesh, pressure
// advance, flow, input shaper, z-offset), and a live speed slider. Pure LVGL
// + theme; the real app wires taps + the slider to Moonraker.
void build_tune(lv_obj_t *parent, TuneHandles *h = nullptr);

// Expert Tune: the deep LIVE printer-tuning surface (the slicer-geometry
// settings live in the AIO Orca panel; the printer can only tune what Klipper
// controls live). Every value pill is keypad-tappable; speed/flow/fan also
// carry quick presets, z-offset carries babystep -/+ with a choice of step
// size. A header chip returns speed, flow, PA and fan to the print's values
// (tune_reset.h). The app wires each to its Klipper command and reflects live
// status back into the pills.
struct SettingsHandles {
  lv_obj_t *back = nullptr;
  lv_obj_t *speed = nullptr, *flow = nullptr, *zoff = nullptr, *pa = nullptr, *fan = nullptr;
  lv_obj_t *speed_p[3] = {nullptr, nullptr, nullptr};   // 50 / 100 / 150 %
  lv_obj_t *flow_p[3]  = {nullptr, nullptr, nullptr};   // 95 / 100 / 105 %
  lv_obj_t *fan_p[3]   = {nullptr, nullptr, nullptr};   // Off / 50 / Full
  lv_obj_t *zoff_minus = nullptr, *zoff_plus = nullptr; // -/+ one babystep
  lv_obj_t *zstep[3]   = {nullptr, nullptr, nullptr};   // babystep size 0.005 / 0.010 / 0.025 mm
  lv_obj_t *speed_minus = nullptr, *speed_plus = nullptr; // -/+ 5 % speed
  lv_obj_t *melt = nullptr;                              // live melt rate readout, mm3/s
  lv_obj_t *reset = nullptr;                             // header chip: back to print values (hidden unless off)
};
void build_settings(lv_obj_t *parent, SettingsHandles *h = nullptr);

// Update the value text on a value pill. pill_set shows a value the machine
// reported; pill_pending shows one that was only sent, dimmed, until the
// readback confirms it.
void pill_set(lv_obj_t *pill, const char *txt);
void pill_pending(lv_obj_t *pill, const char *txt);

// Bed mesh heatmap: a colored grid of the probed Z deviation. The app feeds
// the live probed_matrix from Moonraker; the sim shows a demo surface.
struct MeshHandles {
  lv_obj_t *back = nullptr;
  lv_obj_t *grid = nullptr;     // heatmap cell container (app re-renders into it)
  lv_obj_t *profile = nullptr;  // active profile name
  lv_obj_t *range = nullptr;    // "min .. max mm" spread
};
void build_mesh(lv_obj_t *parent, MeshHandles *h = nullptr);

// B9 friendly unofficial-firmware copy (Jack, 2026-07-08: signed-only
// posture, refuse nicely, wish them luck; FEL/UART stay the sanctioned
// tinkerer path). Single source for every surface in this repo. Draft copy,
// Jack redlines later - kUnofficialSwuRefusedNotice must stay byte-identical
// to PONO_UNOFFICIAL_NOTICE in pono-print-os
// (meta-opencentauri/recipes-data/update-scripts/files/update-pono-print).
// The draft's em dashes are rendered as spaced hyphens: the Plex faces are
// built ASCII-only (tools/regen_pono_fonts.sh), so U+2014 draws a tofu box
// on glass (verified on the headless render).
inline constexpr const char *kUnofficialBuildNotice =
    "Unofficial build - good luck out there. This isn't a Pono-signed image, "
    "so updates and support work differently. FEL/UART got you here; it'll "
    "get you back.";
inline constexpr const char *kUnofficialSwuRefusedNotice =
    "This build isn't signed by Pono, so it won't install over the updater. "
    "If you're rolling your own - good luck, have fun! Just know it's not an "
    "official Pono Print build, and unofficial builds are yours to support. "
    "(The serial console is your friend.)";

// Firmware-integrity badge state (B9). The wire values in /run/pono-integrity
// stay "signed" / "modified" / "unverified" (images already in the field keep
// working); this maps them to the UI's three states. Fail closed: anything
// absent, unreadable, or unrecognized is Unknown -- never official.
enum class IntegrityState { OfficialSigned, Unofficial, Unknown };
IntegrityState integrity_state_from_wire(const char *state);

// System info screen: firmware version, update lane, network, uptime (app
// fills the values). The update row carries a live status ("up to date" /
// "alpha.174 available") and an Install chip the app reveals when a newer
// build is published on the firmware host.
struct SystemHandles {
  lv_obj_t *back = nullptr, *version = nullptr, *ip = nullptr, *host = nullptr, *uptime = nullptr, *mcu = nullptr;
  lv_obj_t *update_status = nullptr;  // "checking..." / "up to date" / "alpha.NNN available"
  lv_obj_t *btn_install = nullptr;    // lamp chip, hidden until an update is available
  lv_obj_t *integrity = nullptr;      // small badge on the FIRMWARE row: official-signed / UNOFFICIAL / unknown
};
void build_system(lv_obj_t *parent, SystemHandles *h = nullptr);
// Set the firmware-integrity badge: OfficialSigned -> quiet phosphor,
// Unofficial -> amber UNOFFICIAL, Unknown -> dim unknown.
void system_set_integrity(SystemHandles *h, IntegrityState state);

// Power screen: restart Klipper, restart firmware, reboot, shutdown.
struct PowerHandles {
  lv_obj_t *back = nullptr, *restart_klipper = nullptr, *restart_fw = nullptr, *reboot = nullptr, *shutdown = nullptr;
};
void build_power(lv_obj_t *parent, PowerHandles *h = nullptr);

// Lights screen: case + hotend LED brightness (Off / 50% / Full).
struct LightsHandles {
  lv_obj_t *back = nullptr;
  lv_obj_t *case_off = nullptr, *case_50 = nullptr, *case_full = nullptr;
  lv_obj_t *hot_off = nullptr, *hot_50 = nullptr, *hot_full = nullptr;
};
void build_lights(lv_obj_t *parent, LightsHandles *h = nullptr);

// Highlight one button in a segmented row (e.g. Off/50%/Full); dim the others.
void seg_highlight(lv_obj_t *const *btns, int n, int active);

// Modal confirm dialog (scrim + card + message + Cancel/Confirm). Built once
// onto lv_layer_top by the app; shown before destructive actions (reboot,
// shutdown, restart). The app sets the message and wires the buttons.
struct ConfirmHandles {
  lv_obj_t *scrim = nullptr, *card = nullptr, *msg = nullptr,
           *cancel = nullptr, *confirm = nullptr;
};
void build_confirm(lv_obj_t *parent, ConfirmHandles *h = nullptr);

// Informational notice modal (scrim + card + paragraph + OK). Like the
// confirm dialog but sized for paragraph-length copy (the B9 unofficial-
// build notice) and with a single dismiss. Built once onto lv_layer_top by
// the app; the app sets the message and wires OK + scrim to hide it.
struct NoticeHandles {
  lv_obj_t *scrim = nullptr, *card = nullptr, *msg = nullptr, *ok = nullptr;
};
void build_notice(lv_obj_t *parent, NoticeHandles *h = nullptr);

// Re-render the heatmap from a row-major z matrix (rows x cols, mm), color-
// mapped across [zmin,zmax]. Front row drawn at the bottom. Safe to call live.
void mesh_render(lv_obj_t *grid, const float *z, int rows, int cols, float zmin, float zmax);

} // namespace pono
