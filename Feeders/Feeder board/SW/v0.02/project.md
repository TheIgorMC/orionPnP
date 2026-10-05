# v0.02

Forked from `v0.01a`. Everything in `v0.01a/project.md` (and the
`beta1/project.md` history behind it) still applies; this file only covers
what is new. Protocol details are in `../PROTOCOL.md`.

## What's new

1. **Per-feeder peel calibration.** The peel run time that suits one
   feeder (spool/roller/belt/motor differences) is saved on that feeder:
   - `CMD_SET_PEEL_TIME` (`0x35`) `[msHi,msLo]`, 10–5000 ms; debug
     `PEELCAL <ms>`.
   - `CMD_GET_PEEL_TIME` (`0x36`) → `CMD_PEEL_TIME_INFO` (`0xA4`); debug
     `PEELCAL`.
   - `CMD_PEEL` with only a direction byte runs the saved time; debug
     `PEELRUN [REV]`. `ERR_NOT_READY` until a time is saved (there is
     deliberately no guessed default).
   - Stored in the ATmega's internal EEPROM at 32 (`PeelCal`, CRC'd),
     independent of `FeederConfig`: a component change or `RESETCFG` never
     clears it. A full chip erase does.
   - Not yet automatic: `CMD_FEED_NEXT` still does not run the peel motor.
     The host (or the GUI's cycle test) pairs them. Folding the saved time
     into the feed is the obvious next step once the number is trusted.
2. **`CMD_JOG` (`0x37`)** — signed 0.1 mm relative move over the bus,
   ACK carries the new raw angle. Debug equivalent `JOG <mm>` (same as
   `MOVEMM`, capped at ±160). Lets a host seat a sprocket hole and then
   `CMD_ZERO_HERE` without the debug port.
3. **AT24C02 support.** On a plain AT24C02 `CMD_GET_SERIAL` NACKs because
   there is no `0x58` identification page; that is not a wiring fault.
   - `CMD_I2C_SCAN` (`0x38`) / debug `I2CSCAN` shows what answers.
   - `CMD_SET_SERIAL` (`0x39`) / debug `SETSERIAL <32 hex>` programs a
     16-byte serial at EEPROM offset `0x10`, reads it back, verifies.
     `ERR_LOCKED` if a factory serial exists, `ERR_I2C` on failure.
   - `CMD_GET_SERIAL` now returns the factory serial if present, else the
     programmed one; `STATUS` shows `serial=factory|user|n/a` and `peel=`.
   - AT24 writes are split on 8-byte page boundaries (previously a write
     crossing one would have wrapped and corrupted the page).
4. New error codes `ERR_I2C` (`0x07`) and `ERR_LOCKED` (`0x08`).

## Status

Built clean (PlatformIO, flash 76.6%, RAM 37.5%). **Not yet run on a real
board**: the new bus commands were exercised from the PC tools against a
simulated feeder only. First checks on hardware: `I2CSCAN` (does `0x50`
answer, does `0x58`), `PEELCAL 1570` then `PEELRUN`, `JOG 0.5` /
`JOG -0.5`, `SETSERIAL` then `SERIAL` after a power cycle.

## Open questions

Everything listed for v0.01a is still open (IMON calibration guess,
`PICK_OFFSET_MM`, UART RX not interrupt-driven, button roles, ...), plus:

- If the part turns out to be an AT24CS02 that fails at `0x58` anyway,
  the cause is address strapping (A0–A2) or wiring, not firmware;
  `I2CSCAN` will show which `0x5x` addresses answer.
- The peel time is a single value for both feed-length pitches. A 2 mm
  and an 8 mm feed need different amounts of peel, so this probably
  becomes "ms per mm of feed" once peel is coupled to feeding.
