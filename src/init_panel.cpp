#include "init_panel.h"
#include "utils.h"
#include "state.h"
#include "config.h"
#include "logger.h"
#include "pono_home.h"   // build_boot / boot_set_progress (the shared boot layout)
#include "pono_anim.h"   // pono::busy_hide() (clear a stranded blocking overlay on disconnect)

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace {
// The boot init script (pono-print-boot-joke) urandom-picks one line into
// /run/pono-print-joke for the login banner. We reuse it as the cycle's start
// index so each boot opens on a different joke, then rotate through the book.
std::string read_boot_joke() {
  std::ifstream f("/run/pono-print-joke");
  if (!f.is_open()) return std::string();
  std::string line;
  std::getline(f, line);
  return line;
}

// boot_play_intro ends when the dedication settles (760 ms delay + 640 ms). The
// first handoff waits for it, so a fast connect does not cut the flag mid-rise.
constexpr uint32_t kIntroMs = 1400;
constexpr uint32_t kReadyBeatMs = 260;    // hold on "Ready" at 100% long enough to read
constexpr uint32_t kFadeMs = 280;         // cover fades out over the cockpit
constexpr uint32_t kPollMs = 2000;        // re-ask Klipper's state while the cover is up
constexpr uint32_t kProbeStaleMs = 6000;  // a probe with no reply by now is abandoned
constexpr uint32_t kRestartGraceMs = 15000;
constexpr uint32_t kHandshakeStaleMs = 20000;  // a handshake with no end by now is abandoned

// Klipper's state_message is a paragraph: the cause first, then boilerplate
// ("Once the underlying issue is corrected..."). The cover shows the cause only,
// at most two lines, joined so the label wraps them itself. A Python traceback
// is for the log: after a cause it ends the reason, and with nothing ahead of
// it the reason is the exception line that closes it, not its frames.
std::string klipper_reason(const std::string &m) {
  std::string out;
  int lines = 0;
  bool in_traceback = false;
  size_t pos = 0;
  while (pos < m.size() && lines < 2) {
    size_t nl = m.find('\n', pos);
    const std::string raw = m.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    size_t a = raw.find_first_not_of(" \t\r");
    size_t b = raw.find_last_not_of(" \t\r");
    const std::string line = (a == std::string::npos) ? std::string() : raw.substr(a, b - a + 1);
    if (line.rfind("Traceback (most recent call last)", 0) == 0) {
      if (lines) break;
      in_traceback = true;
    } else if (in_traceback) {
      if (!line.empty() && a == 0) {   // the first unindented line is the exception
        out = line;
        lines = 1;
        in_traceback = false;
      }
    } else if (line.empty()) {
      if (lines) break;              // a blank line after the cause ends it
    } else {
      if (line.rfind("Once the underlying issue", 0) == 0) break;
      if (!out.empty()) out += ' ';
      out += line;
      lines++;
    }
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
  return out;
}
} // namespace

InitPanel::InitPanel(KWebSocketClient &w, MainPanel &mp, std::mutex& l)
  : cont(lv_obj_create(lv_scr_act()))
  , ws(w)
  , main_panel(mp)
  , lv_lock(l)
{
  // Full-screen boot container: the flying Hawaii flag, a cycling island joke,
  // and a real progress bar (build_boot); it owns the glass until Klipper is
  // ready, then fades out over the live cockpit.
  lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(cont, 0, 0);
  lv_obj_set_style_border_width(cont, 0, 0);
  lv_obj_set_style_radius(cont, 0, 0);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

  pono::build_boot(cont, &boot_);
  born_ms_ = lv_tick_get();

  // The fault view's one button. Hidden (so untappable) except in that view.
  if (boot_.action)
    lv_obj_add_event_cb(boot_.action, [](lv_event_t *e) {
      static_cast<InitPanel *>(lv_event_get_user_data(e))->restart_tapped();
    }, LV_EVENT_CLICKED, this);

  // Cycling island jokes: read the device joke book, seed the start from the
  // boot-picked line so each boot opens on a different one, then rotate slowly.
  load_jokes();
  if (boot_.joke && !jokes_.empty())
    lv_label_set_text(boot_.joke, jokes_[joke_idx_].c_str());
  joke_timer_ = lv_timer_create(
      [](lv_timer_t *t) { static_cast<InitPanel *>(t->user_data)->cycle_joke(); },
      9000, this);   // 9s/joke; faster cycled before the line could be read

  poll_timer_ = lv_timer_create(
      [](lv_timer_t *t) { static_cast<InitPanel *>(t->user_data)->poll(); },
      kPollMs, this);

  pono::boot_set_progress(&boot_, 4, "Waiting for Klipper to start...");

  // Wake the screen once: the flag, joke, instruments, and dedication fade and
  // rise in. One-shot; a later disconnect re-shows the settled screen, no replay.
  pono::boot_play_intro(&boot_);
}

InitPanel::~InitPanel() {
  std::lock_guard<std::mutex> lock(lv_lock);  // don't race the render loop on teardown
  if (joke_timer_) { lv_timer_del(joke_timer_); joke_timer_ = nullptr; }
  if (poll_timer_) { lv_timer_del(poll_timer_); poll_timer_ = nullptr; }
  if (cont != NULL) {
    lv_anim_del(cont, nullptr);   // a handoff fade must not fire into a freed panel
    lv_obj_del(cont);             // frees the flag/joke/status/bar children too
    cont = NULL;
  }
}

// Advance the boot progress bar + status from a websocket-thread callback. Takes
// lv_lock itself (the connect callbacks below do not hold it).
void InitPanel::set_stage(int pct, const char *msg) {
  std::lock_guard<std::mutex> lock(lv_lock);
  pono::boot_show_loading(&boot_);
  pono::boot_set_progress(&boot_, pct, msg);
}

void InitPanel::load_jokes() {
  std::ifstream f("/usr/share/pono-print/jokes.txt");
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();   // strip CR
    if (!line.empty()) jokes_.push_back(line);
  }
  if (jokes_.empty()) {
    std::string j = read_boot_joke();
    if (!j.empty()) jokes_.push_back(j);
  }
  // Last-resort line when jokes.txt is missing. It must not explain the brand
  // back to the operator, and it must not tell them to level a bed that has no
  // reachable screws (this machine compensates the tilt in software).
  if (jokes_.empty())
    jokes_.push_back("Went looking for the bed leveling screws. The mesh had already handled it.");

  std::string seed = read_boot_joke();
  if (!seed.empty()) {
    for (size_t i = 0; i < jokes_.size(); i++)
      if (jokes_[i] == seed) { joke_idx_ = i; break; }
  }
}

