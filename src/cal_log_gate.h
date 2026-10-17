// cal_log_gate.h
//
// When the Make Pono / Full Cal logbook may cover the cockpit. Klipper keeps
// display_status.message until something replaces it, so the last line an
// off-box Full Cal run announced ("Full Cal: T06 out of camera view ...") was
// still standing hours later and raised the logbook, with its STOP, over an
// ordinary print and over a manual home. On-box Make Pono runs as macros; an
// off-box Full Cal run prints jobs it names omega_<test>_<time>.gcode
// (pono_print/calibration_omega_moonraker.py run_test). So "Full Cal" text
// counts only while one of those jobs is on the printer. Kept free of LVGL so
// tests/test_cal_log_gate.cpp can drive it.

#ifndef __PONO_CAL_LOG_GATE_H__
#define __PONO_CAL_LOG_GATE_H__

#include <string>

namespace pono {

// True when print_stats.filename names a job an off-box Full Cal run printed.
// Moonraker may report the file with a directory prefix, so match the base name.
inline bool is_full_cal_job(const std::string &filename) {
  const size_t slash = filename.find_last_of('/');
  const std::string base = (slash == std::string::npos) ? filename : filename.substr(slash + 1);
  return base.rfind("omega_", 0) == 0;
}

// msg: display_status.message. busy: idle_timeout is "Printing" (a print or a
// macro). printing: print_stats is "printing". job: print_stats.filename.
inline bool cal_log_applies(const std::string &msg, bool busy, bool printing, const std::string &job) {
  // No job check needed: every exit of the on-box Make Pono chain clears
  // display_status.message (_PONO_CAL_FINISH before its report runs,
  // _PONO_CAL_HALTED, the _PONO_CAL_ABORTED fallback), and a firmware restart
  // resets it. pono-print-os tools/test_cal_stop_and_resume.sh holds the finish half.
  if (msg.rfind("Make Pono", 0) == 0) return busy || printing;
  if (msg.rfind("Full Cal", 0) == 0) return printing && is_full_cal_job(job);
  return false;
}

}  // namespace pono

#endif  // __PONO_CAL_LOG_GATE_H__
