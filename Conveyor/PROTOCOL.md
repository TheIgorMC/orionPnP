# Orion conveyor: RS485 protocol (DRAFT 0.2, for review)

A board conveyor on **rail 2**, same RS485 bus and frame format as the feeders (see
`Feeders/Feeder board/SW/PROTOCOL.md`). It is a different kind of device: fixed address, no discovery, its own
command range. Nothing here is implemented yet, neither in firmware nor in the host.

Changes from 0.1: width by switch homing with a "clear window" check and a saved last width; a manual width entry;
unload direction (back out of the entry, or forward out of the far side) with an interlock against a board waiting at
the entry and a "next board ready" path; belt slip against the pin decided; hardware fixed (STM32F407 with 2x TMC2209).

## 1. What it does

- **Travel**: a stepper (TMC2209) moves the belt. Positive = toward the stop pin. No home switch on this axis: the
  board's **leading edge at the entry sensor is "0"** every time a board is detected.
- **Entry sensor** (opto-interrupter): detects an inserted board, gives the zero and, one edge later, the **board length**.
- **Width**: rail width adjustable by a second stepper (TMC2209) with a **home switch**.
- **Clamp**: cams on a servo hold the board once it is at the stop.
- **Stopper** (optional, solenoid or servo): a pin the board's leading edge runs against. The belt **slips under the
  board** while it rests on the pin.
- **Stop sensor** (optional, undecided): confirms a board is really at the pin. Kept as a capability bit; the protocol
  works without it.
- **Load and unload**: the board goes forward to the stop, is clamped, worked on, unclamped, and leaves either
  **backwards out of the entry side** (stopping with the rear still on the belt) or **forwards out of the far side**
  (configurable, section 7).

## 2. Hardware (fixed)

Custom board, **STM32F407**, two **TMC2209** drivers (belt, width) configured over their UART, RS485 transceiver with
direction control, entry sensor input, width home switch input, servo output (clamp), stopper output (optional), optional stop
sensor input, status LED. The TMC2209 gives current / microstep setup and driver fault flags (overtemperature, short)
over UART, and StallGuard if wanted later (not needed by this protocol; if it is used for jam detection it must be off
while the belt slips against the pin).

## 3. Address and bus behaviour

- Fixed address **240 (0xF0)**, set in firmware. Addresses **240-247 are reserved for fixed-address devices**; the host
  never hands them to a feeder (it probes only 1..40 and refuses a "highest address" above 239).
- The conveyor **ignores `CMD_DISCOVER` and `CMD_ASSIGN_ADDR`**, so it takes no part in the address assignment phase.
  The host finds it by sending `GET_INFO` to 240 on rail 2 (a scan probes 240 explicitly in addition to 1..N).
- Frame: `[0xAA][ADDR][CMD][LEN][PAYLOAD][CRC8]`, CRC8 poly 0x07 over ADDR..PAYLOAD, 9600 baud (the feeders set the bus
  speed), LEN 0..16, unknown commands and bad CRCs silently ignored. Multi-byte values are **big endian**.
- `CMD_PING` (0x01) / `CMD_PONG` (0x81) work as on the feeders, so the host's monitor treats the conveyor like any other
  unit (green / red tile).
- Feeder firmware ignores 0x50-0x6F and 0xC0-0xDF, and the conveyor ignores everything below 0x50 except PING (0x01), so
  sharing the rail cannot cause confusion.

## 4. Units and the position model

| Quantity | Unit | Type |
|---|---|---|
| Positions, distances, board length | 0.01 mm | int32 (positions) / uint32 (lengths) |
| Width | 0.1 mm | uint16 |
| Speeds | 0.1 mm/s | uint16 |
| Servo angles | degrees | uint8 |
| Times | ms | uint16 |

`pos` is the **leading edge of the loaded board relative to the entry sensor**, positive toward the stop. It is zeroed
on the leading-edge event (the sensor goes blocked while the belt moves forward, or a board is inserted by hand into the
idle sensor) **only when no board is loaded**, and is dead-reckoned from step counts afterwards. `STOP_DISTANCE`
(configuration) is the `pos` at which the leading edge touches the pin. A board of length `L` clears the sensor at
`pos = L`, so the **length is only measurable when `L <= STOP_DISTANCE`** (`ERR_TOO_LONG` otherwise).

