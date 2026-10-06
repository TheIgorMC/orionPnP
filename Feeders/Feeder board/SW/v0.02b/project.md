# v0.02b

Forked from `v0.02`. Everything in `v0.02/project.md` (and `v0.01a/`,
`beta1/` behind it) still applies; this file only covers what is new.
Protocol details are in `../PROTOCOL.md`.

**Status:** syntax-checked with `avr-g++` against the Arduino AVR core and
`Adafruit_NeoPixel` (no errors, no `-Wall` warnings; PlatformIO itself
could not be run in the authoring environment, so flash/RAM sizes are not
measured). **Not yet run on a real board.**

## What's new

1. **Status RGB brightness is adjustable and saved.** `LEDBRIGHT <1-255>`
   (debug) / `CMD_SET_LED_BRIGHTNESS` (`0x3A`). Stored in the ATmega EEPROM
   (`LedCfg` at 48, CRC'd), applied immediately. Default is now 40 (was a
   fixed 128). An SK6812 channel draws ~18 mA at 255, so a single blue
   channel at 40 is ~3 mA, yellow (two channels) ~6 mA. Tune by eye with
   `LEDBRIGHT`; `STATUS` prints `led=`. Brightness change re-sets the last
   color from its raw value (NeoPixel's `setBrightness` rescales stored
   pixels lossily).

