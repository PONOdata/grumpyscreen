// test_cal_log_gate.cpp
//
// Regression gate for when the Make Pono / Full Cal logbook may cover the
// cockpit (src/cal_log_gate.h). The failure it pins: a Full Cal line left in
// display_status.message by an off-box run raised the logbook over an
// ordinary print (the 2026-10-15 KSR test part) and over a manual home. The
// cases keep both halves: on-box Make Pono narration and a real Full Cal job
// still show it, and a leftover Full Cal line on anything else does not.

#include <iostream>
#include <string>
#include "cal_log_gate.h"

using pono::cal_log_applies;
using pono::is_full_cal_job;

static int failures = 0;

static void check(bool ok, const std::string &label) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << label << "\n";
    if (!ok) failures++;
}

int main() {
    const std::string stale = "Full Cal: T06 out of camera view (125 mm down) - manual revi";
    const std::string step = "Full Cal 3/9: T06 Overhang | next T07 Bridge";
    const std::string onbox = "Make Pono 2/3: Bed mesh | Input shaper";
    const std::string ksr = "ksr_fdmtest_v4_PA_7h40m.gcode";
    const std::string omega = "omega_T06_1760500000.gcode";

    std::cout << "is_full_cal_job\n";
    check(is_full_cal_job(omega), "omega_ job matches");
    check(is_full_cal_job("subdir/" + omega), "omega_ job under a directory matches");
    check(!is_full_cal_job(ksr), "an ordinary job does not match");
    check(!is_full_cal_job(""), "no job does not match");
    check(!is_full_cal_job("my_omega_T06.gcode"), "omega_ only counts at the start of the name");

    std::cout << "cal_log_applies\n";
    check(!cal_log_applies(stale, true, true, ksr), "leftover Full Cal line over an ordinary print: hidden");
    check(!cal_log_applies(stale, true, false, ""), "leftover Full Cal line during a manual home: hidden");
    check(!cal_log_applies(stale, false, false, ""), "leftover Full Cal line on an idle machine: hidden");
    check(cal_log_applies(step, true, true, omega), "Full Cal step during its own omega_ job: shown");
    check(cal_log_applies(stale, true, true, omega), "Full Cal status during an omega_ job: shown");
    check(!cal_log_applies(step, false, false, omega), "Full Cal step after the omega_ job ended: hidden");
    check(cal_log_applies(onbox, true, false, ""), "on-box Make Pono macro run: shown");
    check(cal_log_applies(onbox, false, true, ksr), "Make Pono while print_stats prints: shown (unchanged)");
    check(!cal_log_applies(onbox, false, false, ""), "Make Pono on an idle machine: hidden");
    check(!cal_log_applies("Calibrating: bed mesh", true, false, ""), "Calibrating text is the busy overlay's, not the logbook's");
    check(!cal_log_applies("", true, true, omega), "empty message: hidden");

    std::cout << (failures ? "FAILED " : "OK ") << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