A second board inserted while one is loaded is tracked as the **waiting board** (`posNext`, zeroed at its leading edge).
Both boards sit on the same belt, so every belt move shifts both; the waiting board's length is learned when its
trailing edge passes the sensor during a move.

## 5. Command overview

Replies follow the feeder rules: `CMD_ACK` (0x82), `CMD_NACK` (0x83) with `[errCode]`, and a data reply for each GET.

### 5.1 Asynchronous motion

Moves can take many seconds. A feeder holds the bus until its move is done; for the conveyor that would block every
feeder on rail 2 and make STOP impossible. So **motion commands answer immediately**:

- `CMD_ACK` = "accepted, running" (state becomes `BUSY`), or `CMD_NACK [err]` = "refused now".
- The host polls `C_GET_STATUS` (about every 100-200 ms while busy). When `state` is no longer `BUSY`, `lastErr` says how
  it ended and the status holds the results (`pos`, `length`).
- `C_STOP` always works and ends the move at once.
- Clamp and stopper are asynchronous too (the servo needs time); `BUSY` covers them.

### 5.2 Command table

| Command | Code | Payload | Reply | Notes |
|---|---|---|---|---|
| `C_GET_INFO` | 0x50 | none | `C_INFO` (0xC0): `[devType=0x02, fwMajor, fwMinor, capsHi, capsLo]` | `caps`: b0 width axis, b1 stopper, b2 stop sensor, b3 clamp |
| `C_GET_STATUS` | 0x51 | none | `C_STATUS` (0xC1), 14 bytes: `[state, flags(2), lastErr, pos(4), length(4), width(2)]` | see 5.3. `length` = 0xFFFFFFFF unknown, `width` = 0xFFFF unknown |
| `C_STOP` | 0x52 | none | `CMD_ACK` | halts belt and width axis at once. Clamp and stopper stay as they are |
| `C_IDENTIFY` | 0x53 | `[blinks]` | `CMD_ACK` | status LED blinks |
| `C_SET_CFG` | 0x54 | `[id, value(4)]` | `CMD_ACK` / `NACK [BAD_PARAM]` | one parameter, stored at once (section 6) |
| `C_GET_CFG` | 0x55 | `[id]` | `C_CFG` (0xC2): `[id, value(4)]` | |
| `C_MOVE` | 0x58 | `[delta int32]` | `CMD_ACK` / `NACK` | relative belt move, 0.01 mm, negative = backwards, at `SPEED_FEED`. Refused when clamped (`ERR_CLAMPED`); a backwards move is refused while a waiting board is at the entry (`ERR_ENTRY_BLOCKED`). Updates `pos` (and `posNext`) |
| `C_LOAD` | 0x59 | `[flags]` b0 = skip positioning | `CMD_ACK` / `NACK` | loads the **next board** (the one at the entry, or the tracked waiting board after a forward unload): forward, zero at the leading edge if not yet detected, on until the trailing edge clears the sensor, store `length`, then (unless b0) as `C_GOTO_STOP`. Sets "board loaded". Refused while another board is loaded (`ERR_BOARD_PRESENT`) |
| `C_MEASURE` | 0x5A | none | `CMD_ACK` / `NACK` | only the measuring pass, no positioning. Ends with `length` valid |
| `C_GOTO_STOP` | 0x5B | none | `CMD_ACK` / `NACK` | moves the leading edge to `STOP_DISTANCE`. With the stopper extended it drives on by `PUSH` at `SPEED_SLOW` so the belt slips under the board while it rests on the pin. Needs a known `pos` |
| `C_UNLOAD` | 0x5C | none | `CMD_ACK` / `NACK` | direction by `UNLOAD_DIR`, see section 7. Refused when clamped (`ERR_CLAMPED`) or when there is no loaded board (`ERR_NO_BOARD`). Clears "board loaded" when it ends |
| `C_CLEAR` | 0x5D | none | `CMD_ACK` | the user states that nothing is on the conveyor: clears "board loaded", "waiting board" and `pos`. Refused while the entry sensor is blocked (`ERR_BOARD_PRESENT`) or the clamp is engaged |
| `C_SET_WIDTH` | 0x60 | `[width uint16, clear uint8]` | `CMD_ACK` / `NACK` | move the width to `width` (0.1 mm). See section 8 for `clear` |
| `C_HOME_WIDTH` | 0x61 | `[clear uint8]` | `CMD_ACK` / `NACK` | drive to the home switch, set the width to `WIDTH_HOME`, mark the width confirmed |
| `C_DEFINE_WIDTH` | 0x62 | `[width uint16]` | `CMD_ACK` | **no motion**: the user says what the width is now (adjusted by hand), mark it confirmed and store it. Refused while the width axis moves |
| `C_CLAMP` | 0x64 | `[0 release, 1 clamp]` | `CMD_ACK` / `NACK` | clamp needs a board at the stop (stop sensor if fitted, else `pos` within 1 mm of `STOP_DISTANCE`): `ERR_NO_BOARD`. Servo to `CLAMP_ON` / `CLAMP_OFF`, then holds for `CLAMP_MS` |
| `C_STOPPER` | 0x66 | `[0 retract, 1 extend]` | `CMD_ACK` / `NACK` | `ERR_BAD_PARAM` if `STOPPER_TYPE` is 0 |

