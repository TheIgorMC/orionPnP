# Feeder Firmware Quick Reference (alpha01 / alpha02 / alpha03 / beta1)

Debug port: **Serial1**, 9600 baud, newline-terminated, via the ISP header
(D11/D12 — shares that header with ISP flashing, mutually exclusive at any
given instant). Not case-sensitive. `HELP`/`?` prints this list live from
whichever firmware is actually flashed.

Full design rationale for any of this: `alpha01/project.md` /
`alpha02/project.md` / `alpha03/project.md` / `beta1/project.md`. Full
RS485 wire protocol spec (opcodes, error codes): **`PROTOCOL.md`**.

## Bring-up / status

| Command | Does |
|---|---|
| `STATUS` / `WHOAMI` | addr, component, tapeZero, pitch, angle/target, magnet health, i2cErrors (+ `invertA`/`invertB`/`tapeWidthMm`/`lastMoveErr` on alpha02+, `serial=` on alpha03+, `relay=`/`iMonRaw=`/`iMonMa=`/`i5vEstMa=` on beta1, or `[RS485ECHO ON]` banner on alpha01 if active) |
| `HELP` / `?` | print this command list |
| `SIMADDR <n>` | force bus address `n` (1–247) locally, bench-only, skips `CMD_DISCOVER`/`CMD_ASSIGN_ADDR` |
| `LED ON` / `LED OFF` | standard external LED, plain on/off. alpha01/02: D13/PB5 (shares ISP header's SCK line). alpha03+: A3/PC3 (own pin) |
| `IDENTIFY [n]` | **alpha02+** — blink status LED white `n` times (default 3), mirrors `CMD_IDENTIFY` |
| `SETWIDTH <mm>` | **alpha02+** — set this unit's tape width (8/12/16/24/32/44/56), assembly/bench-time, mirrors `CMD_SET_HW_INFO` (alpha02: ATmega EEPROM; alpha03+: AT24CS02) |
| `SERIAL` | **alpha03+** — print the AT24CS02's factory-programmed 128-bit serial number as hex, mirrors `CMD_GET_SERIAL` |
| `RELAY ON` / `OFF` | **beta1 only** — force the RS485 bus-connect relay, bench-only override, bypasses the 5V-stable gate |
| `SELFTEST` | **beta1 only** — re-run the boot self-test on demand: relay x2 (audible click each way), ext LED, IMON/5V readout. Ends with the relay forced OFF — disconnects a live bus link until `RELAY ON`/reboot |
| *(hold SW1 or SW2 at power-up)* | **beta1 only** — relay button-test mode: RGB blue → red/relay off, then each SW1/SW2 press toggles the relay (green=on/red=off) indefinitely. Not a typed command — power-cycle without holding a button for a normal boot |
| `IMON` | **beta1 only** — print `PIN_I_MON` raw ADC + calibrated mA (TPS26600 IMON, 12V rail current sense) |
| `5VSTATUS` | **beta1 only** — print `PIN_5V_READY` raw ADC (internal 1.1V ref) + calibrated mV + estimated `i5vEstMa` + current relay state |
| `I5V` | **beta1 only** — print estimated 5V-rail current (mA), rough 12V-nominal power-balance estimate from IMON, debug only |
| `CALI <mA>` | **beta1 only** — calibrate IMON: capture current `PIN_I_MON` raw ADC against a real bench-ammeter mA reading, persisted |
| `CALV <V>` | **beta1 only** — calibrate 5V_READY: capture current `PIN_5V_READY` raw ADC against a real bench-multimeter volts reading, persisted |
| `CALSTATUS` | **beta1 only** — print the stored IMON/5V_READY calibration points |
| `CALRESET` | **beta1 only** — reset IMON/5V_READY calibration to the factory-calculated defaults |

## Motion (raw angle/tooth)

| Command | Does |
|---|---|
| `A<deg>` | move to absolute angle, e.g. `A90.0` |
| `T<index>` | move to absolute tooth index 0–39, e.g. `T5` |
| `STEP+1` / `STEP-1` | move ±1 tooth (9°) |
| `STEP+0.5` / `STEP-0.5` | move ±half tooth (4.5°) |
| `ZERO` | re-run DRV8833 still-duty motor calibration (electrical — **not** tape zero, see below) |
| `STOP` | brake motor A (alpha02+: both motors) immediately |
| `TRACE ON` / `TRACE OFF` | toggle live per-move progress logging |

## Tape zero / pitch calibration (per loaded component)

| Command | Does |
|---|---|
| `COMPONENT <id>` | set loaded component id (OpenPnP part id) — **resets tape zero + pitch if the id actually changed** |
| `ZEROHERE` | jog with `STEP`/`T` (whole/half-tooth, no camera) to seat the first pocket's hole, then run this to capture it |
| `PITCH <mm>` | set per-pick feed distance in mm — `4` standard EIA-481, `2` fine, `8`/`12`/`16`/`24` wider multi-hole reels — auto-translated to steps |
| `RESETCFG` | clear tape zero + pitch only (keeps component id) |
| `FEEDCFG <zeroRaw 0-4095> <halfTeeth 1-255>` | set both fields directly (low-level, mirrors `CMD_SET_FEED_CONFIG`) |

## Distance-based motion (mm, not degrees/teeth)

| Command | Does |
|---|---|
| `GOTOZERO` | move to calibrated pick point (tape zero + `PICK_OFFSET_MM`) |
| `GOMM <mm>` | move to tape zero + `PICK_OFFSET_MM` + mm (absolute) |
| `MOVEMM <mm>` | move by mm relative to current position — only needed routinely to (re-)measure `PICK_OFFSET_MM` itself with a camera, not per-reel |
| `FEED` | advance by the configured `PITCH` (next pocket) |

## Firmware-specific

| Command | Firmware | Does |
|---|---|---|
| `RS485ECHO ON` / `OFF` | **alpha01 only** | bypass framed protocol, mirror every RS485 byte straight back out — validates transceiver/wiring/RE-DE turnaround before trusting framing/CRC |
| `INVERTA ON` / `OFF` | **alpha02+** | flip motor A direction (RAM-only, resets on reboot) |
| `INVERTB ON` / `OFF` | **alpha02+** | flip motor B direction (RAM-only, resets on reboot) |

`alpha02`/`alpha03`/`beta1`'s SW1 does a real closed-loop +1 tooth jog
(needs a magnet mounted); SW2 always jogs motor B open-loop on every
firmware (no encoder on that motor). `alpha01`'s SW1/SW2 jog motor A/B
open-loop for bring-up without a magnet. On `beta1`, motor A's direction
defaults *inverted* (`invertMotorA = true`) — the final board's DRV8833
OUT1/OUT2 are swapped relative to the bench units this was tuned on.
Separately, `beta1`'s `pins_config.h` also swaps which DRV8833 channel
(`PIN_AIN1`/`PIN_AIN2` vs `PIN_BIN1`/`PIN_BIN2`) each of those roles uses
— found on the first real bench test, where commanding motor A moved the
peel motor instead (see `PROTOCOL.md` "Motor direction default").

---

## RS485 bus commands (not Modbus — see `PROTOCOL.md` for full detail)

Frame: `[0xAA][ADDR][CMD][LEN][PAYLOAD...][CRC8]`. `ADDR`: `0x00` broadcast
(also "still unassigned"), `1`–`247` unicast. Commands below are beta1's
full set; alpha03 has everything except `CMD_STATUS_INFO`'s last 3 payload
bytes (`iMonRaw`/`relayEngaged` - no I_MON or relay on alpha03); alpha02
additionally lacks `CMD_GET_SERIAL` (and its hw-info commands read/write
the ATmega's internal EEPROM, not an AT24CS02); alpha01 implements
everything up to `CMD_SET_EXT_LED` (no `CMD_SET_INVERT_DIR` or anything
after it) plus its own debug-only `RS485ECHO` transport test mode.

| Command | Code | Payload → | Reply |
|---|---|---|---|
| `CMD_PING` | `0x01` | — | `CMD_PONG` (`0x81`) |
| `CMD_DISCOVER` | `0x10` | broadcast, — | `CMD_DISCOVER_HERE` (`0x90`): `[nonceHi,nonceLo,componentIdHi,componentIdLo,tapeWidthMm]` |
| `CMD_ASSIGN_ADDR` | `0x11` | broadcast, `[nonceHi,nonceLo,newAddr]` | `CMD_ACK` under the new address, only from the matching nonce |
| `CMD_GET_COMPONENT` | `0x20` | — | `CMD_COMPONENT_INFO` (`0xA0`): `[idHi,idLo,zeroHi,zeroLo,feedHalfTeeth]` |
| `CMD_SET_COMPONENT` | `0x21` | `[idHi,idLo]` | `CMD_ACK`/`CMD_NACK` |
| `CMD_SET_FEED_CONFIG` | `0x22` | `[zeroHi,zeroLo,feedHalfTeeth]` | `CMD_ACK`/`CMD_NACK` |
| `CMD_RESET_CONFIG` | `0x23` | — | `CMD_ACK` |
| `CMD_ZERO_HERE` | `0x24` | — | `CMD_ACK` |
| `CMD_SET_PITCH_MM` | `0x25` | `[mm]` (1 byte) | `CMD_ACK`/`CMD_NACK` |
| `CMD_FEED_NEXT` | `0x26` | — | `CMD_ACK` / `CMD_NACK[errCode]` |
| `CMD_SET_EXT_LED` | `0x27` | `[state]` (0=off, nonzero=on) | `CMD_ACK`/`CMD_NACK` |
| `CMD_SET_INVERT_DIR` | `0x28` | **alpha02+** — `[motor(0=A,1=B), state(0/1)]` | `CMD_ACK`/`CMD_NACK` |
| `CMD_GET_HW_INFO` | `0x29` | **alpha02+** — — | `CMD_HW_INFO` (`0xA1`): `[tapeWidthMm]` |
| `CMD_SET_HW_INFO` | `0x2A` | **alpha02+** — `[tapeWidthMm]` | `CMD_ACK`/`CMD_NACK` |
| `CMD_GET_STATUS` | `0x30` | **alpha02+** — — | `CMD_STATUS_INFO` (`0xA2`): `[angleRawHi,angleRawLo,as5600Status,faultActive,lastMoveErr]` + **beta1 only**: `,iMonRawHi,iMonRawLo,relayEngaged` |
| `CMD_STOP` | `0x31` | **alpha02+** — — | `CMD_ACK` |
| `CMD_IDENTIFY` | `0x32` | **alpha02+** — `[blinkCount]` (0⇒3) | `CMD_ACK` (after blinking) |
| `CMD_GET_SERIAL` | `0x33` | **alpha03+** — — | `CMD_SERIAL_INFO` (`0xA3`): 16 bytes, or `CMD_NACK` if the AT24CS02 didn't respond |

`CMD_ACK` = `0x82`, `CMD_NACK` = `0x83`. Error codes (in `CMD_NACK`
payloads and `CMD_STATUS_INFO`'s `lastMoveErr`): `0x00` none, `0x01` fault,
`0x02` magnet lost, `0x03` stall, `0x04` timeout, `0x05` bad param, `0x06`
not ready (e.g. feed requested before calibrated). Full detail:
`PROTOCOL.md`.

---

## Two common bench sequences

**Load a reel / calibrate a component:**
```
COMPONENT 12
ZEROHERE          (after jogging STEP/T so the first pocket's hole is seated)
PITCH 4
FEED              (repeat per pick)
```

**Validate RS485 hardware (alpha01 only, needs a second USB-RS485 adapter):**
```
RS485ECHO ON
```
then send arbitrary bytes from the adapter's terminal and confirm they
come back unchanged — one byte at a time, exercising RE/DE turnaround on
every byte. `RS485ECHO OFF` to resume normal framed operation.

**Find a physical feeder by its bus address (alpha02+, over the bus):**
```
CMD_IDENTIFY -> feeder blinks its status LED white 3x
```
or `IDENTIFY` on its own debug port for the same effect locally.

**Read a feeder's factory serial number (alpha03+):**
```
SERIAL
```
or `CMD_GET_SERIAL` over the bus. Needs an AT24CS02 actually populated —
not present on any V0.2a board built so far, only on beta1 hardware.

**Check power-up sequencing on the bench before the divider/relay exist (beta1 only):**
```
RELAY ON       (bypass the 5V-stable gate to test the bus manually)
5VSTATUS       (see the raw internal-1.1V-ref reading and current relay state)
```
Normal boot waits for `PIN_5V_READY` to read stable for 500ms before
engaging the relay on its own — see `PROTOCOL.md` "Power sequencing" for
the reference gotcha this reading works around.

**Hand-calibrate IMON/5V_READY on a bench jig (beta1 only):**
```
CALI 87.5      (with a real ammeter reading 87.5mA on the 12V input right now)
CALV 5.02      (with a real multimeter reading 5.02V on the 5V rail right now)
CALSTATUS      (confirm the stored points)
```
Each command captures the live raw ADC reading against the value you
measured and persists it — future `IMON`/`5VSTATUS`/`STATUS` reads convert
through that point instead of the factory-calculated default. `CALRESET`
undoes both.

**Re-run the full hardware self-test without rebooting (beta1 only):**
```
SELFTEST
```
Relay clicks on/off twice (listen for it), ext LED flashes, then prints
the current IMON/5V readout — same sequence `setup()` runs once at boot.
Ends with the relay forced OFF, so if the feeder was already talking to
a live bus, that link drops until `RELAY ON` or a reboot re-engages it.

**Listen to the relay click for as long as needed (beta1 only, no console needed):**
Hold `SW1` or `SW2` while powering the board up. RGB goes blue, then red
once released (relay off) — from there every button press toggles the
relay and the RGB (green=on/red=off), indefinitely. Power-cycle without
holding a button to get a normal boot back.
