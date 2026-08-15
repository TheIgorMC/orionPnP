Closed-loop wheel-position test bench for the feeder — ATmega328PB-AU port.

Same DRV8833 driver IC, same AS5600 magnetic rotary encoder, and the same
closed-loop command protocol/behavior as `TestBench03` (STM32F411CC
Blackpill). This bench swaps the MCU for a bare ATmega328PB-AU on the
target board, so it's a different platform (8-bit AVR, no native USB,
2KB RAM) rather than a variant of the same board.

Sketch files:

- `drv8833_as5600_testbench.ino` + `pins_config.h` (Arduino IDE, identical
  to `src/main.cpp` + `include/pins_config.h`)
- `src/main.cpp` + `include/pins_config.h` (PlatformIO)

## Pin map: edit `pins_config.h`, not `main.cpp`

All MCU-facing pin assignments live in one file: `include/pins_config.h`
(and its identical Arduino-IDE mirror `pins_config.h` at the project root).
If a board revision moves a signal to a different pin, that file is the
only thing that needs editing — `main.cpp` refers to the pins only by the
names defined there (`PIN_AIN1`, `PIN_GO_FWD_INPUT`, etc.) and never
hardcodes a pin number itself.

Pin numbers are Arduino-style digital/analog numbers as exposed by
MiniCore's ATmega328PB variant (classic Uno-compatible numbering:
D0–D13, A0–A5).

Control inputs (buttons to GND, `INPUT_PULLUP`):

- GO+ input: `A0` — jogs the wheel +1 tooth (forward) when idle
- GO- input: `A1` — jogs the wheel -1 tooth (reverse) when idle

There is **no hardware abort/STOP button** — same reasoning as
`TestBench03`: a move blocks synchronously in `moveToAngle()`, so no
button or Serial input is polled while a move is in progress anyway. A
move can only end via reaching target, the stall detector, the move
timeout, or `nFAULT`. The Serial `STOP` command still exists but only
takes effect between moves.

DRV8833 logic pins:

- `AIN1` -> `D9` (PWM, Timer1/OC1A, forward duty for the sprocket motor)
- `AIN2` -> `D10` (PWM, Timer1/OC1B, reverse duty for the sprocket motor)
- `BIN1` -> `D7` (**unused**, held HIGH/brake for board compatibility)
- `BIN2` -> `D8` (**unused**, held HIGH/brake for board compatibility)
- `nSLEEP` -> `D4` (set HIGH to enable driver)
- `nFAULT` -> `D2` (input, active LOW)

DRV8833 power and motor pins (not MCU GPIO):

- `VM` -> motor supply (5V to 10.8V as valid for your motor)
- `GND` -> common ground with the ATmega328PB board
- `AOUT1`, `AOUT2` -> sprocket motor terminals
- `BOUT1`, `BOUT2` -> not connected to any motor (channel unused)

AS5600 magnetic encoder — hardware TWI0, **fixed pins, not reassignable**
via `pins_config.h`:

- `VCC` -> 3.3V or 5V per your AS5600 breakout's regulator (check before
  wiring — the chip itself is not 5V tolerant on most bare-chip wiring, but
  many breakout modules include a regulator/level shifting; verify yours)
- `GND` -> common ground
- `SCL` -> `A5`
- `SDA` -> `A4`
- `DIR` -> tie to GND if your breakout exposes it (default rotation sense);
  leave unconnected if the breakout doesn't expose the pin
- `OUT` (analog output) -> not connected, firmware reads I2C only

Most AS5600 breakout modules already carry onboard 4.7kΩ pull-ups on
SDA/SCL. If you're wiring a bare AS5600 chip instead of a breakout, add
2.2kΩ–4.7kΩ pull-ups from SDA and SCL to your logic rail yourself.

Serial (USART0) — hardware pins, fixed:

- `RXD` -> `D0`
- `TXD` -> `D1`

This MCU has **no native USB**. Talk to it through an external USB-serial
adapter for now. The pin map note in `pins_config.h` also flags this as
where an RS485 transceiver would attach later (same UART, added DE/RE
direction-control pin) — not implemented in this bench, just called out so
it isn't forgotten when that work starts.

### Magnet mounting

Same guidance as `TestBench03`: diametrically magnetized magnet centered on
the shaft, 0.5mm–3mm above the AS5600 package (1–2mm is a good start), the
encoder shaft turning 1:1 with the wheel.

## Why internal 8MHz oscillator + 9600 baud (not 16MHz + 115200 like TestBench03)

