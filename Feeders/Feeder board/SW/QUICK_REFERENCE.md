# Feeder Firmware Quick Reference (alpha01 / alpha02)

Debug port: **Serial1**, 9600 baud, newline-terminated, via the ISP header
(D11/D12 — shares that header with ISP flashing, mutually exclusive at any
given instant). Not case-sensitive. `HELP`/`?` prints this list live from
whichever firmware is actually flashed.

Full design rationale for any of this: `alpha01/project.md` /
`alpha02/project.md`. Full RS485 wire protocol spec (opcodes, error codes):
**`PROTOCOL.md`**.

## Bring-up / status

| Command | Does |
|---|---|
| `STATUS` / `WHOAMI` | addr, component, tapeZero, pitch, angle/target, magnet health, i2cErrors (+ `invertA`/`invertB`/`tapeWidthMm`/`lastMoveErr` on alpha02, or `[RS485ECHO ON]` banner on alpha01 if active) |
| `HELP` / `?` | print this command list |
| `SIMADDR <n>` | force bus address `n` (1–247) locally, bench-only, skips `CMD_DISCOVER`/`CMD_ASSIGN_ADDR` |
| `LED ON` / `LED OFF` | external debug LED (D13/PB5) on/off |
| `IDENTIFY [n]` | **alpha02 only** — blink status LED white `n` times (default 3), mirrors `CMD_IDENTIFY` |
| `SETWIDTH <mm>` | **alpha02 only** — set this unit's tape width (8/12/16/24/32/44/56), assembly/bench-time, mirrors `CMD_SET_HW_INFO` |

## Motion (raw angle/tooth)

| Command | Does |
|---|---|
| `A<deg>` | move to absolute angle, e.g. `A90.0` |
| `T<index>` | move to absolute tooth index 0–39, e.g. `T5` |
| `STEP+1` / `STEP-1` | move ±1 tooth (9°) |
| `STEP+0.5` / `STEP-0.5` | move ±half tooth (4.5°) |
| `ZERO` | re-run DRV8833 still-duty motor calibration (electrical — **not** tape zero, see below) |
| `STOP` | brake motor A (alpha02: both motors) immediately |
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
| `INVERTA ON` / `OFF` | **alpha02 only** | flip motor A direction (RAM-only, resets on reboot) |
| `INVERTB ON` / `OFF` | **alpha02 only** | flip motor B direction (RAM-only, resets on reboot) |

`alpha02`'s SW1 does a real closed-loop +1 tooth jog (needs a magnet
mounted); SW2 always jogs motor B open-loop on both firmwares (no encoder
on that motor). `alpha01`'s SW1/SW2 jog motor A/B open-loop for bring-up
without a magnet.

---

## RS485 bus commands (not Modbus — see `PROTOCOL.md` for full detail)

Frame: `[0xAA][ADDR][CMD][LEN][PAYLOAD...][CRC8]`. `ADDR`: `0x00` broadcast
(also "still unassigned"), `1`–`247` unicast. Commands below are alpha02's
full set; alpha01 implements everything up to `CMD_SET_EXT_LED` (no
`CMD_SET_INVERT_DIR` or anything after it) plus its own debug-only
`RS485ECHO` transport test mode.

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
| `CMD_SET_INVERT_DIR` | `0x28` | **alpha02 only** — `[motor(0=A,1=B), state(0/1)]` | `CMD_ACK`/`CMD_NACK` |
| `CMD_GET_HW_INFO` | `0x29` | **alpha02 only** — — | `CMD_HW_INFO` (`0xA1`): `[tapeWidthMm]` |
| `CMD_SET_HW_INFO` | `0x2A` | **alpha02 only** — `[tapeWidthMm]` | `CMD_ACK`/`CMD_NACK` |
| `CMD_GET_STATUS` | `0x30` | **alpha02 only** — — | `CMD_STATUS_INFO` (`0xA2`): `[angleRawHi,angleRawLo,as5600Status,faultActive,lastMoveErr]` |
| `CMD_STOP` | `0x31` | **alpha02 only** — — | `CMD_ACK` |
| `CMD_IDENTIFY` | `0x32` | **alpha02 only** — `[blinkCount]` (0⇒3) | `CMD_ACK` (after blinking) |

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

**Find a physical feeder by its bus address (alpha02, over the bus):**
```
CMD_IDENTIFY -> feeder blinks its status LED white 3x
```
or `IDENTIFY` on its own debug port for the same effect locally.
