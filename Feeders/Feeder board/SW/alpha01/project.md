# alpha01 — feeder firmware, real board, RS485 added

Supersedes `TestBench04` (deleted). Same ATmega328PB-AU target, same
DRV8833 + AS5600 closed-loop wheel-position logic, but two changes:

1. **Pins now match the real board schematic** (`SCH_Feeder_V02.pdf`,
   V0.2a MCU page / V0.2b Serial page), not an ad-hoc ISP bench wiring.
   See `include/pins_config.h` — every pin there is copied straight off
   the schematic net names (`AIN1`, `RGB`, `RE`, `RO`, `DI`, etc.), so if
   a future board revision moves a signal, that file should be the only
   thing that needs editing.
2. **RS485 is now real hardware, not a "maybe later" comment.** USART0
   (D0/D1) goes to the MAX1487 transceiver, with combined RE#/DE direction
   control on D2. Because that UART is now a shared bus with other
   feeders on it, all debug/human-readable output moved to **Serial1**
   (USART1, D11/D12) — those pins are shared with the ISP programming
   header (see the "Programming Header" section of the Feeder-Design wiki
   page), so debug access and ISP flashing are mutually exclusive at any
   given instant, same as before, just now on a dedicated port instead of
   sharing the bus UART.

The frame format implemented in `src/main.cpp` (`FRAME_START`/`CMD_PING`/
`CMD_SET_ADDR`/CRC8) is **explicitly a placeholder**, not Modbus. It exists
to validate RE/DE switching timing, addressing, and byte-level transport
on real silicon before committing to a real protocol — see below.

---

## Modbus RTU feasibility on ATmega328PB — evaluation

Not yet decided whether to use Modbus RTU or keep evolving the existing
custom `OrionProtocol`. Here's the resource/timing budget check requested
before that decision gets made:

**Flash (32KB total):** A minimal Modbus RTU slave implementation
(frame parsing, CRC16, function codes 03/06/16 for register read/write)
typically runs 2–6KB depending on library. Combined with this firmware's
existing AS5600 + DRV8833 + calibration logic (alpha01 compiles to a
modest fraction of 32KB on its own), there's comfortable headroom even
after adding a NeoPixel driver and EEPROM address logic.

**RAM (2KB total):** A Modbus RTU ADU is at most ~256 bytes; a feeder's
register table (a few dozen 16-bit registers covering angle, target,
status flags, calibration values, component ID, etc.) is well under 100
bytes. Combined with this firmware's existing state (a few hundred bytes)
and a small NeoPixel buffer (3 bytes/LED, only 1 LED on this board), total
RAM use should land in the hundreds of bytes, not thousands. **Verdict:
comfortable margin, not a constraint.**