// Timer callback: runs under lv_lock (lv_timer_handler holds it), so the label
// write is safe against the render loop. Lock-free by contract (no self-lock).
void InitPanel::cycle_joke() {
  if (jokes_.empty() || !boot_.joke) return;
  joke_idx_ = (joke_idx_ + 1) % jokes_.size();
  lv_label_set_text(boot_.joke, jokes_[joke_idx_].c_str());
}

// Act 2, once: the cinematic gives way to real loading. Stop cycling jokes and
// crossfade the joke out for the legit progress bar + live status. Runs on the
// ws thread (connected()), so it takes lv_lock itself; deleting the joke timer
// under the lock is safe (a timer cb cannot be mid-run while we hold it).
void InitPanel::reveal_progress() {
  std::lock_guard<std::mutex> lock(lv_lock);
  if (progress_shown_) return;
  progress_shown_ = true;
  if (joke_timer_) { lv_timer_del(joke_timer_); joke_timer_ = nullptr; }
  pono::boot_reveal_progress(&boot_);
}

// The socket opened, or Klipper announced ready. Either way, ask Klipper what
// state it is in before loading anything: Moonraker answers the object list
// for a shut-down Klipper too, and the old handshake then dropped the cover
// over a machine that could not move. Runs on the ws thread.
void InitPanel::connected(KWebSocketClient &ws) {
  LOG_DEBUG("init panel connected");
  reveal_progress();   // Act 2: real loading begins, phase the progress bar in
  const unsigned epoch = conn_epoch_.load();
  unsigned seq = 0;
  {
    std::lock_guard<std::mutex> lock(lv_lock);
    if (handshaking_) return;   // a ready handshake is already loading this link
    if (cover_up_) {
      pono::boot_show_loading(&boot_);
      pono::boot_set_progress(&boot_, 12, "Checking on Klipper...");
    }
    seq = arm_probe();
  }
  ws.send_jsonrpc("printer.info", [this, epoch, seq](json &j) { this->probe_reply(j, epoch, seq); });
}