2. **Peel is tied to feed distance.** New per-feeder `PeelRate`
   (`PEELRATE <ms/mm>` / `CMD_SET_PEEL_RATE` `0x3B`, read with
   `CMD_GET_PEEL_RATE` `0x3C`): milliseconds of peel-motor run per mm of
   sprocket travel, stored in 0.1 ms/mm units (EEPROM 52, CRC'd, survives
   component changes and `RESETCFG`, like `PeelCal`). Unset = no coupling,
   exactly v0.02 behaviour (`PEELRATE 0` clears it).
   - **Forward feed** (`FEED`, `CMD_FEED_NEXT`, SW1 short press): the
     sprocket moves first, then the peel motor runs forward for
     `rate x mm`. Peel is normally *after* feeding: the carrier advances
     and the cover tape comes off at the peel point afterwards.
   - **Backward seat** (`snapToToothBackward()`, i.e. `SNAP` and the seat
     after boot homing): the peel motor runs in *reverse first* by
     `rate x backwardMm`, then the sprocket moves back. Reason: going back
     against already-peeled cover tape is hard; reversing the peel first
     gives it slack. Same rate in both directions, so the amounts match
     because the sprocket distance matches. Skipped below 0.3 mm.
   - Sequential, not simultaneous: two motors starting at once would stack
     their breakaway currents on the 5V rail / 12V eFuse (the problem the
     soft-start work in v0.01a was for). Proportional in amount, ordered in
     time.
   - **Not coupled:** fast feed (tape loading), `CMD_JOG`, `MOVEMM`, `GOMM`,
     `STEP`/`T`/`A`. Those are loading/calibration moves; they leave the
     peel motor alone.
   - Peel run time is capped at 5 s per action (`PEEL_CMD_MAX_MS`). A
     peel-time DRV8833 fault returns `ERR_FAULT`.
   - Consequence for a host: `CMD_FEED_NEXT` now replies after
     move + peel. A host that also sends `CMD_PEEL` after each feed would
     peel twice once a rate is set. The PC GUI's "feed only" cycle option
     is the right one then.
   - `PEELCAL`/`PEELRUN`/`CMD_PEEL` (fixed-duration peel) are unchanged.
     `PEELCAL` is a fixed time with no distance attached; `PEELRATE` is
     the distance-proportional one. The v0.02 open question ("a 2 mm and
     an 8 mm feed need different amounts of peel") is what this answers.

3. **Buttons: three modes, SW1 = forward and SW2 = reverse in each.**
   SW1+SW2 pressed together step to the next mode (feed only, peel only,
   feed+peel, then back to feed only). The order matches how a reel is set
   up: load the tape by hand, then tension the cover tape, then test real
   advances.
   - **Feed only (blue, boot default).** SW1 short = feed one tooth, SW1
     hold >= 700 ms = fast feed one turn (tape loading), SW2 = back one
     tooth. The peel motor is never touched.
   - **Peel only (orange).** SW1 held = peel forward, SW2 held = peel in
     reverse, each only while held (10 s cap), for tensioning by feel. Falls
     back to feed-only after 2 min without a press
     (`PEEL_MODE_IDLE_TIMEOUT_MS`), so a forgotten mode can't turn the next
     SW1 press into an unexpected peel.
   - **Feed+peel (green).** SW1 = feed one tooth, then peel by the
     `PeelRate`; SW2 = peel in reverse first, then back one tooth. Without
     a rate saved it behaves like feed-only. One tooth per press even if
     held. Fast feed is feed-only on purpose: it is a loading move.
   - `MODE` / `MODE FEED|PEEL|BOTH` on the debug port shows or sets it;
     `STATUS` prints `mode=`.
   - Colors are `ledReady()`; orange (255,90,0) and green are the new ones.
     Orange sits near yellow (booting) and red (error): both have a very
     different green channel, but pick another if it reads badly on your
     LED. In feed+peel mode, where steady green is the mode color, the
     bus-activity flash is blue instead.
   - Press both within an 80 ms window (`CHORD_WINDOW_MS`) to step. The
     window means every single press waits up to 80 ms before acting, so a
     chord never fires as a feed first. After a chord both buttons must be
     released before anything else registers.
   - The held-through-boot guard is kept: a button down at power-up isn't
     armed until released once.
   - New bus command `CMD_FEED_BACK` (`0x3D`): back up by the configured
     pitch, peel reversed first when a rate is set. The mirror of
     `CMD_FEED_NEXT`; it lets the PC tools test the backward case, which
     the buttons only reach in feed+peel mode.

4. **Green flash on bus traffic.** Whenever a frame addressed to this
   feeder arrives (unicast, or broadcast once it has an address), plus its
   own discovery/assign replies, the status RGB flashes green for 80 ms
   (`RX_FLASH_MS`), so on a bus with several feeders it is obvious which
   one is answering. In peel mode, where steady green already means the
   mode, the flash is blue. Frames for other addresses and CRC failures do
   not flash. A move or peel started by the frame turns the LED purple
   right away; the flash only holds off the idle repaint. Brightness
   follows `LEDBRIGHT`.

5. **PC tools** (`../tools/`): the GUI was reworked for first tests (live
   feeder card, bring-up checklist, peel-rate helper, LED brightness,
   remembered settings, always-visible STOP) and `sim_feeder.py` lets it be
   tried without hardware. See `../tools/README.md`.

6. **TAP-Jig test build.** `pio run -e atmega328pb_isp_test` builds the same
   firmware with `-DTAPJIG_TEST`: no boot homing, no button actions (buttons
   are only reported), the status RGB can be held, and four extra commands
   (`CMD_T_INPUTS/UPTIME/RGB/MOTOR`, `0x40`-`0x43`) for the test-and-program
   jig. Same fuses as the production env. The production build contains none
   of it and ignores those opcodes (the jig's final stage relies on that).
   Both builds were only compile-checked. See `../tools/TAPJIG.md`.

## Open questions

Everything open in `v0.02/project.md` and `v0.01a/project.md` still is, plus:

- **`PEELRATE` has no default and no measured value.** v0.02's
  `PEELCAL 1570` was for an unstated distance. Measure: peel ms for a known
  feed (e.g. a 4 mm tooth) until the cover tape stays taut, divide by the
  mm.
- Whether the forward peel should start slightly *before* the feed ends
  (overlap) instead of strictly after is untested; strictly after is the
  safe choice for the current budget.
- Reverse peel before a backward seat is applied on the boot-time seat as
  well. If the cover tape isn't threaded yet it just spins the peel motor
  for under one tooth's worth.
- SW2 back-one-tooth in feed-only mode moves the sprocket backwards with no
  peel slack, by design (the peel isn't involved in that mode). Backing out
  a tape that is already under peeled cover tape is a feed+peel job.
- Brightness default (40) is a guess for "visible but cheap"; adjust on the
  bench.
- Not built with PlatformIO here: confirm flash/RAM on the real toolchain.
  The first flash does not change fuses relative to v0.02.

## Build / flash

Same as v0.02 (`flash.ps1`, `pio run -e atmega328pb_isp -t upload`).
