Closed-loop wheel-position test bench for the feeder.

Same board and same DRV8833 driver IC as `TestBench02`, plus an AS5600
magnetic rotary encoder on the sprocket shaft so the wheel can be commanded
to an absolute angle (or a tooth step) and the firmware closes the loop
against real position feedback instead of running open-loop.

DRV8833 channel A drives the sprocket motor. **DRV8833 channel B is wired
for board compatibility only and is never driven** — its inputs are set to
a hard brake once in `setup()` and are never touched again.

Sketch files:

- `drv8833_as5600_testbench.ino` (Arduino IDE, identical to `src/main.cpp`)
- `src/main.cpp` (PlatformIO)

## Is 1-tooth / 0.5-tooth indexing doable? Yes.

The wheel has 40 teeth, so:

- 1 tooth = 360° / 40 = **9.0°**
- 0.5 tooth = **4.5°**

The AS5600 is a 12-bit absolute encoder: 4096 counts per revolution, i.e.
360° / 4096 = **0.088° per count**. A half-tooth step (4.5°) is therefore
about **51 encoder counts**, roughly 51x finer than the coarsest increment
you need to resolve. There is plenty of margin to stop well inside a
half-tooth window (firmware defaults to a ±0.30° stop tolerance) even
accounting for encoder noise and mechanical backlash.

## Pin map (STM32F411CC Blackpill)

Control inputs (buttons to GND, `INPUT_PULLUP`):

- GO+ input: `PB12` — jogs the wheel +1 tooth (forward) when idle
- GO- input: `PB13` — jogs the wheel -1 tooth (reverse) when idle

There is **no hardware abort/STOP button**. Both inputs are edge-triggered
jogs, only acted on between moves (`loop()` polls them; `moveToAngle()`
blocks synchronously and does not check any button or Serial input while a
move is in progress). A move can only end via reaching target, the stall
detector (800ms no motion), the 6s move timeout, or `nFAULT`. The serial
`STOP` command still exists but likewise only takes effect when idle, for
the same reason.

DRV8833 logic pins:

- `AIN1` -> `PA8` (PWM, forward duty for the sprocket motor)
- `AIN2` -> `PA9` (PWM, reverse duty for the sprocket motor)
- `BIN1` -> `PA10` (**unused**, held HIGH/brake for board compatibility)
- `BIN2` -> `PB9` (**unused**, held HIGH/brake for board compatibility)
- `nSLEEP` -> `PB14` (set HIGH to enable driver)
- `nFAULT` -> `PB15` (input, active LOW)

DRV8833 power and motor pins (not MCU GPIO):

- `VM` -> motor supply (5V to 10.8V as valid for your motor)
- `GND` -> common ground with Blackpill
- `AOUT1`, `AOUT2` -> sprocket motor terminals
- `BOUT1`, `BOUT2` -> not connected to any motor (channel unused)

AS5600 magnetic encoder (I2C1):

- `VCC` -> 3.3V (Blackpill 3V3 rail — the AS5600 is **not** 5V tolerant on
  most breakout boards, check your module's regulator before using 5V)
- `GND` -> common ground
- `SCL` -> `PB6`
- `SDA` -> `PB7`
- `DIR` -> tie to GND if your breakout exposes it (default rotation sense);
  leave unconnected if the breakout doesn't expose the pin
- `OUT` (analog output) -> not connected, firmware reads I2C only

Most AS5600 breakout modules already carry onboard 4.7kΩ pull-ups on
SDA/SCL. If you're wiring a bare AS5600 chip instead of a breakout, add
2.2kΩ–4.7kΩ pull-ups from SDA and SCL to 3.3V yourself.

### Magnet mounting

- Use a diametrically magnetized magnet (AMS recommends 6mm x 2.5mm or
  similar) centered on the motor/wheel shaft, on the side facing the
  AS5600's top marking.
- Keep the magnet 0.5mm–3mm above the chip package; 1mm–2mm is a good
  starting point. Too far and `MAGNITUDE`/`AGC` readings degrade; too close
  and the sensor saturates.
- The encoder must see one full mechanical turn per one full wheel
  revolution (mount it on the wheel shaft itself, or on any 1:1 shaft
  coupled to it — not through a gear reduction, or the tooth math below no
  longer applies).

## Command protocol (USB serial, 115200 baud, newline terminated)

- `A<deg>` — move to an absolute wheel angle, e.g. `A90.0`
- `T<index>` — move to an absolute tooth index 0–39, e.g. `T5`
- `STEP+1` / `STEP-1` — move by one whole tooth (±9.0°), relative to the
  current commanded target
- `STEP+0.5` / `STEP-0.5` — move by half a tooth (±4.5°)
- `ZERO` — re-run the zero-point (still-duty) auto-calibration on demand
- `STOP` — brake motor A immediately (only takes effect when idle; a move in
  progress cannot be interrupted this way, see Pin map above)
- `STATUS` — print current angle, tooth position, target, magnet health
  (`MD`/`ML`/`MH`, `AGC`, `MAGNITUDE`, plus a one-word `health` verdict:
  `OK`/`WEAK`/`STRONG`/`NONE`), calibrated duty values, I2C error count, and
  whether move tracing is on
- `TRACE ON` / `TRACE OFF` — toggle live per-move progress logging (on by
  default); see "Diagnosing a move" below
- `HELP` / `?` — print the command list

Hardware buttons mirror a subset of this: GO+ jogs +1 tooth, GO- jogs -1
tooth, both usable without a serial terminal attached. There is no hardware
equivalent of `STOP`.

## Auto zero-point calibration

"Zero point" here means the boundary duty value sent to the DRV8833 that
still keeps the motor **stationary** — the highest PWM duty below the
point where the motor actually starts turning (breakaway/stiction point).
Firmware finds this automatically instead of relying on a guessed constant:

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
  checked): a failed AS5600 read is logged (throttled) and falls back to the
  last known-good angle instead of feeding a garbage value into the
  stall/tolerance math. Cumulative failure count is visible via `STATUS`
  (`i2cErrors=`).