// Mark a printer.info probe as sent and give it a number. Only the reply to the
// newest probe counts: two can be out at once (the poll re-asks after
// kProbeStaleMs, and a ready notice asks again), and an older "startup" or
// "shutdown" landing late would otherwise raise the cover over a live cockpit.
// Caller holds lv_lock.
unsigned InitPanel::arm_probe() {
  probe_inflight_ = true;
  probe_sent_ms_ = lv_tick_get();
  return ++probe_seq_;
}

// A printer.info reply (ws thread). Ready starts the handshake exactly once per
// link; every other state is put on the cover in words, and a stopped Klipper
// gets the one button that recovers it.
void InitPanel::probe_reply(json &j, unsigned epoch, unsigned seq) {
  std::string state, message;
  const bool answered = j.contains("result") && j["result"].is_object();
  if (answered) {
    json &r = j["result"];
    if (r.contains("state") && r["state"].is_string()) state = r["state"].get<std::string>();
    if (r.contains("state_message") && r["state_message"].is_string())
      message = r["state_message"].get<std::string>();
  }

  bool go = false;
  {
    std::lock_guard<std::mutex> lock(lv_lock);
    if (seq != probe_seq_) return;   // a newer probe is out; only its answer is current
    probe_inflight_ = false;
    if (epoch != conn_epoch_.load() || handshaking_) return;   // a newer link, or already loading

    if (state == "ready") {
      handshaking_ = true;
      handshake_ms_ = lv_tick_get();
      restart_grace_ = false;
      go = true;
    } else {
      if (!cover_up_) raise_cover();   // not ready: the cockpit cannot act, the cover says why
      const bool grace = in_restart_grace();
      if (!answered) {
        // Moonraker is up and its Klippy host is not (restarting, or not yet started).
        pono::boot_show_loading(&boot_);
        pono::boot_set_progress(&boot_, 8, grace ? "Restarting firmware..." : "Waiting for Klipper to start...");
      } else if (state == "startup") {
        pono::boot_show_loading(&boot_);
        pono::boot_set_progress(&boot_, 16, "Klipper is starting...");
      } else if ((state == "shutdown" || state == "error") && grace) {
        // The restart was sent a moment ago and Klipper has not got to it yet.
        // Either stopped state still answers here until it does, and showing
        // the fault again would invite a second tap on a restart in progress.
        pono::boot_show_loading(&boot_);
        pono::boot_set_progress(&boot_, 8, "Restarting firmware...");
      } else if (state == "shutdown") {
        pono::boot_show_fault(&boot_, "Klipper stopped", klipper_reason(message).c_str());
      } else if (state == "error") {
        pono::boot_show_fault(&boot_, "Klipper can't start", klipper_reason(message).c_str());
      } else {
        pono::boot_show_loading(&boot_);
        pono::boot_set_progress(&boot_, 8, "Waiting for Klipper...");
      }
    }
  }
  if (go) begin_handshake(epoch);
}

