# Pono Calibrate - design

Date: 2026-06-16. Branch: `feat/pono-calibrate` (grumpyscreen).
Status: design + first build. Solo-design (Jack asleep, delegated the creative
calls); the name is flagged for his veto. Nothing is flashed. UI is gated on
Jack's on-glass eyeball; anything that changes printer behavior is gated on his
bench sign-off.

## Concept (one sentence)

Pono Calibrate is the machine making itself pono - "in perfect order, accurate,
correct, well", four of the 43 glosses - and saying so out loud the whole time,
with the porch lamp making its round of the controls.

## Why now (the spine)

The first Full Calibration run, 2026-06-11, was the worst failure Jack has seen:
the host watch died, the touchscreen froze behind a cheerful banner with no stop
while the machine kept running a homing/print cycle driverless, and the only
escape was pulling wall power. (memory:
`project_pono_print_cockpit_watch_2026_06_11.md:91-128`.)

That reframes the whole task. The narration box and the safety interlock are not
two features, they are one: an honest, liveness-aware, always-stoppable UI. The
60fps animation is the only part that is pure craft; even it has a rule (it must
never imply progress it does not have).

## Name

"Full Calibration" -> **"Make Pono"** (recommended). Pono in its first sense is
correct / accurate / in perfect order; calibration literally makes the machine
pono. It is local, it narrates ("making it pono"), it uses the word in its true
sense, and it reads better than the verb "Ponoize". The subtitle stays plain so
the operator still knows what it is: `FULL CALIBRATION`.

- Alternatives for Jack's veto: "The Full Pono" (cheekier), "Ponoize" (his
  throwaway).
- Internal identifiers (`TuneHandles::omega`, the `PONO_CAL_OMEGA` macro) stay;
  they are never on glass. The macro rename is a cross-repo bench-gate item.

## The three pieces

1. **Rename** - `pono_home.cpp:473/475` (card label + subtitle). The on-wire
   progress prefix is the Klipper macro's `SET_DISPLAY_TEXT` ("Full Cal X/N");
   the UI is made to accept both "Make Pono X/N" and "Full Cal X/N" so the macro
   can be renamed later with no hard lockstep.

2. **Orbit (60fps), removed 2026-10-05 at Jack's request** - an amber porch-lamp light orbiting the options cluster on
   the Tune screen. Continuous instrument motion (pono-design allows it, like the
   radar sweep), eased with personality, an unbroken loop. Built frameless: an
   `lv_anim` driving angle -> (x,y) on a small amber glow, so there is no
   `lv_animimg` int8 frame-count cap and nothing baked into flash. It is ambient
   IDENTITY, never a progress signal - the Light Test daylight gate forbids it
   implying work. Real state lives in the narration and a true bar.

3. **Narration "logbook" box (the keystone)** - a bottom strip, NOW / NEXT, fed
   from `display_status.message`, with eased transitions. Liveness-aware: a
   watchdog on the last-update time; if the machine goes quiet past a threshold
   during a run, the box flips to a FAULT state ("no word from the machine - Ns",
   alarm color) and surfaces STOP, instead of a frozen-cheerful banner. A
   prominent, always-reachable STOP fires `CANCEL_PRINT` (the safe graceful stop;
   M112 stays a physical button because its output-kill is not yet bench-verified).
   Real progress from X/N. A done state ("all pono") that retires.

## Light Test verdict: LIGHT

Conditional on the three keystones being built right.

1. Facilitator PASS - the user's own goal, a true machine.
2. Daylight PASS - the orbit is ambient, not fake progress; the state is honest.
3. Symmetry PASS - STOP as easy as Start (inverts the disaster's roach-motel).
4. True PASS - real progress and real liveness; it surfaces lost-contact, never
   fakes it.
5. Stopping PASS - rewards completion ("all pono") and retires.
6. Clear-head PASS - offered to an operator who chose it.

The three keystones (ambient-not-fake orbit, always-reachable STOP,
liveness-aware fault) ARE the inversion of the disaster: asymmetry plus fake
liveness, pointed back at the operator.

## Safety interlock

- **UI refuse-if-printing** (build now; additive, strictly safer): the
  grumpyscreen cal tiles refuse to fire if a job is printing or paused
  (`main_panel.cpp::_sub_tap`), showing a refusal instead of firing G28/motion
  into a live print. This is the device-trigger gap the audit found.
- **Macro refuse-if-printing** (draft; bench-gate): a guard at the top of
  `PONO_CAL_STANDARD`/`PONO_CAL_OMEGA` in pono-print-os.
- **Bed-mesh-purge decoupling** (draft; bench-gate): the standalone Bed Mesh tile
  runs `BED_MESH_CALIBRATE_WITH_WIPE`, which purges filament and bare-G28s with no
  part-on-bed verify; decouple it for filament-free probing.
- The always-reachable STOP is part of the narration box above.

## Build vs gate

- BUILD tonight (UI only, reversible, sim-verified, draft PR, gated on Jack's
  eyeball before any flash): rename, orbit, UI refuse-if-printing guard,
  narration box.
- DRAFT + bench-gate (changes printer behavior, needs Jack at the printer): the
  macro refuse-if-printing guard, the bed-mesh-purge decoupling, and the macro
  emitting "Make Pono X/N: now | next".
- Nothing is flashed tonight.

## Verification

`sim/build_headless.ps1 -Screen tune` for the orbit, and a new `-Screen makepono`
for the narration states (now/next and the lost-contact fault). `tell_check`
clean (pono-design) and `light_check` clean (light-patterns). On-glass eyeball
and the bench guards are Jack's.
