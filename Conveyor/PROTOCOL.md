# Orion conveyor: RS485 protocol (DRAFT 0.1, for review)

A board conveyor on **rail 2**, same RS485 bus and frame format as the feeders (see
`Feeders/Feeder board/SW/PROTOCOL.md`). It is a different kind of device: fixed address, no discovery, its own
command range. Nothing here is implemented yet, neither in firmware nor in the host.

## 1. What it does

- **Travel**: a stepper moves the belt. Positive = toward the stop pin. There is no home switch on this axis: the
  controller calls the board's **leading edge at the entry sensor "0"** every time a board is detected.
- **Entry sensor** (opto-interrupter): detects an inserted board, gives the zero and, one edge later, the **board length**.
- **Width**: adjustable rail width for different boards (axis and mechanism still to be decided, section 8).
- **Clamp**: cams on a servo hold the board once it is at the stop.
- **Stopper** (optional, solenoid or servo): a pin the board's leading edge runs against.
- **Load and unload on the same side**: the board goes forward to the stop, is clamped, worked on, unclamped, and then
  the belt runs **backwards** until the board sticks out of the entry side by a set amount (not fully out).

## 2. Address and bus behaviour

- Fixed address **240 (0xF0)**, set in firmware. Addresses **240-247 are reserved for fixed-address devices**; the host
  never hands them to a feeder (it already probes only 1..40 and will refuse a "highest address" above 239).
- The conveyor **ignores `CMD_DISCOVER` and `CMD_ASSIGN_ADDR`**, so it takes no part in the address assignment phase.
  The host finds it by sending `GET_INFO` to 240 on rail 2 (a scan probes 240 explicitly in addition to 1..N).
- Frame: `[0xAA][ADDR][CMD][LEN][PAYLOAD][CRC8]`, CRC8 poly 0x07 over ADDR..PAYLOAD, 9600 baud, LEN 0..16, unknown
  commands and bad CRCs silently ignored. All multi-byte values are **big endian**.
- `CMD_PING` (0x01) / `CMD_PONG` (0x81) work as on the feeders, so the host's monitor treats the conveyor like any
  other unit (green / red tile).
- Feeder firmware ignores 0x50-0x6F and 0xC0-0xDF, and the conveyor ignores everything below 0x50 except
  PING (0x01), so sharing the rail cannot cause confusion.

## 3. Units and the position model

| Quantity | Unit | Type |
|---|---|---|
| Positions, distances, board length | 0.01 mm | int32 (positions) / uint32 (lengths) |
| Width | 0.1 mm | uint16 |
| Speeds | 0.1 mm/s | uint16 |
| Servo angles | degrees | uint8 |
| Times | ms | uint16 |

`pos` is the **leading edge of the board relative to the entry sensor**, positive toward the stop. It is zeroed
by the controller on the leading-edge event (sensor goes blocked while moving forward, or when a board is inserted by hand
into the idle sensor) and is dead-reckoned from step counts afterwards. `stopDistance` (configuration) is the pos
at which the leading edge touches the pin. A board of length `L` clears the sensor at `pos = L`, so the **length is
only measurable when `L <= stopDistance`** (`ERR_TOO_LONG` otherwise, section 6).

## 4. Command overview

Replies follow the feeder rules: `CMD_ACK` (0x82), `CMD_NACK` (0x83) with `[errCode]`, and a data reply for each GET.

### 4.1 Asynchronous motion (important difference)

Moves can take many seconds. A feeder holds the bus until its move is done; for the conveyor that would block every
feeder on rail 2 and make STOP impossible. So **motion commands answer immediately**:

