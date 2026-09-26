#ifndef __INIT_PANEL_H__
#define __INIT_PANEL_H__

#include "lvgl/lvgl.h"
#include "websocket_client.h"
#include "main_panel.h"
#include "pono_home.h"   // pono::BootHandles + build_boot/boot_set_progress

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class InitPanel {
 public:
  InitPanel(KWebSocketClient &ws, MainPanel &mp, std::mutex &l);
  ~InitPanel();

  void connected(KWebSocketClient &ws);
  void disconnected(KWebSocketClient &ws);
  void set_message(const char *message);

 private:
  void load_jokes();                          // fill jokes_ from the device joke book
  void cycle_joke();                          // advance to the next island joke (timer cb)
  void reveal_progress();                     // Act 2: phase in progress, stop the joke (once)
  void set_stage(int pct, const char *msg);   // progress + status; takes lv_lock itself

  // Klipper's own state decides what the cover says. Every probe reply lands
  // here (ws thread); ready starts the handshake, anything else is shown.
  void probe_reply(json &j, unsigned epoch, unsigned seq);
  unsigned arm_probe();                       // caller holds lv_lock; the seq the reply must carry
  void begin_handshake(unsigned epoch);       // ws thread, lock NOT held
  void finish_boot();                         // caller holds lv_lock
  void raise_cover();                         // caller holds lv_lock
  void poll();                                // LVGL timer, lock held
  void restart_tapped();                      // LVGL event, lock held
  bool in_restart_grace() const;              // caller holds lv_lock

  lv_obj_t *cont;                 // full-screen boot container
  pono::BootHandles boot_;        // flying flag + joke + status + bar (+ fault reason and action)
  std::vector<std::string> jokes_;
  size_t joke_idx_ = 0;
  lv_timer_t *joke_timer_ = nullptr;  // rotates the joke while we wait
  lv_timer_t *poll_timer_ = nullptr;  // re-asks Klipper's state while the cover is up
  bool progress_shown_ = false;       // Act 2 latch (reveal once, on first connect)

  // Bumped on every disconnect. Written on the ws thread and read by the LVGL
  // thread (poll), so atomic; an in-flight reply compares it before touching
  // the screen.
  std::atomic<unsigned> conn_epoch_{0};

  // Everything below is touched only under lv_lock.
  bool cover_up_ = true;              // the boot cover owns the glass (true at start)
  bool handshaking_ = false;          // a ready-state handshake is in flight for this epoch
  bool intro_done_ = false;           // the first handoff waits out the intro, later ones do not
  bool probe_inflight_ = false;
  unsigned probe_seq_ = 0;            // the newest probe sent; a reply to an older one is stale
  uint32_t probe_sent_ms_ = 0;
  uint32_t handshake_ms_ = 0;         // when the current handshake began (poll's watchdog)
  uint32_t born_ms_ = 0;              // lv_tick at construction, times the intro hold
  uint32_t restart_grace_until_ = 0;  // after a Restart tap, a stale shutdown reply does not re-show the fault
  bool restart_grace_ = false;

  KWebSocketClient &ws;
  MainPanel &main_panel;
  std::mutex &lv_lock;
};

#endif // __INIT_PANEL_H__
