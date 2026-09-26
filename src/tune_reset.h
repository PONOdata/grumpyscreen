#ifndef __TUNE_RESET_H__
#define __TUNE_RESET_H__

#include <cmath>
#include <string>

// Back to print values: the bookkeeping behind the Expert Tune reset chip,
// kept free of LVGL and the socket so tests/test_tune_reset.cpp can drive it.
//
// Speed and flow go back to 100%. PA and fan have no fixed print value, since
// the job's gcode sets them, so their baseline is what the machine held just
// before this screen first changed them. A baseline drops the moment anything
// else moves that value, or the job it came from ends (job(), below): the
// print has taken it back, and a reset would fight the print. Z is never
// offered; a babystep is a correction the operator means to keep.
//
// Every value here is the text a pill shows ("150%", "0.040"), which is also
// what the readback and the request are compared on in main_panel.

namespace pono {

struct TuneReset {
  enum { SPEED, FLOW, ZOFF, PA, FAN, N };  // MainPanel's TUNE_* order, asserted there
  std::string base[N];  // machine value before the first screen change (PA, fan), empty when none
  std::string mine[N];  // what this screen last asked for, empty when nothing is outstanding

  static bool snapshots(int i) { return i == PA || i == FAN; }

  // This screen sent txt for control i while the machine reported `machine`.
  // No baseline is taken when the screen already changed the value without
  // one (the first change came before any readback): the machine would be
  // holding the screen's own value, and resetting to it would undo nothing.
  void request(int i, const std::string &machine, const std::string &txt) {
    if (snapshots(i) && base[i].empty() && mine[i].empty()) base[i] = machine;
    mine[i] = txt;
  }

  // A readback for i with nothing pending. A value this screen did not ask
  // for means the job or another client moved it.
  void settled(int i, const std::string &machine) {
    if (machine != mine[i]) drop(i);
  }

  // The settle timer gave up on i. Still at the baseline means refused or
  // slow, and the baseline holds. Anything else arrived from elsewhere while
  // the request was in flight.
  void gave_up(int i, const std::string &machine) {
    if (machine != mine[i] && machine != base[i]) drop(i);
  }

  void new_job() { for (int i = 0; i < N; i++) drop(i); }
  void drop(int i) { base[i].clear(); mine[i].clear(); }

  // Which job the baselines belong to: its file, and the steady-clock second
  // it started (now minus Klipper's total_duration). The printing edge that
  // calls new_job() is not enough. A reconnect lands straight in a print, and
  // a queued job can follow the last one inside a single status update, so
  // neither shows the machine leaving printing, and a job whose PA happens to
  // equal this screen's last request would keep the old job's baseline.
  //
  // Within one job the start holds still, through a pause too, and a new job
  // moves it by at least the old job's length. It is re-read on every update,
  // so clock slew cannot build up across a long print. Anything that is not a
  // running job with a known start ends the baselines; so does a first
  // sighting, since nothing says they came from this job. Returns true when it
  // dropped them.
  static constexpr double JOB_SLACK = 5.0;  // seconds of delivery jitter between two reads
  bool job(bool running, const std::string &file, double started) {
    bool known = running && !std::isnan(started);
    bool same = known && job_known && file == job_file && std::fabs(started - job_start) <= JOB_SLACK;
    job_known = known;
    job_file = file;
    job_start = started;
    if (!same) new_job();
    return !same;
  }
  bool job_known = false;
  std::string job_file;
  double job_start = 0.0;

  // Bitmask (1 << i) of controls off the print's values, judged on what the
  // machine reports. A control not yet read (empty) is never off.
  int off(const std::string (&machine)[N]) const {
    int m = 0;
    for (int i : {SPEED, FLOW})
      if (!machine[i].empty() && machine[i] != "100%") m |= 1 << i;
    for (int i : {PA, FAN})
      if (!base[i].empty() && !machine[i].empty() && machine[i] != base[i]) m |= 1 << i;
    return m;
  }
};

}  // namespace pono

#endif