This board has no crystal — it runs on the ATmega328PB's internal 8MHz RC
oscillator. That oscillator has ~2% factory tolerance (vs. a crystal's
near-zero error), which becomes a real problem for UART framing at high
baud rates: 115200 baud (as used on the STM32 benches, where it costs
nothing because it's virtual USB CDC, not a real UART clocked off the MCU)
would be marginal-to-unreliable here. 9600 baud tolerates the oscillator's
error comfortably, and it also matches the planned RS485 link speed for
this board, so there's no rate to reconcile later. `SERIAL_BAUD` in
`main.cpp` and `monitor_speed` in `platformio.ini` are both set to 9600 —
change both together if you ever add a crystal and want to go faster.

For the same F_CPU-margin reason, the I2C clock to the AS5600 was dropped
from `TestBench03`'s 400kHz to 100kHz (standard mode): at 8MHz F_CPU the
TWBR divider needed for 400kHz is very small, which distorts the SCL duty
cycle. 100kHz has comfortable margin for how infrequently this firmware
actually needs to read the encoder.

## Flashing: ISP only, no bootloader

This is a bare ATmega328PB-AU with no bootloader burned. `platformio.ini`
is set up for a **USBasp** programmer via ISP (6-pin header into
`MOSI`/`MISO`/`SCK`/`RESET`/`VCC`/`GND`):

```bash
pio run -e atmega328pb_isp -t fuses    # one-time: set oscillator/BOD/etc fuses
pio run -e atmega328pb_isp -t upload   # flash firmware, every time after
```

`-t fuses` was validated against MiniCore's fuse calculator in this repo
(no programmer attached, so it only got as far as the point where it
correctly reported `lfuse=0xe2 hfuse=0xd6 efuse=0xf7` for this
oscillator/BOD/UART/EESAVE combination before failing to find a USBasp) —
it hasn't been validated end-to-end against real silicon. Double check the
computed fuse values against your actual board before trusting them
blindly, especially if you change any `board_hardware.*` setting in
`platformio.ini`.

If you're using a different programmer, swap `upload_protocol` in
`platformio.ini` — the common alternatives are commented right above it
(`stk500v1` for Arduino-as-ISP, `avrispmkii` for an Atmel/Microchip AVRISP
mkII).

No serial bootloader is set up in this bench (not needed: iterating via ISP
each time is simpler than the STM32 benches' DFU dance, and this board
isn't expected to need field-reflashing without a programmer attached).

## Command protocol (Serial, 9600 baud, newline terminated)

Identical to `TestBench03`:

- `A<deg>` — move to an absolute wheel angle, e.g. `A90.0`
- `T<index>` — move to an absolute tooth index 0–39, e.g. `T5`
- `STEP+1` / `STEP-1` — move by one whole tooth (±9.0°), relative to the
  current commanded target
- `STEP+0.5` / `STEP-0.5` — move by half a tooth (±4.5°)
- `ZERO` — re-run the zero-point (still-duty) auto-calibration on demand
- `STOP` — brake motor A immediately (only takes effect when idle; a move
  in progress cannot be interrupted this way, see Pin map above)
- `STATUS` — print current angle, tooth position, target, magnet health
  (`MD`/`ML`/`MH`, `AGC`, `MAGNITUDE`, plus a one-word `health` verdict:
  `OK`/`WEAK`/`STRONG`/`NONE`), calibrated duty values, I2C error count, and
  whether move tracing is on
- `TRACE ON` / `TRACE OFF` — toggle live per-move progress logging (on by
  default); see "Diagnosing a move" below
- `HELP` / `?` — print the command list

Hardware buttons mirror a subset of this: GO+ jogs +1 tooth, GO- jogs -1
tooth, both usable without a serial terminal attached. There is no
hardware equivalent of `STOP`.

## Auto zero-point calibration

Identical algorithm and tuning knobs to `TestBench03`:

1. On boot (and again any time you send `ZERO`), the motor is braked and
   the current AS5600 angle is recorded as a baseline.
2. Duty is ramped up in small steps (default: from 15 to 200, in steps of
   5, ~60ms per step) while the encoder is watched. As soon as the angle
   moves more than the noise threshold (0.6°) from baseline, that step's
   duty is recorded as `breakaway`, and the previous step (the last one
   that produced no measurable motion) is recorded as `stillDutyMax` —
   the actual zero point.
3. This is repeated independently for each direction, since H-bridge and
   motor characteristics are rarely perfectly symmetric.
4. The closed-loop mover then uses `breakaway + margin` as its low-speed
   "creep" duty when close to target, guaranteeing it can always actually
   move at the commanded creep speed instead of stalling at too low a duty.
5. Any wheel motion incurred during calibration is undone at the end by a
   corrective move back to the pre-calibration angle, so calibration is
   effectively position-neutral.
6. If the AS5600 doesn't report a detected magnet, or the motor never
   breaks away up to the max calibration duty (disconnected motor, stuck
   mechanism, wiring fault), calibration is skipped/aborted and a safe
   default creep duty is used instead — check `STATUS` and the serial log
   if you see this warning.

Calibration results (`stillDutyMax`, `minMoveDutyFwd`, `minMoveDutyRev`)
are printed after every calibration run and are visible any time via
`STATUS`. They are not persisted across power cycles — recalibration runs
automatically on every boot.

## Closed-loop move behavior

Identical to `TestBench03`:

- Error is computed as the shortest signed angular path to target (handles
  wraparound at 0°/360° correctly), so the wheel always takes the shorter
  way round.
- Above 3.0° of error, the motor runs at a fixed fast duty (110 by
  default). Inside 3.0°, it drops to the calibrated creep duty for that
  direction to avoid overshoot.
- The move completes and brakes once the error is within ±0.30° — small
  compared to a half-tooth step (4.5°).
- Stall protection: if the encoder shows no motion for 800ms while a move
  is commanded, the move aborts and brakes (protects against a jammed
  wheel or a disconnected motor).
- Overall move timeout: 6 seconds, after which the move aborts and brakes
  even if still not on target.
- Magnet loss: if the AS5600 stops reporting a detected magnet (`MD` bit
  clears) partway through a move, the move aborts immediately with its own
  distinct error instead of being misread as a generic stall.
- `nFAULT` is polled during a move and aborts it immediately. There is no
  button/Serial abort mid-move (see Pin map above) — the stall detector,
  the move timeout, and `nFAULT` are the only ways an in-progress move ends
  early.
- I2C reads are validated (`Wire.endTransmission`/`requestFrom` return
  checked): a failed AS5600 read is logged (throttled) and falls back to
  the last known-good angle instead of feeding a garbage value into the
  stall/tolerance math. Cumulative failure count is visible via `STATUS`
  (`i2cErrors=`).

Tuning knobs live at the top of `src/main.cpp`: `ANGLE_TOLERANCE_DEG`,
`CREEP_THRESHOLD_DEG`, `FAST_DUTY`, `INVERT_DIRECTION` (flip if the wheel
turns the wrong way relative to commanded sign), `SERIAL_BAUD`,
`I2C_CLOCK_HZ`, and the `CAL_*` constants for calibration behavior. Pin
assignments are deliberately **not** here — see `pins_config.h`.

### Diagnosing a move

Every commanded move (a jog button, `STEP`, `T`, `A`) is tagged with a
sequence number, e.g. `[move 12]`, so its lines can be told apart from the
3-second `STATUS` heartbeat and from other moves in the log:

- `[move N] start=... target=... err=...` — printed once when the move
  begins.
- `[move N] t=...ms angle=... err=... duty=... dir=... sinceMotion=...ms` —
  printed roughly every 150ms while the move is in progress (`TRACE ON`,
  the default; disable with `TRACE OFF` if it's too noisy).
- On any abort (stall, timeout, fault, or magnet loss), a one-line reason
  is followed by a detail line with `start`/`target`/`current`/`err`,
  `elapsed`/`sinceMotion`, the `duty`/`dir` that was being driven, and a
  full magnet health line — enough to tell a real mechanical jam (angle
  genuinely flat, duty at `FAST_DUTY`) apart from a sensor problem
  (`health` not `OK`, or repeated `WARN: AS5600 I2C read failed`) or a
  stale-target problem (large `err` on what should have been a small jog).

**Motor spinning with no move in progress:** `moveToAngle()` runs
synchronously, so `loop()`'s 3-second `STATUS` heartbeat can only print
between moves, never during one. If you see plain `STATUS`-formatted lines
(no `[move N]` tag) showing the angle changing steadily across heartbeats,
the motor is being driven by something other than the closed-loop logic.
On the STM32 bench this turned out to be a PWM output that didn't
actually turn off after "braking" — a known STM32duino
`digitalWrite`-after-`analogWrite` quirk. That specific cause doesn't
apply here: AVR's `digitalWrite()` already tears down a pin's PWM/timer
compare-output state when called (see the comment on `brakeMotorA()` in
`main.cpp`), so this failure mode is not expected to reproduce on this
MCU. The heartbeat drift guard was ported over anyway as a backstop: it
compares angle across the 3-second interval and prints `WARN: wheel moved
... deg between heartbeats with no move in progress` if it ever sees
unexplained motion, regardless of cause.

**Target auto-resync:** if a move aborts before reaching its target,
`targetAngleDeg` previously stayed at the unreached value. Since the jog
buttons/`STEP` add onto `targetAngleDeg` rather than the wheel's actual
position, repeated failures would otherwise compound silently — each new
jog aiming further past a position the wheel never actually reached.
Firmware resyncs `targetAngleDeg` to the real measured angle whenever a
commanded move fails, and logs it (`NOTE: resyncing target to actual angle
...`) so the drift is visible instead of silent.

## PlatformIO

This folder includes a PlatformIO project:

- `platformio.ini`
- `src/main.cpp`
- `include/pins_config.h`

Environment: `atmega328pb_isp` (board `ATmega328PB`, via MiniCore).

### Build

From this folder:

```bash
pio run -e atmega328pb_isp
```

### Flash (USBasp via ISP)

```bash
pio run -e atmega328pb_isp -t fuses    # one-time, or after changing board_hardware.* settings
pio run -e atmega328pb_isp -t upload
```

### Arduino IDE

Open `drv8833_as5600_testbench.ino` — `pins_config.h` in the same folder
will show up as a second tab and gets compiled automatically. You'll need
MiniCore added via Arduino IDE's Boards Manager (board URL:
`https://mcudude.github.io/MiniCore/package_MCUdude_MiniCore_index.json`),
then select **ATmega328 / ATmega328PB**, clock **Internal 8 MHz**, and a
matching programmer under Tools before using "Upload Using Programmer".
