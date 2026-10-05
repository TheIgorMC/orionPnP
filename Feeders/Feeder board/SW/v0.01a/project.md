# v0.01a

First MAJOR-version release of the feeder firmware — the stepping stone
going forward, not another bench-iteration alpha/beta. Forked from
`beta1` at the commit where `PIN_5V_READY`/`PIN_485_RELAY` were
corrected (they'd been assigned backwards — PE3/A7 is the relay drive,
not the 5V-rail sense input; that's on A2/PC2 instead).

For the full design history behind everything this inherits — the
addressing scheme, tape-zero/distance-based motion, the RS485 protocol,
AT24CS02 serial/hardware-identity storage, IMON/5V_READY hand
calibration, the boot-time homing gate, all of it — see
`beta1/project.md`. This file only covers what's different in v0.01a
itself.

## What's different from beta1

1. **Bench-only auto-run test aids are disabled.** `runDebugSelfTest()`
   (relay ×2/ext LED toggle + IMON/5V readout, previously run
   unconditionally on every boot) and `runRelayButtonTest()` (hold
   SW1/SW2 at power-up to hijack the relay for manual toggling) are
   commented out of their call sites in `setup()`, not deleted — both
   functions are still defined and still reachable on demand:
   `runDebugSelfTest()` via the `SELFTEST` debug command, the relay via
   `RELAY ON`/`OFF`. A stepping-stone release shouldn't have boot
   behavior that changes based on whether a button happens to be held,
   or that clicks a relay and flashes LEDs on every single power-up
   regardless of whether anyone's bench-testing it. Real diagnostics
   stay available; automatic bench theater doesn't.
2. **`showStartupLedSequence()`'s red/green/blue splash is disabled
   too**, same reasoning as item 1 even though it's not a debug command
   in its own right — an arbitrary color sequence at boot that doesn't
   reflect any real status is still "boot behavior that doesn't mean
   anything." Replaced with solid yellow (booting, not ready yet) held
   from early in `setup()` until `loop()`'s first iteration overwrites
   it with the real magnet-detect green/red — so the RGB now only ever
   shows either "not ready" or true current status, nothing decorative.
3. **Relay engagement is untouched** — still gated entirely behind
   `waitFor5vStableAndEngageRelay()`, which only engages once
   `PIN_5V_READY` has read stable for `RELAY_READY_STABLE_MS`. This was
   already the real safety behavior in beta1; nothing about the actual
   gating logic changed for v0.01a, only the bench-aid noise around it
   (including the yellow-LED change above — that's purely cosmetic,
   the relay was never toggled by the LED sequence itself).
4. **`PIN_5V_READY`'s divider is still not correct on real hardware** as
   of this fork — the PCB needs the 4.7k(rail)/1k(GND) resistor swap
   beta1 found was backwards (see `beta1/project.md`, "Open questions").
   Kept the firmware logic exactly as beta1 had it (internal-1.1V-
   reference read, calibration point, stability check) rather than
   stripping it out — the code is correct, it's just waiting on the
   matching hardware fix to actually read anything meaningful.

5. **Status RGB now means something.** Yellow = booting, **blue** =
   ready (magnet detected, no driver fault), **red** = error (no magnet
   or DRV8833 fault), **purple** = a motor is moving (feed move, duty
   calibration/homing, or peel), white = `IDENTIFY`. The LED is only
   rewritten when the color actually changes.
6. **Peel motor can run on its own** (to tension the cover tape):
   - **SW2** now runs it forward *for as long as the button is held*
     (was a fixed 300ms burst per press), capped at `PEEL_HOLD_MAX_MS`
     (10s).
   - **`PEEL <ms>`** debug command, 1–5000ms, negative = reverse.
   - **`CMD_PEEL` (`0x34`)** bus command, `[dir, duration×10ms]`.
   All three stop early on a DRV8833 fault. SW1 is unchanged: one tooth
   forward on the feed motor, closed loop (needs the magnet).
7. **Debug output trimmed.** `TRACE` is off by default (one
   `move N ok <angle>` / `move N ERR <why>` line per move; `TRACE ON`
   brings back the 150ms trace), `STATUS` is two compact lines, commands
   reply `ok` / `ERR: ...`, `HELP` is a 7-line summary, and boot prints
   one banner + status instead of help + status twice. `IMON`,
   `5VSTATUS` and `I5V` all print the same one-line analog readout.
8. **Flash 97% -> 66%, RAM 50% -> 37%.** Debug line parsing no longer
   uses `String` (fixed `char` buffer + `strcmp_P`, own number parser
   instead of `toFloat()`/`strtod`), which dropped malloc/free/realloc/
   strtod and the RAM copies of every command-name literal. The
   `platformio.ini` also had `board_hardware.uart = uart0`, which on
   MiniCore means "a bootloader is installed": 512B reserved and
   **BOOTRST burned** so reset jumped to the empty boot section. Now
   `no_bootloader` - high fuse changes 0xD6 -> 0xD7, so **the first flash
   of this version must include fuses** (`flash.ps1` without
   `-SkipFuses`).
9. The heartbeat's "wheel moved while idle" warning no longer fires after
   a deliberate move (baseline is reset at the end of every move).
10. **Feed direction fixed (confirmed on real hardware).** The old default
    fed tape backwards while the loop still settled - so motor AND
    encoder were both flipped relative to physical "forward". Fixing only
    the motor polarity would have broken the loop (it'd drive away from
    every target). `invertMotorA` now flips the motor drive and the
    AS5600 angle together; default `true` = correct direction on this
    board (`MOTOR_A_BASE_INVERT` keeps the previously validated
    motor-vs-encoder relationship). Flipping it at runtime mirrors the
    angle, so a saved tape zero stops pointing at the same hole.
11. **Tooth seating always approaches BACKWARDS.** `snapToToothBackward()`
    moves to the tooth at or behind the current angle, never the one
    ahead, so seating never pushes extra tape forward. Runs automatically
    after homing (replacing the old "return to wherever the wheel
    started" move), and on demand with `SNAP`. Tooth grid is counted from
    the tape zero hole if set, else from encoder 0.
12. **Fast feed: hold SW1 >= 700ms** = one full sprocket turn forward
    (40 teeth, 160mm) for loading tape; short press is still one tooth.
    Long press chosen over double press so a normal single feed never has
    to wait to rule out a second press. Also `FASTFEED` debug command.
    Runs as four 90deg hops - first three pass through at full speed
    (`moveToAngle(..., passThrough=true)`), only the last decelerates.
13. **DRV8833 sleeps when idle.** `nSLEEP` goes low 300ms after the last
    motor drive (time for the end-of-move brake to stop the wheel), and
    `driveMotorA()`/`driveMotorB()` wake it on demand (2ms). Asleep, the
    outputs are Hi-Z - motors coast instead of being braked.
14. **Motor soft-start + buttons can't fire from a boot-held state**,
    both found from the same real-hardware symptom: the 12V eFuse
    tripping, specifically on motor commands, not at boot.
    - **No motor command used to ramp duty at all** - `driveMotorA()`/
      `driveMotorB()` went straight from "stopped" to the target duty in
      one `analogWrite()`. A motor at a dead stop has no back-EMF yet, so
      that's close to full voltage across the winding resistance - the
      locked-rotor/breakaway current regime, briefly well above running
      current. `VMOT` sits on the 5V rail, so that spike is drawn from
      the buck's output and reflected back to its 12V input (roughly
      scaled by the step-down ratio) - exactly what the eFuse's current
      limit sees. `softStartDuty()` now ramps a target duty up over
      `MOTOR_SOFTSTART_MS` (100ms) instead of stepping to it, applied in
      `moveToAngle()` (only when motor A is starting from a genuine
      standstill - `motorARunning` tracks this so a `fastFeedTurn()`
      passThrough hop, already spinning, isn't needlessly re-ramped
      between hops) and unconditionally in `runPeel()`/
      `peelWhileSw2Held()` (motor B always starts from a dead stop,
      there's no passThrough equivalent for peel). This tames the
      transient spike at the start of a move; it doesn't raise an eFuse
      current limit that's genuinely set below the motor's running
      current.
    - **SW1/SW2 were level-checked, not edge-checked** -
      `if (digitalRead(PIN_SWx) != activeLevel) return;` is satisfied
      immediately by a button already held down when `loop()` starts, no
      fresh press needed, and the debounce gate (`lastEdgeMs` starting at
      0) doesn't stop that either. Holding SW2 through power-up (a
      natural thing to try on the bench) fired a full-duty
      `driveMotorB()` within the debounce window of `loop()`'s first
      pass - stacking the no-soft-start spike above on the point in the
      board's life it's least settled. `sw1SeenReleased`/
      `sw2SeenReleased` now gate on having observed the button actually
      released at least once before any press can register, so a button
      held from before boot simply never arms until it's let go - no
      change to normal post-boot press/hold/long-press behavior.

Everything else — pin map, protocol, EEPROM layout, calibration,
addressing, tape-zero/pitch calibration, the DRV8833 channel-swap fix,
`invertMotorA` (still unverified against that channel swap on real
hardware) — carries over unchanged from beta1.

## Build / flash

Same as beta1 — see `flash.ps1` and `PROTOCOL.md`/`QUICK_REFERENCE.md`
at the `SW/` root for the debug-port command reference (all beta1
commands apply here too, `SELFTEST`/`RELAY ON`/`OFF` included, they're
just not run automatically anymore):

```bash
pio run -e atmega328pb_isp -t fuses    # one-time, or after board_hardware.* changes
pio run -e atmega328pb_isp -t upload
```

Debug port (Serial1, 9600 baud) is only reachable through the ISP header
(D11/D12) — mutually exclusive with ISP flashing on that same header at
any given instant.

## Open questions (not resolved here)

Carries forward every open question from `beta1/project.md` that wasn't
specifically addressed above — in particular:

- `invertMotorA = true`'s correctness is still unverified against the
  `PIN_AIN1`/`PIN_AIN2` ↔ `PIN_BIN1`/`PIN_BIN2` channel swap; re-test
  direction on real hardware.
- `PIN_5V_READY`'s PCB resistor swap (see above).
- No interrupt-driven UART0 RX yet — bus traffic arriving mid-move is
  still lost.
- `PICK_OFFSET_MM` is still a `0.0` placeholder, not yet measured on
  real hardware.
- Peel direction (`invertMotorB = false`) not yet confirmed on real
  hardware - if SW2 winds the cover tape the wrong way, `INVERTB ON`.
- Peel is not yet coordinated with feeding (FEED doesn't take up cover
  tape automatically) - tensioning is manual for now.
- **Button roles (TODO):** final mapping is meant to be SW1 = FEED,
  SW2 = UNFEED. SW2 is peel-while-held for now as a stopgap. Needs a
  decision on how peel/tensioning is triggered once SW2 is taken, and how
  UNFEED interacts with the peel motor and the seat-backwards tooth rule.
  See the TODO above `peelWhileSw2Held()` in `src/main.cpp`.

- **IMON calibration is a guess, to be developed on.** The only load
  current available on the bench so far is 28 mA, which reads as raw 128
  on `PIN_I_MON`. The firmware stores a single (raw, mA) point and
  assumes a straight line through the origin, so the working calibration
  is `CALI 28` taken at raw 128, with 0 mA assumed to be raw 0 (also not
  measured). Nothing between or beyond those two points has been checked.
  For scale, the factory default is raw 833 = 200 mA (~0.24 mA/count) and
  the 28 mA point gives ~0.22 mA/count, so they roughly agree, but the
  TPS26600's near-zero IMON offset and the real full-scale slope are
  unverified. To develop on: measure at least one more current well above
  28 mA (ideally near the 200 mA design max) plus a true no-load raw
  reading; if no-load raw is not ~0, replace the single-point-through-
  origin model with a two-point (offset + slope) one. 1 count is ~0.22 mA,
  so the low end is coarse and anything near the eFuse limit is
  extrapolated.

See `beta1/project.md`'s own "Open questions" section for the complete,
up-to-date list and reasoning behind each.