Tuning knobs live at the top of `src/main.cpp`: `ANGLE_TOLERANCE_DEG`,
`CREEP_THRESHOLD_DEG`, `FAST_DUTY`, `INVERT_DIRECTION` (flip if the wheel
turns the wrong way relative to commanded sign), and the `CAL_*` constants
for calibration behavior.

### Diagnosing a move

Every commanded move (`GO` button, `STEP`, `T`, `A`) is tagged with a
sequence number, e.g. `[move 12]`, so its lines can be told apart from the
3-second `STATUS` heartbeat and from other moves in the log:

- `[move N] start=... target=... err=...` — printed once when the move
  begins.
- `[move N] t=...ms angle=... err=... duty=... dir=... sinceMotion=...ms` —
  printed roughly every 150ms while the move is in progress (`TRACE ON`,
  the default; disable with `TRACE OFF` if it's too noisy).
- On any abort (stall, timeout, fault, or magnet loss), a one-line
  reason is followed by a detail line with `start`/`target`/`current`/`err`,
  `elapsed`/`sinceMotion`, the `duty`/`dir` that was being driven, and a
  full magnet health line — enough to tell a real mechanical jam (angle
  genuinely flat, duty at `FAST_DUTY`) apart from a sensor problem (`health`
  not `OK`, or repeated `WARN: AS5600 I2C read failed`) or a stale-target
  problem (large `err` on what should have been a small jog).

**Motor spinning with no move in progress:** `moveToAngle()` runs
synchronously, so `loop()`'s 3-second `STATUS` heartbeat can only print
between moves, never during one. If you see plain `STATUS`-formatted lines
(no `[move N]` tag) showing the angle changing steadily across heartbeats,
the motor is being driven by something other than the closed-loop logic —
most likely a PWM output that didn't actually turn off. `analogWrite()` on
`PA8`/`PA9` puts those pins into hardware timer mode; a bare `digitalWrite()`
afterward doesn't reliably tear that back down on this core, so a "braked"
pin can keep outputting its last PWM duty. `brakeMotorA()` now forces
`pinMode(..., OUTPUT)` before the brake `digitalWrite`s to guarantee a full
GPIO re-init out of timer/alternate-function mode. As a backstop, the
heartbeat itself now also compares angle across the 3-second interval and
prints `WARN: wheel moved ... deg between heartbeats with no move in
progress` if it ever sees unexplained motion again, regardless of cause.

**Target auto-resync:** if a move aborts before reaching its target,
`targetAngleDeg` previously stayed at the unreached value. Since `GO`/`STEP`
add onto `targetAngleDeg` rather than the wheel's actual position, repeated
failures used to compound silently — each new jog aimed further past a
position the wheel never actually reached, until a "+1 tooth" jog ended up
commanding a move most of the way around the wheel. Firmware now resyncs
`targetAngleDeg` to the real measured angle whenever a commanded move fails,
and logs it (`NOTE: resyncing target to actual angle ...`) so the drift is
visible instead of silent.

### USB "device not recognized" note

Same as `TestBench02`: on STM32F411, USB FS data pins are `PA11` (D-) and
`PA12` (D+). This firmware does not drive either pin, so no special
workaround is needed here, but avoid reassigning motor or encoder pins onto
`PA11`/`PA12` if you customize the pin map.

## PlatformIO + DFU

This folder includes a PlatformIO project:

- `platformio.ini`
- `src/main.cpp`

Default environment:

- `blackpill_f411cc_dfu` (board `genericSTM32F411CC`)

Fallback environment (if your PlatformIO package does not include F411CC
board ID):

- `blackpill_f411ce_dfu`

### Build

From this folder:

```bash
pio run -e blackpill_f411cc_dfu
```

### Upload with DFU

1. Connect Blackpill by USB.
2. Put MCU in system bootloader mode (BOOT0 high, then reset).
3. Upload:

```bash
pio run -e blackpill_f411cc_dfu -t upload
```

4. Return BOOT0 low and reset to run firmware normally.

If `genericSTM32F411CC` is not recognized, use:

```bash
pio run -e blackpill_f411ce_dfu -t upload
```