// Klipper is ready: load the printer and hand the glass to the cockpit. Runs on
// the ws thread without lv_lock; every reply re-checks the epoch, so a chain
// from a link that has since dropped stops where it is.
void InitPanel::begin_handshake(unsigned epoch) {
  State *state = State::get_instance();
  state->reset();
  set_stage(22, "Reading the printer...");

  ws.send_jsonrpc("printer.objects.list", [this, epoch](json& d) {
    if (epoch != this->conn_epoch_.load()) return;
    State *state = State::get_instance();
    state->set_data("printer_objs", d, "/result");

    this->set_stage(55, "Loading printer state...");

    this->ws.send_jsonrpc("server.files.roots",
        [](json& j) { State::get_instance()->set_data("roots", j, "/result"); });

    this->ws.send_jsonrpc("printer.info",
        [](json& j) { State::get_instance()->set_data("printer_info", j, "/result"); });

    this->main_panel.subscribe();

    // spoolman
    this->ws.send_jsonrpc("server.info", [this](json &j) {
      LOG_DEBUG("server_info {}", j.dump());
      State::get_instance()->set_data("server_info", j, "/result");

      auto &components = j["/result/components"_json_pointer];
      if (components.is_array()) {
        // Element-wise + is_string instead of get<vector<string>>(): a single
        // non-string entry there would otherwise throw out of the ws callback.
        for (auto &c : components) {
          if (c.is_string() && c.template get<std::string>() == "spoolman") {
            this->main_panel.enable_spoolman();
            break;
          }
        }
      }
    });

    auto display_sensors = state->get_display_sensors();
    this->main_panel.create_sensors(display_sensors);

    auto display_fans = state->get_display_fans();
    this->main_panel.create_fans(display_fans);

    auto display_leds = state->get_display_leds();
    this->main_panel.create_leds(display_leds);

    // subscribe to all objects except gcode_macro
    auto objs = d["/result/objects"_json_pointer];
    if (!objs.is_null()) {
      json sub_objs;
      for (auto &obj : objs) {
        if (!obj.is_string()) continue;  // skip a non-string entry rather than throw out of the connect callback
        std::string obj_name = obj.template get<std::string>();
        if (obj_name.rfind("gcode_macro ", 0 ) != 0) {
          sub_objs[obj_name] = nullptr;
        }
      }

      this->set_stage(78, "Subscribing to printer...");

      json subs = {{ "objects", sub_objs }};
      LOG_DEBUG("subscribing to {}", subs.dump());
      this->ws.send_jsonrpc("printer.objects.subscribe", subs, [this, epoch](json &data) {
        if (!data.contains("result")) {
          // Klipper went away between the list and the subscribe. Stand down;
          // the poll asks again and a ready state starts a fresh handshake.
          std::lock_guard<std::mutex> lock(this->lv_lock);
          if (epoch == this->conn_epoch_.load()) this->handshaking_ = false;
          return;
        }
        // A retired chain (a drop, or the poll watchdog) must not load its
        // snapshot into the cockpit. Checked again under the lock below.
        if (epoch != this->conn_epoch_.load()) return;
        State::get_instance()->set_data("printer_state", data, "/result/status");
        this->main_panel.init(data);
        LOG_DEBUG("done init");
        std::lock_guard<std::mutex> lock(this->lv_lock);
        // If the link dropped (and maybe came back) since this connect began, a
        // newer disconnect already re-raised the boot screen. Don't let this
        // stale reply hide it and leave a cockpit sitting over a dead link.
        if (epoch != this->conn_epoch_.load()) return;
        this->handshaking_ = false;
        this->finish_boot();
      });
    } else {
      // Empty/missing objects list (a malformed reply, or an error instead of a
      // result): without this the boot bar sticks at 78% forever with no word.
      // Stand down so the poll retries the whole handshake.
      this->set_stage(55, "Waiting for printer...");
      std::lock_guard<std::mutex> lock(this->lv_lock);
      if (epoch == this->conn_epoch_.load()) this->handshaking_ = false;
    }
  });
}

// Hand the glass to the cockpit. The cover stays opaque for the rest of the
// intro (first boot only) and a beat on "Ready", then fades out over a cockpit
// already sitting on Home. Taps during the fade land on the cover, not on a
// half-seen button. Caller holds lv_lock.
void InitPanel::finish_boot() {
  pono::boot_set_progress(&boot_, 100, "Ready");
  if (!cover_up_) return;   // Klipper re-announced ready under a live cockpit: nothing to reveal
  cover_up_ = false;
  restart_grace_ = false;
  main_panel.set_boot_cover(false);   // cockpit back on Home under the cover
  lv_obj_move_foreground(cont);       // back_to_home raised the cockpit; keep the cover over it

  uint32_t hold = kReadyBeatMs;
  if (!intro_done_) {
    intro_done_ = true;
    const uint32_t el = lv_tick_elaps(born_ms_);
    if (el < kIntroMs) hold += kIntroMs - el;
  }

  lv_anim_del(cont, nullptr);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, cont);
  lv_anim_set_user_data(&a, this);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_time(&a, kFadeMs);
  lv_anim_set_delay(&a, hold);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&a, [](void *o, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(o), (lv_opa_t)v, 0);
  });
  // Runs from lv_timer_handler, so under lv_lock. A disconnect mid-fade deletes
  // the anim (raise_cover) and this never runs.
  lv_anim_set_ready_cb(&a, [](lv_anim_t *an) {
    auto *self = static_cast<InitPanel *>(lv_anim_get_user_data(an));
    lv_obj_add_flag(self->cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(self->cont);
    lv_obj_set_style_opa(self->cont, LV_OPA_COVER, 0);
    self->main_panel.boot_cover_cleared();   // E-STOP and the top layer come back
  });
  lv_anim_start(&a);
}

