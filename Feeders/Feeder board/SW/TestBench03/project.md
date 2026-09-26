Closed-loop wheel-position test bench for the feeder.

Same board and same DRV8833 driver IC as `TestBench02`, plus an AS5600
magnetic rotary encoder on the sprocket shaft so the wheel can be commanded
to an absolute angle (or a tooth step) and the firmware closes the loop
against real position feedback instead of running open-loop.

DRV8833 channel A drives the front (tape-feed) sprocket motor,
closed-loop via the AS5600. DRV8833 channel B drives the rear (peel)
motor, which pulls the cover tape — it has **no position feedback of its
own**, so it's always driven open-loop, fixed on/off (no PWM/variable
speed). Motor B is only ever driven:

- **after** motor A completes a move/feed that turns out to be in the
  real-world **forward** direction (tap-jog, `STEP+`/`T`/`A` commands that
  move forward, or forward continuous feed) — motor B then runs alone for
  exactly as long as that move/feed took. It is **never** driven for A's
  reverse motion, and never runs *while* A is moving (sequential, not
  concurrent — see "PEEL-after-forward, continuous feed, and the manual
  PEEL command" below for why).
- alone, for a fixed duration, on the manual `PEEL` command (GO+
  double-tap or serial `PEEL`).

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

- GO+ input: `PB12` — forward jog button
- GO- input: `PB13` — reverse jog button

Each button reads three gestures (see "PEEL-after-forward, continuous
feed, and the manual PEEL command" below for full behavior):

- **tap** — jog one tooth
- **hold** past `LONG_PRESS_MS` — continuous feed until released
- **double-tap** (GO+ only) — `PEEL`

There is **no hardware abort/STOP button**. Buttons are only acted on
between moves/feeds (`loop()` polls them; `moveToAngle()` and
`continuousFeed()` block synchronously and do not check Serial input while
running — `continuousFeed()` does poll its own triggering button directly,
to know when to stop). A single move can only end via reaching target, the
stall detector (800ms no motion), the 6s move timeout, or `nFAULT`; a
continuous feed ends via button release, the stall detector, or `nFAULT`
(it has no timeout, since duration is however long the button is held).
The serial `STOP` command still exists but likewise only takes effect when
idle, for the same reason.

DRV8833 logic pins:

- `AIN1` -> `PA8` (PWM, forward duty for the front/tape-feed motor)
- `AIN2` -> `PA9` (PWM, reverse duty for the front/tape-feed motor)
- `BIN1` -> `PA10` (rear/peel motor, fixed on/off — no PWM, see below)
- `BIN2` -> `PB9` (rear/peel motor, fixed on/off — no PWM, see below)
- `nSLEEP` -> `PB14` (set HIGH to enable driver)
- `nFAULT` -> `PB15` (input, active LOW)

DRV8833 power and motor pins (not MCU GPIO):

- `VM` -> motor supply (5V to 10.8V as valid for your motors)
- `GND` -> common ground with Blackpill
- `AOUT1`, `AOUT2` -> front/tape-feed motor terminals
- `BOUT1`, `BOUT2` -> rear/peel motor terminals

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
- If `STATUS`/move logs show `health=WEAK` (`ML=1`) with a low-ish `AGC`
  and `MAGNITUDE` reading well under what you saw at initial setup, the
  magnet is too far from the chip — move it closer, within the 0.5mm–3mm
  range above. A marginal/weak signal like this is also the most likely
  cause of an intermittent `ERROR: magnet lost during move (MD=0)` abort
  that happens within milliseconds of a normal read (i.e. it isn't a
  sustained loss, just the reading flickering across the detection
  threshold) — fix the gap rather than treating that abort as a software
  bug.

## Command protocol (USB serial, 115200 baud, newline terminated)

- `A<deg>` — move to an absolute wheel angle, e.g. `A90.0`
- `T<index>` — move to an absolute tooth index 0–39, e.g. `T5`
- `STEP+1` / `STEP-1` — move by one whole tooth (±9.0°), relative to the
  current commanded target
- `STEP+0.5` / `STEP-0.5` — move by half a tooth (±4.5°)
- `ZERO` — re-run the zero-point (still-duty) auto-calibration on demand
- `STOP` — brake motors A and B immediately (only takes effect when idle; a
  move or continuous feed in progress cannot be interrupted this way, see
  Pin map above)
- `PEEL` — run the rear/peel motor (B) forward for `PEEL_DURATION_MS`; motor
  A is untouched. Serial equivalent of the GO+ double-tap.
- `STATUS` — print current angle, tooth position, target, magnet health
  (`MD`/`ML`/`MH`, `AGC`, `MAGNITUDE`, plus a one-word `health` verdict:
  `OK`/`WEAK`/`STRONG`/`NONE`), calibrated duty values, I2C error count, and
  whether move tracing is on
- `TRACE ON` / `TRACE OFF` — toggle live per-move progress logging (on by
  default); see "Diagnosing a move" below
- `HELP` / `?` — print the command list

Hardware buttons cover a superset of this: GO+ tap jogs +1 tooth then
peels afterward (timed to match), GO- tap jogs -1 tooth only, either
button held runs continuous feed (GO+ peels after release), GO+
double-tapped runs `PEEL` immediately — all usable without a serial
terminal attached. There is no hardware equivalent of `STOP`.

## PEEL-after-forward, continuous feed, and the manual PEEL command

Motor B (rear/peel) has no encoder, so none of its behavior here is
closed-loop — it's either "run alone for exactly as long as A's last
forward move/feed took" or "run open-loop for a fixed time." Related
tuning knobs, all at the top of `src/main.cpp`:

- `LONG_PRESS_MS` (350) — hold a jog button past this and it's a long-press
  instead of a tap.
- `DOUBLE_TAP_WINDOW_MS` (350) — a second GO+ tap starting within this long
  after the first tap's release counts as a double-tap.
- `PEEL_DURATION_MS` (400) — how long the *manual* `PEEL` command drives
  motor B. This is a timed duration, not a measured tape length (no
  feedback exists to measure it) — tune it to how much cover tape one
  manual peel should pull for your mechanism. It does **not** apply to the
  automatic peel described below, which is timed to the move instead.
- `CONTINUOUS_FEED_DUTY` (defaults to `FAST_DUTY`) — motor A's duty during
  continuous feed. There's no target to creep toward while free-running,
  so this is the only speed used.
- `INVERT_DIRECTION_B` — flip if motor B runs backwards from what's
  expected (independent of `INVERT_DIRECTION`, which is motor A only).

**PEEL-after-forward:** whenever a move (`moveToAngle()` — tap-jog, `STEP`,
`T`, `A`) or a continuous feed turns out to be in the real-world forward
direction, motor B does **not** run concurrently with motor A. Instead,
once A finishes (reaches target, or the button is released for continuous
feed), motor B runs alone at its own fixed full-power duty for exactly as
long as A's move/feed just took, then brakes. Motor B has no speed
feedback of its own, so there was no way to know how far it had actually
travelled if run concurrently with A at A's (variable, creep-then-fast)
duty — running it afterward, timed to A's elapsed duration, is the
approximation used instead. A's reverse motion never triggers this. A
failed/aborted move or feed (stall, timeout, fault, magnet loss) does
**not** trigger it either — only a clean completion does.

**Continuous feed (long-press):** holding either jog button past
`LONG_PRESS_MS` switches from "jog one tooth" to "run continuously at
`CONTINUOUS_FEED_DUTY` until the button is released," ignoring
target/tolerance entirely — it's a manual speed-jog, not a closed-loop
move, so there's no move timeout. The stall detector and `nFAULT` are still
live during a continuous feed, same protection as a normal move. When the
button is released, motor A brakes and `targetAngleDeg` is resynced to the
real measured angle — same reasoning as the target auto-resync described
below, so the next tap/hold starts from where the wheel actually is — and
then, if the feed was forward, PEEL-after-forward runs motor B for the
total hold duration.

**Manual PEEL (GO+ double-tap, or serial `PEEL`):** drives only motor B
forward for the fixed `PEEL_DURATION_MS`, immediately, with no associated
move. Motor A is untouched — the sprocket wheel doesn't move. `nFAULT`
still aborts it early, but there's no other way to interrupt a peel in
progress (same "no hardware abort mid-action" reasoning as moves).

**Gesture detection implementation note:** `classifyPress()` blocks (a
short, bounded busy-wait, same style as the rest of this firmware) to tell
tap/double-tap/hold apart before the caller acts — for a hold, it returns
as soon as `LONG_PRESS_MS` is crossed while the button is *still* held, and
the caller (`continuousFeed()`) then polls that same pin directly in its
own loop to know when to stop. This means a GO+ tap's jog doesn't start
until the double-tap window has elapsed with no second tap — by design, to
disambiguate it from a double-tap; GO- has no double-tap gesture, so its
taps register immediately on release.

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
`CREEP_THRESHOLD_DEG`, `FAST_DUTY`, and the `CAL_*` constants for
calibration behavior.

There are two completely different "wrong direction" fixes here, at two
different layers. Flipping the wrong one for a given symptom makes things
*worse*, not better — each one individually breaks a different invariant:

- **`INVERT_DIRECTION`** (default `false`) — the motor/encoder
  self-consistency flag. The whole closed loop's convergence depends on
  "driving forward makes the raw angle reading move toward the target";
  flipping this when that's *not* the actual problem breaks that
  invariant and makes the loop fight itself. Symptom that means you
  actually need this one: a move drives away from target, error keeps
  growing instead of shrinking, and it eventually times out after nearly a
  full revolution.
- **`RAW_ANGLE_INCREASE_IS_REVERSE`** (default `true` on this bench) — a
  pure labeling fact, completely separate from the loop's internal
  self-consistency: "does driving the raw AS5600 angle to increase count
  as this mechanism's real-world forward, or reverse?" Everything that
  needs to translate human intent (which way is `+1 tooth`, which way
  `T<index>` counts, when the automatic PEEL-after-forward triggers, which
  label a log line prints) reads this one fact via `TOOTH_STEP_DEG` /
  `toRealForward()`, instead of each place being flipped independently and
  risking drifting out of sync with each other. Symptom that means you
  need *this* one, not `INVERT_DIRECTION`: moves/jogs complete correctly —
  no fighting, no runaway, no timeout — but the resulting real-world
  direction is backwards from what the button/command implies. Confirmed
  needed on this bench: `STEP-2` (a pure target-angle change, no buttons
  involved) completed cleanly but in the physically-backwards direction —
  exactly this symptom, not `INVERT_DIRECTION`'s.

If you ever need to re-derive which one applies: change nothing about the
motor/encoder wiring, run a `STEP` command, and watch the `[move N]` trace
in the log (see "Diagnosing a move" below). If `err` shrinks steadily to
zero and the move completes normally, the loop is self-consistent — only
`RAW_ANGLE_INCREASE_IS_REVERSE` should ever be touched, regardless of which
real-world direction it ends up moving. If `err` grows instead of shrinking
and the move times out after nearly a full revolution, that's
`INVERT_DIRECTION`.

Motor B / gesture tuning (`INVERT_DIRECTION_B`, `LONG_PRESS_MS`,
`DOUBLE_TAP_WINDOW_MS`, `PEEL_DURATION_MS`, `CONTINUOUS_FEED_DUTY`) is
covered in "PEEL-after-forward, continuous feed, and the manual PEEL
command" above.

### Diagnosing a move

Every commanded move, continuous feed, or `PEEL` is tagged with a sequence
number, e.g. `[move 12]`, so its lines can be told apart from the 3-second
`STATUS` heartbeat and from each other in the log. Continuous feed reuses
the same `[move N]` start/trace/abort shape as a normal move (just without
a `target`/`err`, since it has none); `PEEL` only logs a start and an
end/abort line, since it's a plain timed drive with no angle to trace:

- `[move N] start=... target=... err=...` — printed once when the move
  begins.
- `[move N] t=...ms angle=... err=... duty=... dir=... sinceMotion=...ms` —
  printed roughly every 150ms while the move is in progress (`TRACE ON`,
  the default; disable with `TRACE OFF` if it's too noisy). `dir=` is the
  real-world direction (via `toRealForward()`), not the raw motor-pin
  sense, so it matches what you'd actually see the wheel do.
- On any abort (stall, timeout, fault, or magnet loss), a one-line
  reason is followed by a detail line with `start`/`target`/`current`/`err`,
  `elapsed`/`sinceMotion`, the `duty`/`dir` that was being driven, and a
  full magnet health line — enough to tell a real mechanical jam (angle
  genuinely flat, duty at `FAST_DUTY`) apart from a sensor problem (`health`
  not `OK`, or repeated `WARN: AS5600 I2C read failed`) or a stale-target
  problem (large `err` on what should have been a small jog).
- On a clean completion that turned out to be real-world-forward, two more
  lines follow under the *same* `[move N]` tag: `PEEL: running rear motor
  (B) for ...ms (matches this move's duration)` and `PEEL complete` — this
  is the automatic PEEL-after-forward described above, not a new move.

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