**Timing:** Modbus RTU frame-boundary detection needs a 3.5-character
silent-interval timeout. At the 9600 baud this board is already
constrained to (internal 8MHz RC oscillator, ~2% tolerance — see
TestBench04's original notes), 3.5 characters ≈ 4ms — trivially handled
by a timer reset on each received byte, well within an 8-bit AVR's
capability. This is *easier* at 9600 than it would be at higher standard
Modbus bauds (19200+), where the gap timeout gets proportionally tighter.

**The real risk isn't Modbus itself — it's the blocking move.**
`moveToAngle()` runs synchronously for up to `MOVE_TIMEOUT_MS` (6s).
USART0 has only a 2-byte hardware RX buffer. alpha01 still polls
`Serial.available()` from `loop()` (same as every previous bench), so
**any bus traffic arriving while a move is in progress is lost today.**
This isn't specific to Modbus — it would break OrionProtocol traffic
mid-move too — but it becomes a hard requirement once real bus polling
starts, host-side timeouts included. Global interrupts are *not* disabled
for the whole move (only briefly around each NeoPixel write, ~30µs for a
single LED), so an RX-interrupt-driven ring buffer would keep collecting
bytes during a move without needing to touch the deliberately-blocking
motion code. **Recommended before Modbus (or heavier OrionProtocol)
traffic is layered on:** move UART0 RX into an interrupt handler with a
small ring buffer, decoupling byte capture from `loop()`'s polling rate
and from motion blocking.

**Verdict: yes, capable.** Modbus RTU slave + AS5600 I2C polling + DRV8833
PWM motion + a single-LED NeoPixel status indicator all fit comfortably in
flash/RAM. The one prerequisite is interrupt-driven RX (above) — not a
resource limit, just an ordering dependency.

---

## Addressing scheme

The core tension raised: a feeder's live bus address needs to be small
(Modbus RTU unicast addresses are 1 byte, 1–247) and must stay unique
among whatever's currently on the rail, while the number of distinct
**component/part types** a feeder might carry over its lifetime could run
into the hundreds. Those are two different numbers and don't need to
share an address space:

- **Bus address** (this firmware's `feederAddress`, `uint8_t`, 1–247):
  identifies *which physical feeder* is talking on the bus right now.
  With up to ~64 feeders per system (two banks × 32, per the README), 247
  slots is ample headroom.
- **Component/part ID**: just data, stored in a register (or several) on
  the feeder — a 16- or 32-bit value with effectively no size pressure
  from the addressing scheme at all. It never needs to be, and shouldn't
  be, encoded into the bus address.

### Persistent address, not renegotiated per session

`feederAddress` is loaded from EEPROM at boot (`loadAddressFromEeprom()`).
If EEPROM holds a value in 1–247, that becomes the live address
immediately — no re-registration needed. This directly satisfies "insert
a feeder addressed a week ago and it just works": as long as the host
never hands that same number to a *different* feeder while the first one
might still come back, there's no collision. That's a host-side bookkeeping
rule (addresses are sticky per feeder, freed only by an explicit host
action), not something firmware enforces — firmware's job is just to keep
using the same number forever once it has one.

### The "unassigned" problem

A factory-fresh feeder (EEPROM never written, reads back the erased-flash
sentinel `0xFF`) can't safely guess a bus address — plugging into the
shared rail without one would either collide or need bus-wide arbitration
logic. `alpha01` resolves this the same way the project's own physical
design already does, rather than inventing new bus arbitration:

- Firmware maps any invalid/unset EEPROM value to a single reserved
  **`PARKING_ADDRESS` (0xF8 / 248)** — deliberately outside the valid
  Modbus unicast range (1–247), so it can never collide with a real
  assigned address.
- The project's host design already puts a **dedicated programming bus**
  (a separate RS485 line, physically one feeder at a time via the
  programmer slot — see Concept.md and the Feeder Host's 3rd RS485 line)
  ahead of the shared feeder rail. Because only one feeder is ever
  electrically present on that line at a time, every never-assigned
  feeder can safely share the same parking address with zero collision
  risk — there's structurally only one listener.
- `CMD_SET_ADDR` (and the `ADDR <n>` debug command, for bench use without
  a host) only succeeds while `feederAddress == PARKING_ADDRESS` — once a
  feeder has a permanent address, it refuses to be silently reassigned by
  further bus traffic. Re-addressing an already-assigned feeder is left
  as a deliberate, explicit host action to design later, not something
  that happens implicitly.

### Net result

- Bus addresses stay small (1–247) and are handed out once, permanently,
  through the existing one-at-a-time programming slot.
- Unassigned feeders park on one fixed, out-of-range address with no
  arbitration needed, because the mechanical design already guarantees
  only one of them is listening at a time.
- Component ID cardinality (hundreds+) is irrelevant to addressing — it's
  just register data on top of an already-unique bus address.
- A feeder reinserted after any amount of time keeps its address and
  keeps working, provided the host's registry treats addresses as sticky
  rather than reassignable pool slots.

---

## Open questions (not resolved here)

- Modbus RTU vs. continuing/extending `OrionProtocol` — the flash/RAM/
  timing budget above says either is affordable; the choice is now a
  tooling/ecosystem one (Modbus gives off-the-shelf host libraries and
  bus analyzers; OrionProtocol is already partly implemented and
  purpose-fit).
- Interrupt-driven UART0 RX (ring buffer) — needed before real bus
  traffic coexists with blocking moves; not yet implemented in alpha01.
- Host-side address registry / reassignment flow — alpha01 implements the
  feeder side of persistence and refusal-once-assigned; the host's
  bookkeeping (which addresses are in use, how a feeder gets deliberately
  re-parked/erased) isn't designed yet.
- RS485 turnaround timing (`rs485Write()`'s `Serial.flush()` then
  immediate RE-low) hasn't been scoped on a bus analyzer yet — worth
  checking against the MAX1487's datasheet turnaround spec once hardware
  is in hand.

## PlatformIO

PlatformIO only for now — no Arduino IDE `.ino` mirror was carried over
from TestBench04 (add one later if bench-testing without PlatformIO turns
out to be needed).

Same as TestBench04: ISP-only, no bootloader, internal 8MHz oscillator.

```bash
pio run -e atmega328pb_isp -t fuses    # one-time, or after board_hardware.* changes
pio run -e atmega328pb_isp -t upload
```

Debug port (Serial1, 9600 baud) is only reachable through the ISP header
(D11/D12) — see `pins_config.h`. It's mutually exclusive with ISP flashing
on that same header at any given instant.