- `CMD_ACK` = "accepted, running" (state becomes `BUSY`), or `CMD_NACK [err]` = "refused now" (busy, clamped, no board, bad param).
- The host polls `C_GET_STATUS` (about every 100-200 ms while busy, the monitor's normal pings continue). When `state`
  is no longer `BUSY`, `lastErr` says how it ended and the status holds the results (`pos`, `length`).
- `C_STOP` always works and ends the move at once.
- Commands that only switch something (clamp, stopper) are also asynchronous, since the servo needs time; `BUSY` covers them.

### 4.2 Command table

| Command | Code | Payload | Reply | Notes |
|---|---|---|---|---|
| `C_GET_INFO` | 0x50 | none | `C_INFO` (0xC0): `[devType=0x02, fwMajor, fwMinor, capsHi, capsLo]` | `caps`: b0 width axis, b1 stopper, b2 stop sensor, b3 clamp. Lets the host tell a conveyor from anything else |
| `C_GET_STATUS` | 0x51 | none | `C_STATUS` (0xC1), 13 bytes: `[state, flags, lastErr, pos(4), length(4), width(2)]` | see 4.3. `length` = 0xFFFFFFFF unknown, `width` = 0xFFFF unknown |
| `C_STOP` | 0x52 | none | `CMD_ACK` | halts the belt and width axis immediately. Clamp and stopper stay as they are |
| `C_IDENTIFY` | 0x53 | `[blinks]` | `CMD_ACK` | status LED blinks |
| `C_SET_CFG` | 0x54 | `[id, value(4)]` | `CMD_ACK` / `NACK [BAD_PARAM]` | one parameter, stored in EEPROM at once (see 5) |
| `C_GET_CFG` | 0x55 | `[id]` | `C_CFG` (0xC2): `[id, value(4)]` | |
| `C_MOVE` | 0x58 | `[delta int32]` | `CMD_ACK` / `NACK` | relative belt move, 0.01 mm, negative = backwards, at `SPEED_FEED`. Refused when clamped (`ERR_CLAMPED`). Updates `pos` |
| `C_LOAD` | 0x59 | `[flags]` b0 = skip positioning | `CMD_ACK` / `NACK` | the whole load: forward, zero at the leading edge (if not yet detected), run on until the trailing edge clears the sensor, store `length`, then (unless b0) go to the stop position as in `C_GOTO_STOP` |
| `C_MEASURE` | 0x5A | none | `CMD_ACK` / `NACK` | only the measuring pass of `C_LOAD`, no positioning. Ends with `length` valid |
| `C_GOTO_STOP` | 0x5B | none | `CMD_ACK` / `NACK` | moves the leading edge to `stopDistance`. With the stopper extended it drives on by `PUSH` (0.01 mm) at `SPEED_SLOW` so the belt slips under the board while it rests on the pin. Needs a known `pos` (a board was detected) |
| `C_UNLOAD` | 0x5C | none | `CMD_ACK` / `NACK` | reverse until the board's **trailing edge** reaches the entry sensor, then `EXPOSE` further (so the rear of the board sticks out, not off the belt). Refused when clamped. Retracts the stopper first if present. Ends with `pos` re-referenced to the trailing-edge event |
| `C_SET_WIDTH` | 0x60 | `[width uint16]` | `CMD_ACK` / `NACK` | needs the width axis homed (`ERR_NOT_HOMED`) and no board in the sensor (`ERR_BOARD_PRESENT`) |
| `C_HOME_WIDTH` | 0x61 | none | `CMD_ACK` / `NACK` | homes the width axis; width known afterwards |
| `C_CLAMP` | 0x64 | `[0 release, 1 clamp]` | `CMD_ACK` / `NACK` | clamp needs a board at the stop (stop sensor if fitted, else `pos` within 1 mm of `stopDistance`): `ERR_NO_BOARD`. Servo goes to `CLAMP_ON` / `CLAMP_OFF` angle, then holds for `CLAMP_MS` |
| `C_STOPPER` | 0x66 | `[0 retract, 1 extend]` | `CMD_ACK` / `NACK` | `ERR_BAD_PARAM` if `STOPPER_TYPE` is 0 |

### 4.3 State and flags

`state`: 0 `IDLE`, 1 `BUSY`, 2 `ERROR` (last move failed; cleared by the next accepted command), 3 `BOARD_AT_ENTRY`
(a board was inserted by hand and the zero is set; waiting for the host), 4 `HOMING_REQUIRED`.

`flags`: b0 entry sensor blocked, b1 stop sensor blocked (if fitted), b2 clamp engaged, b3 stopper extended,
b4 board position known (`pos` valid), b5 length valid, b6 width homed, b7 driver fault.

The host can watch `BOARD_AT_ENTRY` (monitor ping sweeps can read `C_GET_STATUS` instead of PING for this device)
and offer "board inserted: load it?".

## 5. Configuration parameters (`C_SET_CFG` / `C_GET_CFG`)

All persisted. Defaults are placeholders until the mechanics are known.

| Id | Name | Unit | Default | Meaning |
|---|---|---|---|---|
| 0x01 | `STOP_DISTANCE` | 0.01 mm | 25000 | sensor to pin, leading edge. Also the longest measurable board |
| 0x02 | `PUSH` | 0.01 mm | 200 | drive-on against the pin with the stopper extended |
| 0x03 | `EXPOSE` | 0.01 mm | 3000 | how far the board's rear goes past the sensor on unload |
| 0x04 | `SPEED_FEED` | 0.1 mm/s | 800 | normal belt speed |
| 0x05 | `SPEED_SLOW` | 0.1 mm/s | 150 | near the pin, and while measuring |
| 0x06 | `ACCEL` | 0.1 mm/s^2 | 2000 | |
| 0x07 | `STEPS_PER_MM` | steps x 100 | per mechanics | belt stepper calibration |
| 0x08 | `DIR_INVERT` | 0/1 | 0 | flips the belt direction |
| 0x09 | `MOTOR_CURRENT` | mA | per driver | run current |
| 0x0A | `HOLD_MODE` | 0 release, 1 hold | 1 | stepper hold when idle (1 keeps the board from creeping while clamped is not enough on a slick belt) |
| 0x0B | `MOVE_TIMEOUT` | s | 20 | any belt move that does not finish stops with `ERR_TIMEOUT` |
| 0x0C | `SENSOR_LEVEL` | 0/1 | 1 | sensor level when blocked |
| 0x0D | `STOPPER_TYPE` | 0 none, 1 solenoid, 2 servo | 0 | |
| 0x0E | `STOPPER_ON` / 0x0F `STOPPER_OFF` | deg (servo) or ms (solenoid pulse) | | |
| 0x10 | `CLAMP_ON` / 0x11 `CLAMP_OFF` | degrees | | cam positions |
| 0x12 | `CLAMP_MS` | ms | 600 | time the servo needs |
| 0x13 | `WIDTH_STEPS_PER_MM` | steps x 100 | | width axis |
| 0x14 | `WIDTH_MIN` / 0x15 `WIDTH_MAX` | 0.1 mm | | allowed range |
| 0x16 | `WATCHDOG` | s | 10 | no valid frame for this device for this long while moving: stop (`ERR_WATCHDOG`). 0 = off. Clamp and stopper are left as they are |

## 6. Error codes (`NACK [err]` and `lastErr`)

Reuses the feeder codes 0x01 `FAULT` (driver), 0x04 `TIMEOUT`, 0x05 `BAD_PARAM`, 0x07 `I2C` (EEPROM), and adds:

| Code | Name | Meaning |
|---|---|---|
| 0x10 | `ERR_NO_BOARD` | nothing at the sensor / stop when one is needed |
| 0x11 | `ERR_BOARD_PRESENT` | a board is in the sensor, so the width must not move |
| 0x12 | `ERR_CLAMPED` | belt moves refused while the clamp is engaged |
| 0x13 | `ERR_BUSY` | another motion is running (send `C_STOP` first) |
| 0x14 | `ERR_TOO_LONG` | the trailing edge did not clear the sensor before `STOP_DISTANCE` |
| 0x15 | `ERR_NOT_HOMED` | width axis not homed |
| 0x16 | `ERR_SENSOR` | sensor stuck blocked with nothing there, or never changes during a measure move |
| 0x17 | `ERR_WIDTH_LIMIT` | requested width outside `WIDTH_MIN..WIDTH_MAX` |
| 0x18 | `ERR_WATCHDOG` | host went silent during a move |

## 7. Sequences

**Load** (board pushed in by hand until the sensor sees it; `state = BOARD_AT_ENTRY`, pos = 0):

1. `C_STOPPER 1` (if a stopper is fitted), poll until IDLE.
2. `C_LOAD 0`, poll `C_GET_STATUS` until not BUSY. Result: `length` valid, `pos = STOP_DISTANCE`.
3. `C_CLAMP 1`, poll. The board is held. Work.

**Unload on the same side:**

1. `C_CLAMP 0`, poll.
2. `C_UNLOAD`, poll. Retracts the stopper, runs the belt backwards until the board's rear is `EXPOSE` past the sensor.
3. The user takes the board; the sensor clears.

**Change of board size:** `C_SET_WIDTH w` (no board in the sensor), then load as above.

## 8. Open points

1. **Width mechanism**: motorised (stepper with a home switch, as drafted), a servo, or set by hand and read back? This
   decides `C_HOME_WIDTH` / `C_SET_WIDTH` and the `WIDTH_*` parameters.
2. **Stop sensor** (board really at the pin): recommended, bit 2 of `caps`. Without it the host only knows the board
   got to `stopDistance`.
3. **Boards longer than `STOP_DISTANCE`** cannot be measured by passing the sensor. Either keep the sensor-to-pin
   distance longer than the longest board, or measure on the way out during `C_UNLOAD` (the trailing edge event).
   The draft keeps the first option.
4. **Belt slip against the pin** (`PUSH`) assumes the belt may slide under a stopped board. If it must not, the stopper is
   skipped and the board is positioned by steps only (`STOPPER_TYPE 0`).
5. **Controller and driver** (MCU, stepper driver with or without stall detection) are not chosen. Nothing here needs
   stall detection.
6. **Asynchronous vs. blocking** replies (4.1) is a recommendation. The feeder style (reply when done) would be simpler in
   firmware but blocks rail 2 for the length of every move.

## 9. Host side (for later, not part of this draft)

An `OrionConveyor` class next to `OrionFeeder`, a **Conveyor** tab in the feeder bus manager (board length and width,
clamp / stopper state, manual buttons, Load / Unload), the fixed address handled in the bus scan and the monitor, and
a few OpenPnP actuators or script helpers (`load`, `clamp`, `release`, `unload`) so jobs can call it.
