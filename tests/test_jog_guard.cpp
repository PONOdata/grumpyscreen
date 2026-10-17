// test_jog_guard.cpp
//
// Regression gate for the Move-screen jog feedback (src/jog_guard.h). A jog
// tap on an unhomed axis used to dispatch, Klipper refused it, and the glass
// showed nothing (three Z+100 taps on 2026-10-15 went that way). The cases
// below hold both halves: the pre-send refusal names the right axis and stays
// quiet once that axis is homed, and the two errors a jog is known to hit come
// back in plain words while anything else passes through untouched.

#include <iostream>
#include <string>
#include "jog_guard.h"

using pono::jog_error_text;
using pono::jog_refusal;

static int failures = 0;

static void check(bool ok, const std::string &label) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << label << "\n";
    if (!ok) failures++;
}

int main() {
    std::cout << "jog_refusal\n";
    check(!jog_refusal('z', "").empty(), "nothing homed: Z refused");
    check(jog_refusal('z', "").rfind("Z is not homed", 0) == 0, "refusal names the axis in capitals");
    check(jog_refusal('x', "xyz").empty(), "all homed: X goes");
    check(jog_refusal('z', "xyz").empty(), "all homed: Z goes");
    check(!jog_refusal('z', "xy").empty(), "X/Y homed only: Z still refused");
    check(jog_refusal('x', "yz").rfind("X is not homed", 0) == 0, "Y/Z homed only: X refused and named");
    check(jog_refusal('y', "xy").empty(), "X/Y homed only: Y goes");
    check(jog_refusal('Z', "xyz").empty(), "upper-case axis matches lower-case homed_axes");
    check(jog_refusal('X', "").rfind("X is not homed", 0) == 0, "upper-case axis still named");

    std::cout << "jog_error_text\n";
    const std::string unhomed = jog_error_text("Must home axis first: 0.000 0.000 100.000 [0.000]");
    check(unhomed.find("not homed") != std::string::npos, "Must home axis first -> plain words");
    check(unhomed.find("0.000") == std::string::npos, "coordinates dropped from the unhomed text");
    const std::string range = jog_error_text("Move out of range: 128.000 128.000 356.000 [0.000]");
    check(range.find("smaller step") != std::string::npos, "Move out of range -> plain words");
    const std::string other = "Unknown command:\"G1X\"";
    check(jog_error_text(other) == other, "an unrecognised error passes through unchanged");
    check(jog_error_text("").empty(), "empty stays empty");

    std::cout << (failures ? "FAILED " : "OK ") << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
