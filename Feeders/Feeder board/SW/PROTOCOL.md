# Feeder RS485 Protocol (beta1) — current state

This is the canonical spec for the frame protocol implemented in
`beta1/src/main.cpp`, kept separate from `alpha01`/`alpha02`/`alpha03`/
`beta1`'s `project.md` files so it doesn't drift out of sync across four
copies. Still **not Modbus** — see `alpha01/project.md`'s Modbus
feasibility section for why that's still an open decision; this is what's
actually implemented today.

- `alpha03` implements everything here except `CMD_STATUS_INFO`'s last
  three payload bytes (`iMonRaw`/`relayEngaged` - alpha03 has no I_MON or
  relay), and `invertMotorA` defaults to `false` there instead of `true`.
- `alpha02` additionally lacks `CMD_GET_SERIAL`, and its
  `CMD_DISCOVER_HERE`/`CMD_GET_HW_INFO`/`CMD_SET_HW_INFO` read/write
  `FeederHardwareInfo` from the ATmega's internal EEPROM, not an AT24CS02
  (alpha02 doesn't have one).
- `alpha01` implements a smaller subset still (everything up to and
  including `CMD_SET_EXT_LED`, no hardware info/status/stop/identify/
  serial) plus its own `RS485ECHO` debug-only test mode.
- See `alpha01/project.md`.

## Physical layer

- RS485 half-duplex, MAX1487 transceiver, combined RE#/DE direction
  control on one MCU pin (`PIN_RS485_RE`).
- 9600 baud — both feeder UARTs (RS485 + local debug) run off the
  ATmega328PB's internal 8MHz RC oscillator, which isn't accurate enough
  for standard higher bauds. Revisit if a crystal is added in a future
  hardware revision.

## Frame format

```
[0xAA][ADDR][CMD][LEN][PAYLOAD...][CRC8]
```

| Field | Size | Notes |
|---|---|---|
| Start | 1 B | fixed `0xAA` |
| ADDR | 1 B | `0x00` = broadcast (also "still unassigned"); `1`–`247` = unicast bus address |
| CMD | 1 B | command/reply code, see table below |
| LEN | 1 B | payload length in bytes (0–16) |
| PAYLOAD | LEN B | command-specific |
| CRC8 | 1 B | poly `0x07`, computed over `ADDR..PAYLOAD` (not the start byte) |

A feeder's own address (or `0x00` while unassigned) always tags frames it
sends. Unknown commands are silently ignored; a CRC mismatch silently
drops the frame (no retry/NACK — the sender's own timeout is the recovery
mechanism, matching a simple RTU-style bus).

## Addressing / discovery

Bus address is **RAM-only and disposable** — no EEPROM persistence, no
dedicated programming bus (dropped from the project; see
`alpha01/project.md` "Addressing scheme, v2" for the full reasoning).
Every boot starts at `ADDR_UNASSIGNED` (`0x00`) and re-earns an address
each time:

1. Host broadcasts `CMD_DISCOVER`.
2. Every still-unassigned feeder waits a random jitter delay (up to
   `DISCOVERY_JITTER_MAX_MS`, 200ms) then replies `CMD_DISCOVER_HERE` with
   its session nonce, its persisted component id, and (as of this
   revision) its tape width — so a host learns most of what it needs
   about a feeder in one round-trip, without separate queries for the
   common case.
3. Host picks a free address and broadcasts `CMD_ASSIGN_ADDR` with that
   nonce; only the matching feeder adopts the address.

## Command reference

`CMD_ACK` = `0x82`, `CMD_NACK` = `0x83` (payload `[errCode]` where noted —
see **Error codes** below; otherwise empty).

| Command | Code | Payload → | Reply | Notes |
|---|---|---|---|---|
| `CMD_PING` | `0x01` | — | `CMD_PONG` (`0x81`) | liveness check |
| `CMD_DISCOVER` | `0x10` | broadcast, — | `CMD_DISCOVER_HERE` (`0x90`): `[nonceHi,nonceLo,componentIdHi,componentIdLo,tapeWidthMm]` | only unassigned feeders react |
| `CMD_ASSIGN_ADDR` | `0x11` | broadcast, `[nonceHi,nonceLo,newAddr]` | `CMD_ACK` under the new address | only the matching nonce adopts it |
| `CMD_GET_COMPONENT` | `0x20` | — | `CMD_COMPONENT_INFO` (`0xA0`): `[idHi,idLo,zeroHi,zeroLo,feedHalfTeeth]` | |
| `CMD_SET_COMPONENT` | `0x21` | `[idHi,idLo]` | `CMD_ACK` | resets tape zero + pitch if the id actually changed |
| `CMD_SET_FEED_CONFIG` | `0x22` | `[zeroHi,zeroLo,feedHalfTeeth]` | `CMD_ACK` | low-level, sets both fields directly |
| `CMD_RESET_CONFIG` | `0x23` | — | `CMD_ACK` | clears tape zero + pitch only, keeps component id |
| `CMD_ZERO_HERE` | `0x24` | — | `CMD_ACK` | captures current position as tape zero |
| `CMD_SET_PITCH_MM` | `0x25` | `[mm]` (1 B) | `CMD_ACK`/`CMD_NACK` | auto-translated to `feedHalfTeeth` |
| `CMD_FEED_NEXT` | `0x26` | — | `CMD_ACK` / `CMD_NACK[errCode]` | advance by configured pitch |
| `CMD_SET_EXT_LED` | `0x27` | `[state]` (0/nonzero) | `CMD_ACK`/`CMD_NACK` | standard LED, plain on/off. alpha01/02: D13/PB5 (shares the ISP header's SCK line). alpha03: A3/PC3 (own pin, no ISP conflict) |
| `CMD_SET_INVERT_DIR` | `0x28` | `[motor(0=A,1=B), state(0/1)]` | `CMD_ACK`/`CMD_NACK` | RAM-only, resets on reboot |
| `CMD_GET_HW_INFO` | `0x29` | — | `CMD_HW_INFO` (`0xA1`): `[tapeWidthMm]` | `0xFF` = unset |
| `CMD_SET_HW_INFO` | `0x2A` | `[tapeWidthMm]` | `CMD_ACK`/`CMD_NACK` | assembly/bench-time only, validated against EIA-481 widths, no reset command |
| `CMD_GET_STATUS` | `0x30` | — | `CMD_STATUS_INFO` (`0xA2`): `[angleRawHi,angleRawLo,as5600Status,faultActive,lastMoveErr,iMonRawHi,iMonRawLo,relayEngaged]` | live telemetry; last 3 bytes **beta1 only** (alpha02/03 send the first 5) |
| `CMD_STOP` | `0x31` | — | `CMD_ACK` | immediate brake, both motors |
| `CMD_IDENTIFY` | `0x32` | `[blinkCount]` (0 ⇒ default 3) | `CMD_ACK` (after blinking) | white LED flashes, distinct from the magnet-status green/red |
| `CMD_GET_SERIAL` | `0x33` | **alpha03+** — — | `CMD_SERIAL_INFO` (`0xA3`): 16 bytes | AT24CS02 factory-programmed 128-bit serial number; `CMD_NACK` if the chip didn't respond |

## Error codes

Returned in a `CMD_NACK` payload wherever a command's failure has a
specific, useful cause (currently `CMD_FEED_NEXT`; `CMD_GET_STATUS`'s
`lastMoveErr` field reports the same codes for the most recent move,
whether or not it was triggered by the command currently being handled —
e.g. a local button jog). `0x00` is the only success value.

| Code | Name | Meaning |
|---|---|---|
| `0x00` | `ERR_NONE` | success |
| `0x01` | `ERR_FAULT` | DRV8833 `nFAULT` asserted during the move |
| `0x02` | `ERR_MAGNET_LOST` | AS5600 stopped reporting a detected magnet mid-move |
| `0x03` | `ERR_STALL` | no encoder motion for `STALL_TIMEOUT_MS` |
| `0x04` | `ERR_TIMEOUT` | move exceeded its timeout without reaching target |
| `0x05` | `ERR_BAD_PARAM` | malformed/out-of-range command payload |
| `0x06` | `ERR_NOT_READY` | e.g. `CMD_FEED_NEXT` requested before pitch/zero calibrated |

## Hardware identity vs. per-component config

Two separate persisted structures, on purpose — see `alpha02/project.md`
for the full reasoning:

- **`FeederConfig`** (component id, tape zero, feed pitch) — reset
  whenever `componentId` changes or on an explicit `CMD_RESET_CONFIG`.
  This is "what's currently loaded." Always the ATmega's internal EEPROM,
  on every firmware version.
- **`FeederHardwareInfo`** (tape width) — set once at
  assembly/bench-test time, never reset by anything else. This is "what
  this physical unit is," fixed by its mechanical build (tape guide/rail
  width), identical for every reel ever loaded into it. It does not feed
  into any of the firmware's own motion math — sprocket hole pitch is a
  fixed 4mm regardless of tape width (EIA-481) — its value is purely
  informational, for a host to validate reel compatibility and for fleet/
  inventory management. Included in the `CMD_DISCOVER_HERE` reply so a
  host learns it immediately without a separate query in the common case.
  Storage location differs by version: alpha01/02 keep it in the ATmega's
  internal EEPROM (same as `FeederConfig`, different address); **alpha03
  moves it onto the AT24CS02's EEPROM** (byte offset `0x00`), so it
  survives even a full chip-erase/reflash of the ATmega, not just a
  component change — see `alpha03/project.md`.

### AT24CS02 (alpha03 only)

I2C EEPROM + factory-programmed, read-only 128-bit unique serial number,
on the same I2C bus as the AS5600 (`SDA`/`SCL`, different address, no new
pins). Two I2C device-select addresses per the datasheet — not two memory
regions within one address:

- `0x50` — general-purpose EEPROM (read/write). `FeederHardwareInfo` lives
  at byte offset `0x00` here.
- `0x58` — identification page (read-only, factory-programmed). The
  128-bit serial number is read from byte offset `0x00` of this address,
  16 bytes, exposed via `CMD_GET_SERIAL`.

Not present on any V0.2a board built so far — `alpha03` is the first
firmware to expect it, ahead of the beta1 schematic actually adding it.
Every access is written to degrade gracefully (checked
`endTransmission`/`requestFrom`, same defensive pattern as the AS5600
code) rather than hang if the chip isn't populated: `CMD_GET_HW_INFO`
reports `tapeWidthMm = 0xFF` (unset) and `CMD_GET_SERIAL` replies
`CMD_NACK` instead of crashing or blocking. **None of this has been
validated against real AT24CS02 silicon** — the address assumptions,
the identification-page layout, and the fixed 5ms write-cycle delay (no
ack-polling implemented) are all from the datasheet, not from a working
board.

## Power sequencing (beta1 only)

Not present on any board built so far — beta1 is the first firmware to
expect this hardware, ahead of the schematic actually adding it.

- **`PIN_I_MON` (A6/PE2)** — analog, the IMON output of a TPS26600 eFuse
  on the 12V rail (RIMON=309kΩ, 1%), voltage proportional to load
  current, linear through the origin: ~4.07V at the 200mA design max
  load. Read with the default AVCC reference. `iMonRaw` (raw ADC) goes
  out over `CMD_STATUS_INFO`; firmware also exposes a converted
  `readIMonMilliamps()` (used by the `IMON`/`STATUS` debug commands) built
  from that same 4.07V/200mA calibration point. `EN`/`FLT#` are not wired
  to the MCU — the MCU only runs once the eFuse is already on, so there's
  no fault state where firmware could still be reading a pin to report it.
- **`PIN_5V_READY` (A7/PE3) + `PIN_485_RELAY` (D13/PB5)** — a 4.7k
  (rail)/1k (GND) divider on the 5V rail gates a MOSFET-driven relay that physically
  connects/disconnects this feeder's RS485 lines from the shared bus.
  The relay stays disconnected at reset and only engages once the rail
  has read stable for 500ms (`RELAY_READY_STABLE_MS`), so a feeder that's
  still mid-power-up can't disturb a bus already in use. `relayEngaged`
  is reported in `CMD_STATUS_INFO`; there's no bus command to force it
  (deliberate — if the relay's open, a bus command couldn't reach the
  feeder to force it anyway; use the `RELAY ON`/`OFF` debug-port override
  for bench testing instead).

  **Reference gotcha:** `PIN_5V_READY` is read against the ATmega's
  internal 1.1V bandgap reference, not the default AVCC — the divider
  taps the same 5V rail that AVCC normally *is*, so an AVCC-referenced
  read would report a fixed ratio regardless of the rail's actual value
  and could never detect "still ramping." The internal reference fixes
  detection; the 4.7k/1k ratio (~0.88V, ~816/1023 counts against the 1.1V
  reference at a healthy 5V rail) was chosen so it only clips above
  ~6.27V, comfortably clear of a 5V rail's normal tolerance, while using
  ~80% of the ADC's range — roughly 2x the resolution of a 10k/1k
  alternative. See `beta1/project.md`, "5V rail reading: a reference
  gotcha," for the full comparison.

## Motor direction default (beta1 only)

`invertMotorA` defaults to `true` in beta1 (`false` in alpha02/03) — the
final board's DRV8833 OUT1/OUT2 (motor-output side) are swapped relative
to the bench units this control loop was originally tuned against. Still
runtime-overridable (`INVERTA`, `CMD_SET_INVERT_DIR`) if this turns out to
be the wrong fix once real hardware exists — see `beta1/project.md`.

## Not yet in this protocol

- No interrupt-driven UART0 RX — see `alpha01/project.md`'s Modbus
  feasibility section; still a prerequisite before trusting this under
  real bus load with moves in progress.
- No re-addressing flow for an already-assigned feeder (deliberate — see
  `alpha01/project.md` "Addressing scheme, v2").
- No persistence for `invertMotorA`/`invertMotorB` — RAM-only per boot.
- CRC8 (not CRC16) — adequate for this bus's low rate/short frames so
  far; revisit if real-world error rates say otherwise.