### 5.3 State and flags

`state`: 0 `IDLE`, 1 `BUSY`, 2 `ERROR` (last move failed; cleared by the next accepted command),
3 `BOARD_AT_ENTRY` (the entry sensor is blocked and no motion is running; with "board loaded" clear this is "a board
was inserted, waiting for the host" and `pos` is zeroed; with it set this is a **waiting board**).

`flags` (16 bits): b0 entry sensor blocked, b1 stop sensor blocked (if fitted), b2 clamp engaged, b3 stopper extended,
b4 `pos` valid, b5 `length` valid, b6 width homed since power-up, b7 driver fault (TMC2209), b8 **board loaded**,
b9 **board waiting at the entry** (blocked while one is loaded), b10 **width confirmed** (homed or defined since
power-up; clear = the value is only the last saved one), b11 waiting board's length valid, b12 **board leaving** (the unloaded board has not left the entry sensor yet: it is being taken out, so it is not reported as a newly inserted board).

## 6. Configuration parameters (`C_SET_CFG` / `C_GET_CFG`)

All persisted. Defaults are placeholders until the mechanics are known.

| Id | Name | Unit | Default | Meaning |
|---|---|---|---|---|
| 0x01 | `STOP_DISTANCE` | 0.01 mm | 25000 | sensor to pin, leading edge. Also the longest measurable board |
| 0x02 | `PUSH` | 0.01 mm | 200 | drive-on against the pin with the stopper extended (the belt slips) |
| 0x03 | `EXPOSE` | 0.01 mm | 3000 | backwards unload: how far the board's rear goes past the sensor |
| 0x04 | `SPEED_FEED` | 0.1 mm/s | 800 | normal belt speed |
| 0x05 | `SPEED_SLOW` | 0.1 mm/s | 150 | near the pin, and while measuring |
| 0x06 | `ACCEL` | 0.1 mm/s^2 | 2000 | |
| 0x07 | `BELT_STEPS_PER_MM` | steps x 100 | per mechanics | full steps per mm; microstepping is `MICROSTEPS` |
| 0x08 | `BELT_DIR_INVERT` | 0/1 | 0 | flips the belt direction |
| 0x09 | `BELT_IRUN` / 0x0A `BELT_IHOLD` | mA | per motor | TMC2209 run / hold current |
| 0x0B | `MICROSTEPS` | 1..256 | 16 | both axes |
| 0x0C | `MOVE_TIMEOUT` | s | 20 | any belt move that does not finish stops with `ERR_TIMEOUT` |
| 0x0D | `SENSOR_LEVEL` | 0/1 | 1 | entry sensor level when blocked |
| 0x0E | `STOPPER_TYPE` | 0 none, 1 solenoid, 2 servo | 0 | |
| 0x0F | `STOPPER_ON` / 0x10 `STOPPER_OFF` | deg (servo) or ms (solenoid pulse) | | |
| 0x11 | `CLAMP_ON` / 0x12 `CLAMP_OFF` | degrees | | cam positions |
| 0x13 | `CLAMP_MS` | ms | 600 | time the servo needs |
| 0x14 | `WIDTH_STEPS_PER_MM` | steps x 100 | | width axis |
| 0x15 | `WIDTH_MIN` / 0x16 `WIDTH_MAX` | 0.1 mm | | allowed range |
| 0x17 | `WIDTH_HOME` | 0.1 mm | = `WIDTH_MIN` | the width at the home switch |
| 0x18 | `WIDTH_DIR_INVERT` | 0/1 | 0 | |
| 0x19 | `WIDTH_IRUN` / 0x1A `WIDTH_IHOLD` | mA | | |
| 0x1B | `UNLOAD_DIR` | 0 back out of the entry side, 1 forward out of the far side | 0 | section 7 |
| 0x1C | `EXIT_DISTANCE` | 0.01 mm | 35000 | forward unload: belt travel from the stop position |
| 0x1D | `WATCHDOG` | s | 10 | no valid frame for this device for this long while moving: stop (`ERR_WATCHDOG`). 0 = off. Clamp and stopper stay as they are |

Saved besides the parameters: the last **width** (after every width move or definition), the **clamp state**, **board
loaded**, the **stopper state** and the board length.

**Power-up behaviour (decided):** the clamp **stays engaged** across a power cycle. The cam holds the board mechanically,
so a job that was stopped can resume without reloading. At boot the controller restores "board loaded", the length and the
stopper state from storage; if the clamp was engaged it sets `pos = STOP_DISTANCE` (`pos` valid), because a clamped board
is at the stop. The clamp servo is not moved at boot: its output is enabled at the stored angle, so it does not twitch or
release. Everything that depends on the board stays refused as usual (belt moves, width moves: `ERR_CLAMPED` /
`ERR_BOARD_PRESENT`) until the host releases the clamp. The host sees `board loaded` + `clamp engaged` in the first status
and can continue the job. If the user removed the board while the power was off, `C_CLEAR` (after releasing the clamp)
resets the memory.

## 7. Unload direction and the board at the entry

`UNLOAD_DIR 0` (back out of the entry side, same side as loading):

1. Stopper retracts if present. The belt runs backwards until the **trailing edge** of the board reaches the entry
   sensor (a precise reference), then `EXPOSE` further, so the rear sticks out but the board is not off the belt.
2. **Interlock**: with a **waiting board at the entry** the exit is blocked. `C_UNLOAD` (and any backwards `C_MOVE`)
   is refused with `ERR_ENTRY_BLOCKED` (0x19). The host tells the user "a board is waiting at the entry, remove it to
   unload" and re-checks the status. Nothing is auto-reversed into an inserted board.
3. When no board is loaded and one is inserted, the status shows `BOARD_AT_ENTRY` and the host prompts "load it?".

`UNLOAD_DIR 1` (forward out of the far side):

1. Stopper retracts, the belt runs forward `EXIT_DISTANCE`. The board leaves the working area.
2. A board waiting at the entry is **not an obstacle, it is "next board ready"**: it rides along and is tracked
   (`posNext`, its length measured as its trailing edge passes the sensor). After the unload ends, the host can
   immediately `C_LOAD` it (positioned from its tracked `posNext`). The host may do that automatically.
3. Guard: if the waiting board's leading edge would pass `STOP_DISTANCE` during that move, the unload is refused
   (`ERR_NEXT_OVERRUN`, 0x1A) and the user is asked to remove it.

## 8. Width: clear window, last width, manual entry

- The width is stored after every move or definition. At power-up the controller reports that stored width with
  `width confirmed` **clear**: it is where the carriage was left, not proof that it is still there.
- A width move needs the belt to be empty. `C_SET_WIDTH` / `C_HOME_WIDTH` carry a `clear` byte. With `clear = 0` the
  command is refused with `ERR_BOARD_PRESENT` when the entry sensor is blocked, the clamp is engaged, or "board loaded"
  is set. With `clear = 1` the **user has stated** the conveyor is empty: the controller drops its board memory
  (as `C_CLEAR`) and moves, but still refuses while the entry sensor is blocked or the clamp is engaged.
- **Host flow ("clear window")**: before any width action the host asks "Is the conveyor clear of boards?" with three
  answers: **Clear: home now** (`clear = 1`), **A board is on it: remove it** (the host waits until the entry sensor and
  clamp are free and asks again), or **Enter the width by hand** (`C_DEFINE_WIDTH`, no motion).
- The width axis moves toward the home switch on `C_HOME_WIDTH` and sets `WIDTH_HOME`. If the stored width is wrong and
  a move runs into the switch, the controller takes that as a homing event, corrects the width, and finishes the move
  from the right place (`width confirmed` set).
- On first connection with `width confirmed` clear, the host offers the clear window once.

## 9. Error codes (`NACK [err]` and `lastErr`)

Reuses the feeder codes 0x01 `FAULT` (TMC2209 fault), 0x04 `TIMEOUT`, 0x05 `BAD_PARAM`, 0x07 `I2C` (storage), and adds:

| Code | Name | Meaning |
|---|---|---|
| 0x10 | `ERR_NO_BOARD` | nothing at the sensor / stop / loaded when one is needed |
| 0x11 | `ERR_BOARD_PRESENT` | a board is in the sensor, clamped or loaded, so the width must not move or another cannot be loaded |
| 0x12 | `ERR_CLAMPED` | belt moves refused while the clamp is engaged |
| 0x13 | `ERR_BUSY` | another motion is running (send `C_STOP` first) |
| 0x14 | `ERR_TOO_LONG` | the trailing edge did not clear the sensor before `STOP_DISTANCE` |
| 0x15 | `ERR_NOT_HOMED` | a width move before any width is known (nothing stored, not homed) |
| 0x16 | `ERR_SENSOR` | sensor stuck blocked with nothing there, or never changes during a measure move |
| 0x17 | `ERR_WIDTH_LIMIT` | requested width outside `WIDTH_MIN..WIDTH_MAX` |
| 0x18 | `ERR_WATCHDOG` | host went silent during a move |
| 0x19 | `ERR_ENTRY_BLOCKED` | a waiting board at the entry blocks a backwards move / unload |
| 0x1A | `ERR_NEXT_OVERRUN` | forward unload would push the waiting board past the stop |

## 10. Sequences

**Load** (board pushed in by hand until the sensor sees it; `state = BOARD_AT_ENTRY`, board loaded clear, pos = 0; the host asks "load it?"):

1. `C_STOPPER 1` (if fitted), poll until IDLE.
2. `C_LOAD 0`, poll `C_GET_STATUS` until not BUSY. Result: `length` valid, `pos = STOP_DISTANCE`, board loaded.
3. `C_CLAMP 1`, poll. The board is held. Work.

**Unload, back out of the entry** (`UNLOAD_DIR 0`): `C_CLAMP 0`; if a board waits at the entry the host stops here and
asks the user to remove it; `C_UNLOAD`, poll; the user takes the board.

**Unload, forward** (`UNLOAD_DIR 1`): `C_CLAMP 0`; `C_UNLOAD`, poll; if a waiting board exists (`b9`), the host runs
the load sequence for it at once.

**Change of board size:** the clear window (section 8), `C_HOME_WIDTH 1` or `C_SET_WIDTH w 1`, then load.

## 11. Open points

(Clamp across power cycles is decided: it stays engaged, section 6.)

1. **Stop sensor**: undecided how to make it work well. Capability bit 2 stays; nothing depends on it.
2. **Boards longer than `STOP_DISTANCE`** cannot be measured by passing the sensor. Keep the sensor-to-pin distance
   longer than the longest board, or measure the length on the way out in a backwards unload (the trailing-edge event,
   `pos` at that moment is `D - L`). The draft keeps the first option.
3. Default values (speeds, currents, distances, servo angles) need the real mechanics.

## 12. Host side (for later, not part of this draft)

An `OrionConveyor` class next to `OrionFeeder`, a **Conveyor** tab in the feeder bus manager (board length and width,
clamp / stopper state, manual buttons, Load / Unload, the clear window dialog, "board inserted: load it?" and "board waiting
at the entry" prompts), the fixed address handled in the bus scan and the monitor, and a few OpenPnP actuators or script
helpers (`load`, `clamp`, `release`, `unload`) so jobs can call it.
