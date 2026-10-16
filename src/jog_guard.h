// jog_guard.h
//
// Move-screen jog feedback. Klipper refuses a relative move on an axis that is
// not homed, and refuses one that would leave the travel limits, and in both
// cases the only trace was a line in the console: the tap looked dead. These
// two helpers give the glass something to say instead. Kept free of LVGL and
// the socket so tests/test_jog_guard.cpp can drive them.

#ifndef __PONO_JOG_GUARD_H__
#define __PONO_JOG_GUARD_H__

#include <cctype>
#include <string>

namespace pono {

// Why a jog on `axis` ('x', 'y' or 'z') would be refused before it is sent,
// given toolhead.homed_axes; empty when the move can go.
inline std::string jog_refusal(char axis, const std::string &homed_axes) {
  const char lo = (char)std::tolower((unsigned char)axis);
  if (homed_axes.find(lo) != std::string::npos) return {};
  const char up = (char)std::toupper((unsigned char)axis);
  return std::string(1, up) + " is not homed yet, so the printer will not move it. "
         "Tap Home all first, then jog.";
}

// Plain words for a jog Klipper sent back as an error; the raw text when it
// is not one of the two a jog is known to hit.
inline std::string jog_error_text(const std::string &err) {
  if (err.rfind("Must home axis first", 0) == 0)
    return "The printer is not homed, so it refused that move. Tap Home all first, then jog.";
  if (err.rfind("Move out of range", 0) == 0)
    return "That move would go past the end of travel, so the printer refused it. "
           "Pick a smaller step.";
  return err;
}

}  // namespace pono

#endif  // __PONO_JOG_GUARD_H__
