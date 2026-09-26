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

See `beta1/project.md`'s own "Open questions" section for the complete,
up-to-date list and reasoning behind each.