// Put the cover back over everything, opaque, and close the cockpit's stale
// state under it. Idempotent. Caller holds lv_lock.
void InitPanel::raise_cover() {
  lv_anim_del(cont, nullptr);   // stop a handoff fade in flight (its ready_cb never runs)
  lv_obj_set_style_opa(cont, LV_OPA_COVER, 0);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(cont);
  cover_up_ = true;
  main_panel.set_boot_cover(true);
}

// A Restart firmware tap in the last kRestartGraceMs. Caller holds lv_lock.
bool InitPanel::in_restart_grace() const {
  return restart_grace_ && (int32_t)(restart_grace_until_ - lv_tick_get()) > 0;
}

// While the cover is up, keep asking Klipper where it is, so a shutdown that
// happened while nobody was listening, or a Klipper that came back without
// saying so, still reaches the glass. LVGL timer, so under lv_lock.
void InitPanel::poll() {
  if (!cover_up_) return;
  if (handshaking_) {
    // A reply that never came (Moonraker wedged mid-handshake) must not keep
    // the poll quiet forever: give up on it and ask again.
    if (lv_tick_elaps(handshake_ms_) < kHandshakeStaleMs) return;
    // Retire the stalled chain before asking again: a late reply from it
    // must not run init or drop the cover beside the handshake that replaces it.
    conn_epoch_.fetch_add(1);
    handshaking_ = false;
    probe_inflight_ = false;
  }
  if (!ws.isConnected()) {
    // Act 1 (never linked) keeps its jokes; after that, say which link is down.
    if (progress_shown_) {
      pono::boot_show_loading(&boot_);
      pono::boot_set_progress(&boot_, 4, "Waiting for Moonraker...");
    }
    return;
  }
  if (probe_inflight_ && lv_tick_elaps(probe_sent_ms_) < kProbeStaleMs) return;
  const unsigned seq = arm_probe();
  const unsigned epoch = conn_epoch_.load();
  ws.send_jsonrpc("printer.info", [this, epoch, seq](json &j) { this->probe_reply(j, epoch, seq); });
}

// The fault view's Restart firmware button. No confirm: Klipper is already
// stopped, so this can only bring it back, and it is the one thing to do. LVGL
// event, so under lv_lock.
void InitPanel::restart_tapped() {
  // One restart at a time: a tap queued behind the first must not send another.
  if (!cover_up_ || !ws.isConnected() || in_restart_grace()) return;
  ws.send_jsonrpc("printer.firmware_restart");
  restart_grace_ = true;
  restart_grace_until_ = lv_tick_get() + kRestartGraceMs;
  pono::boot_show_loading(&boot_);
  pono::boot_set_progress(&boot_, 8, "Restarting firmware...");
}

void InitPanel::disconnected(KWebSocketClient &ws) {
  LOG_DEBUG("init panel disconnected");
  conn_epoch_.fetch_add(1);   // invalidate any in-flight connect callbacks from the dead link
  // Socket still open means Klipper dropped (a restart, or a shutdown) and
  // Moonraker is there to ask; closed means Moonraker itself is gone.
  const bool link = ws.isConnected();
  unsigned epoch = 0, seq = 0;
  {
    std::lock_guard<std::mutex> lock(lv_lock);
    // disconnected() runs on the websocket thread; every LVGL write here must
    // hold lv_lock against the render loop (guppyscreen.cpp loop).
    handshaking_ = false;
    raise_cover();   // also clears the overlay tracking so nothing strands on the top layer
    pono::boot_show_loading(&boot_);
    if (link) {
      pono::boot_set_progress(&boot_, 4, in_restart_grace() ? "Restarting firmware..." : "Checking on Klipper...");
      seq = arm_probe();
      epoch = conn_epoch_.load();
    } else {
      probe_inflight_ = false;
      pono::boot_set_progress(&boot_, 4, progress_shown_ ? "Waiting for Moonraker..." : "Waiting for Klipper to start...");
    }
  }
  // Ask now rather than on the next poll, so a shutdown shows its reason at once.
  if (link) ws.send_jsonrpc("printer.info", [this, epoch, seq](json &j) { this->probe_reply(j, epoch, seq); });
}

// CONTRACT: writes the LVGL status label without self-locking. The caller must
// hold GuppyScreen::lv_lock, or run before the render loop starts.
void InitPanel::set_message(const char *message) {
  if (boot_.status) lv_label_set_text(boot_.status, message);
}
