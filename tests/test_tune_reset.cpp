// test_tune_reset.cpp
//
// Regression gate for the Expert Tune "back to print values" chip
// (src/tune_reset.h). The chip offers to undo on-screen tuning mid-print, so
// its two ways to fail are both bad: offering a reset that fights the print's
// own values (the job moved fan or PA itself), or hiding one that is owed. The
// cases below are written the way main_panel drives the struct: request() on a
// tap, settled() on a readback with nothing pending, gave_up() when the settle
// timer expires, new_job() on a fresh print, job() on every print_stats read.

#include <cmath>
#include <iostream>
#include <string>
#include "tune_reset.h"

using pono::TuneReset;

static int failures = 0;

static void check(bool ok, const std::string &label) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << label << "\n";
    if (!ok) failures++;
}

// Machine readback in TUNE_* order, as the pills format it.
struct Machine {
    std::string v[TuneReset::N] = {"100%", "100%", "0.000", "0.040", "0%"};
};

static bool off(const TuneReset &r, const Machine &m, int i) { return r.off(m.v) & (1 << i); }

int main() {
    std::cout << "tune_reset\n";

    { TuneReset r; Machine m;
      check(r.off(m.v) == 0, "an untouched machine at print values shows no chip"); }

    { TuneReset r; Machine m; m.v[TuneReset::SPEED] = "150%";
      check(off(r, m, TuneReset::SPEED), "speed left at 150% from an earlier job is off with no screen change"); }

    { TuneReset r; Machine m; m.v[TuneReset::FLOW] = "95%";
      check(off(r, m, TuneReset::FLOW), "flow away from 100% is off"); }

    { TuneReset r; Machine m; m.v[TuneReset::SPEED] = ""; m.v[TuneReset::FLOW] = "";
      check(r.off(m.v) == 0, "speed and flow not yet read are never off"); }

    { TuneReset r; Machine m;
      r.request(TuneReset::PA, m.v[TuneReset::PA], "0.060");
      m.v[TuneReset::PA] = "0.060"; r.settled(TuneReset::PA, "0.060");
      check(off(r, m, TuneReset::PA), "PA changed on screen and confirmed is off");
      check(r.base[TuneReset::PA] == "0.040", "PA baseline is the value held before the change");

      m.v[TuneReset::PA] = "0.050"; r.settled(TuneReset::PA, "0.050");
      check(!off(r, m, TuneReset::PA), "PA moved by the job afterwards drops the baseline");
      check(r.base[TuneReset::PA].empty(), "that drop clears the baseline, not just the bit");

      r.request(TuneReset::PA, m.v[TuneReset::PA], "0.070");
      check(r.base[TuneReset::PA] == "0.050", "the next screen change snapshots what the job set"); }

    { TuneReset r; Machine m;
      r.request(TuneReset::FAN, "0%", "50%");
      r.gave_up(TuneReset::FAN, "0%");
      check(!off(r, m, TuneReset::FAN), "a refused fan request leaves nothing to reset");
      check(r.base[TuneReset::FAN] == "0%", "a refused request keeps the baseline");
      r.request(TuneReset::FAN, "0%", "100%");
      check(r.base[TuneReset::FAN] == "0%", "a retry after a refusal keeps the first baseline"); }

    { TuneReset r; Machine m;
      r.request(TuneReset::FAN, "0%", "50%");
      r.gave_up(TuneReset::FAN, "0%");            // readback slower than the settle timer
      m.v[TuneReset::FAN] = "50%"; r.settled(TuneReset::FAN, "50%");
      check(off(r, m, TuneReset::FAN), "a readback that lands after the timer still counts as the screen's"); }

    { TuneReset r; Machine m;
      r.request(TuneReset::FAN, "0%", "50%");
      m.v[TuneReset::FAN] = "100%"; r.gave_up(TuneReset::FAN, "100%");  // the job's M106 won the race
      check(!off(r, m, TuneReset::FAN), "a job value that lands while the request is in flight takes the fan back"); }

    { TuneReset r; Machine m;
      r.request(TuneReset::PA, "0.040", "0.060");
      m.v[TuneReset::PA] = "0.060"; r.settled(TuneReset::PA, "0.060");
      r.new_job();
      check(!off(r, m, TuneReset::PA), "a new job drops the last job's PA baseline"); }

    { TuneReset r; Machine m;
      r.request(TuneReset::PA, "", "0.060");      // tapped before any readback arrived
      m.v[TuneReset::PA] = "0.060"; r.settled(TuneReset::PA, "0.060");
      r.request(TuneReset::PA, "0.060", "0.080");
      check(r.base[TuneReset::PA].empty(), "no baseline is taken from the screen's own earlier value");
      m.v[TuneReset::PA] = "0.080"; r.settled(TuneReset::PA, "0.080");
      check(!off(r, m, TuneReset::PA), "with no true baseline PA is never offered"); }

    { TuneReset r; Machine m;
      r.request(TuneReset::PA, "0.040", "0.060");
      m.v[TuneReset::PA] = "0.060"; r.settled(TuneReset::PA, "0.060");
      r.request(TuneReset::PA, "0.060", r.base[TuneReset::PA]);    // the chip's own tap
      m.v[TuneReset::PA] = "0.040"; r.settled(TuneReset::PA, "0.040");
      check(r.off(m.v) == 0, "after the chip's reset lands nothing is off");
      check(r.base[TuneReset::PA] == "0.040", "the reset keeps the baseline for the next change"); }

    { TuneReset r; Machine m;
      r.request(TuneReset::ZOFF, "0.000", "0.010");
      m.v[TuneReset::ZOFF] = "0.010"; r.settled(TuneReset::ZOFF, "0.010");
      check(r.off(m.v) == 0, "a Z babystep is never offered for reset");
      check(r.base[TuneReset::ZOFF].empty(), "Z takes no baseline"); }

    // job(): the file plus the steady-clock second the print started. These
    // model the updates main_panel feeds it, init and delta alike.
    const double NaN = std::nan("");
    auto tuned_pa = [](TuneReset &r, Machine &m) {  // PA 0.040 -> 0.060 on screen, confirmed
        r.request(TuneReset::PA, m.v[TuneReset::PA], "0.060");
        m.v[TuneReset::PA] = "0.060"; r.settled(TuneReset::PA, "0.060");
    };

    { TuneReset r; Machine m;
      r.job(true, "a.gcode", 1000.0);
      tuned_pa(r, m);
      // The socket drops, a.gcode finishes, the same file prints again, and the
      // new run sets PA 0.060 itself. The reconnect's full status says printing,
      // so the printing edge never fires; the readback matches the last request.
      r.settled(TuneReset::PA, "0.060");
      check(off(r, m, TuneReset::PA), "precondition: the settled readback alone keeps the old baseline");
      check(r.job(true, "a.gcode", 1000.0 + 3600.0), "a reconnect into a new run of the same file is a new job");
      check(!off(r, m, TuneReset::PA), "and the last run's PA baseline is not offered on it");
      check(r.base[TuneReset::PA].empty() && r.mine[TuneReset::PA].empty(), "both halves of the baseline go"); }

    { TuneReset r; Machine m;
      r.job(true, "a.gcode", 1000.0);
      tuned_pa(r, m);
      check(r.job(true, "b.gcode", 1000.0), "another file is another job, even at the same start");
      check(!off(r, m, TuneReset::PA), "so its PA is not offered either"); }

    { TuneReset r; Machine m;
      r.job(true, "a.gcode", 1000.0);
      tuned_pa(r, m);
      check(!r.job(true, "a.gcode", 1000.0 + 0.8), "a later read of the same job, 0.8 s of jitter, keeps it");
      check(!r.job(true, "a.gcode", 1000.0), "jitter the other way keeps it too");
      check(!r.job(true, "a.gcode", 1000.0 + TuneReset::JOB_SLACK), "jitter at the slack still keeps it");
      check(off(r, m, TuneReset::PA), "and the chip still owes the PA reset"); }

    { TuneReset r; Machine m;
      r.job(true, "a.gcode", 1000.0);
      tuned_pa(r, m);
      check(r.job(true, "a.gcode", 1000.0 + TuneReset::JOB_SLACK + 0.5), "a start past the slack is a new job");
      check(!off(r, m, TuneReset::PA), "and drops the baseline"); }

    { TuneReset r; Machine m;
      r.job(true, "a.gcode", 1000.0);
      tuned_pa(r, m);
      check(r.job(true, "a.gcode", 1000.0 - 600.0), "a start that moved back is not this job either"); }

    { TuneReset r; Machine m;
      r.job(true, "a.gcode", 1000.0);
      tuned_pa(r, m);
      bool kept = true;
      for (int k = 1; k <= 30; k++) kept = !r.job(true, "a.gcode", 1000.0 + k) && kept;  // 1 s of slew per read
      check(kept && off(r, m, TuneReset::PA), "slow clock slew across many reads never ends the job"); }

    { TuneReset r; Machine m;
      r.job(true, "a.gcode", 1000.0);
      tuned_pa(r, m);
      check(!r.job(true, "a.gcode", 1000.0), "a pause is the same job (main_panel passes running for paused)");
      check(r.job(false, "a.gcode", 1000.0), "the job ending drops its baselines");
      check(!off(r, m, TuneReset::PA), "so a finished print offers no PA reset"); }

    { TuneReset r; Machine m;
      r.job(true, "a.gcode", 1000.0);
      tuned_pa(r, m);
      check(r.job(true, "a.gcode", NaN), "a running job with no start read cannot be matched, so it drops");
      check(!off(r, m, TuneReset::PA), "the baseline goes with it"); }

    { TuneReset r; Machine m;
      tuned_pa(r, m);
      check(r.job(true, "a.gcode", 1000.0), "a first sighting cannot vouch for a baseline taken before it");
      check(!off(r, m, TuneReset::PA), "so it drops"); }

    { TuneReset r; Machine m;
      r.job(false, "a.gcode", 1000.0);             // a read that was not a running job
      tuned_pa(r, m);
      check(r.job(true, "a.gcode", 1000.0), "a read that was not running never vouches for the next one"); }

    { TuneReset r; Machine m;
      r.job(false, "", 0.0);                       // boot while idle
      r.job(true, "a.gcode", 1000.0);              // the print starts
      tuned_pa(r, m);
      check(!r.job(true, "a.gcode", 1001.0), "a job first seen before the tap keeps the tap's baseline");
      check(off(r, m, TuneReset::PA), "and the chip offers it"); }

    { TuneReset r; Machine m;
      r.job(true, "a.gcode", 1000.0);
      tuned_pa(r, m);
      r.job(true, "a.gcode", 5000.0);              // reconnect into a new run
      m.v[TuneReset::PA] = "0.030";
      r.request(TuneReset::PA, "0.030", "0.050");
      check(r.base[TuneReset::PA] == "0.030", "the new job snapshots its own PA");
      check(!r.job(true, "a.gcode", 5000.5), "and its next read is the same job");
      m.v[TuneReset::PA] = "0.050"; r.settled(TuneReset::PA, "0.050");
      check(off(r, m, TuneReset::PA), "so the new job's reset is offered"); }

    std::cout << (failures ? "FAILED " : "ok ") << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
