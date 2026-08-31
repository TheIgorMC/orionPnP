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
`CMD_DISCOVER`/CRC8) is **explicitly a placeholder**, not Modbus. It exists
to validate RE/DE switching timing, addressing, and byte-level transport
on real silicon before committing to a real protocol — see below.

**Revision note:** the addressing scheme below is v2, replacing the
EEPROM-persistent-address + dedicated-programming-bus design from the
first version of this document. The dedicated programming bus was
dropped from the project (feeders now program over the same shared rail
they run on), which broke the collision-free assumption that design
relied on — see "Addressing scheme" for the replacement.

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

## Addressing scheme (v2 — no dedicated programming bus)

The project dropped the dedicated one-at-a-time programming bus — feeders
now program over the same shared rail they run on. That removed the
physical guarantee the first version of this scheme leaned on (only one
unassigned feeder ever electrically present at a time), so addressing had
to be redesigned around three separated concerns instead of one:

1. **Bus address** — low-level, RS485-only, exists so the host can talk to
   one feeder without others answering.
2. **Component/part ID** — which OpenPnP part (`openPnP/parts.xml` `id`,
   currently small integers, comfortably fits a `uint16_t`) is loaded
   right now.
3. **Feed calibration** — where "ready" is on the currently-loaded tape,
   and how far to advance per pick. Only meaningful for the *current*
   component, not the feeder in the abstract.

Conflating any of these (e.g. routing part identity through the bus
address, as OrionProtocol's assign-to-`0x0000` flow effectively does) is
what made 247 addresses feel scarce. Kept separate, none of them are:

### Bus address: disposable, RAM-only, re-earned every boot

`busAddress` starts at `ADDR_UNASSIGNED` (0x00) on every boot — nothing
about it is read from or written to EEPROM. A feeder gets a real address
by discovery:

1. Host broadcasts `CMD_DISCOVER` (addr 0x00).
2. Every still-unassigned feeder waits a random jitter delay
   (`DISCOVERY_JITTER_MAX_MS`, up to 200ms) before replying with
   `CMD_DISCOVER_HERE`, carrying a random per-boot `sessionNonce` *and*
   its persisted `componentId` — so the host immediately learns what's
   loaded without a separate round-trip, when that's already known.
   Jitter is why this doesn't need collision-free bus arbitration
   hardware (RS485 can't do CAN/1-Wire-style bitwise arbitration): if two
   unassigned feeders' replies collide, the host just sees a bad CRC and
   re-polls; each round's jitter is independently random, so given a few
   rounds every feeder eventually gets a clear slot.
3. Host picks a free small address and broadcasts `CMD_ASSIGN_ADDR` with
   `[nonceHi, nonceLo, newAddr]`. Only the feeder whose `sessionNonce`
   matches adopts `newAddr` — everyone else ignores it, so this is safe
   to broadcast even while other feeders are mid-discovery.
4. Feeder ACKs under its new unicast address, confirming.

Because this repeats from scratch every boot, **reinserting the same
feeder gets it a (possibly different) address, and that's fine** — nothing
durable was ever tied to the number. This is what makes a 1-byte
(1–247) address workable again despite there being no exclusive
programming slot: the address is cheap enough to throw away and redo
every single time.

`SIMADDR <n>` (debug port) force-sets `busAddress` locally without going
through discovery, for bench-testing motion/config commands with no host
on the bus yet — not part of the real flow.

### Component ID + feed calibration: EEPROM, sticky, explicit

Unlike the address, these are exactly what should survive a power cycle —
`FeederConfig` (`componentId`, `tapeZeroRaw`, `feedHalfTeeth`, `crc`) is
read from EEPROM at boot (`loadConfig()`) and reset to a fully-unset state
only if the CRC doesn't check out (factory-fresh board).

The intended host flow after discovery:

- If the `CMD_DISCOVER_HERE` reply's `componentId` is already set (same
  reel as before power-off/reinsertion) → host already knows what's
  loaded and can fetch `tapeZeroRaw`/`feedHalfTeeth` via
  `CMD_GET_COMPONENT` and resume immediately. No operator involvement.
- If `componentId` is `COMPONENT_ID_UNSET` (fresh feeder, or after a
  reset) → host prompts "what's loaded here?", writes the answer via
  `CMD_SET_COMPONENT`, and that kicks off calibration (jog to the first
  pocket, set `feedHalfTeeth` for this reel's pitch) via
  `CMD_SET_FEED_CONFIG` before the feeder is usable.

`setComponentId()` deliberately wipes `tapeZeroRaw`/`feedHalfTeeth`
whenever the *value* changes (not on every call — writing the same id
back, e.g. confirming "still the same reel" after a replug, leaves
calibration untouched). A stale zero/step size silently carried over from
a different component would be worse than forcing a visible recalibration
prompt. `CMD_RESET_CONFIG` (`RESETCFG` on the debug port) clears just the
calibration, independent of a component-id change, for redoing a bad
calibration on the same reel.

`feedHalfTeeth` is **one field**, not three separately-stored presets —
2 = standard EIA-481 4mm/1-tooth sprocket pitch, 1 = 2mm "fine" pitch,
anything else = "custom" for wider-pitch reels (8/12/16/24mm...). "Raw/
fine/custom" are just named values a host UI might offer as shortcuts for
what to write here, not three parallel storage slots — a loaded reel only
ever has one active pitch at a time.

This is also explicitly separate from `calibrateZero()`/`stillDutyMax`
elsewhere in this file — that's DRV8833 motor-duty characterization
(electrical, unrelated to which tape is loaded) and still reruns on every
boot regardless of component. Two different things named similarly by
coincidence; don't conflate them.

### Distance-based motion (mm in, steps out)

`tapeZeroRaw`/`feedHalfTeeth` are now wired into actual motion, not just
stored. The chain is: **mm → degrees → the existing `moveToAngle()`
closed-loop mover** (stall/timeout/fault handling unchanged, none of it
duplicated) — nothing above the mm-based helpers needs to know a step is
9°, or that a sprocket hole is a specific angle at all:

- `degForMm()`/`mmForDeg()` — the base conversion, from
  `DEG_PER_MM = DEG_PER_TOOTH / SPROCKET_HOLE_PITCH_MM` (2.25°/mm — EIA-481
  fixes sprocket holes at 4mm regardless of tape width, and the wheel
  geometry from the TestBench03/04 bench tests gives 9°/tooth, so this is
  just those two known constants combined).
- `halfTeethForMm()` — rounds a physical pitch (mm) to the nearest
  half-tooth (2mm, the finest step this wheel resolves), used by
  `setFeedPitchMm()` so a host/operator only ever specifies a pitch in mm
  (`PITCH 4`, `CMD_SET_PITCH_MM`) and never touches a tooth count directly.
- `setTapeZeroHere()` — captures the current AS5600 position (as a 12-bit
  count, `angleDegToRaw12()`) into `cfg.tapeZeroRaw` once the operator has
  jogged the wheel so the sprocket hole belonging to the reel's first real
  pocket is seated. `ZEROHERE` / `CMD_ZERO_HERE`.
- `moveByMm()` — relative move by a physical distance from wherever the
  wheel currently is. `MOVEMM <mm>`.
- `moveToTapeZeroPlusMm()` — absolute move to zero + `PICK_OFFSET_MM` + an
  mm offset (e.g. the Nth pocket = zero + N×pitch). `GOMM <mm>` /
  `GOTOZERO` (mm=0).
- `feedOnePitch()` — advance by exactly the configured `feedHalfTeeth`,
  what a real pick sequence calls between picks. `FEED` / `CMD_FEED_NEXT`.

### Tape zero calibration, v2 — separating the mechanical constant from the per-reel variable

`ZEROHERE` originally captured one number that conflated two physically
different things: (a) which sprocket hole is seated, and (b) the fine
mechanical distance from a seated hole to where the camera/nozzle actually
picks. (b) is geometry of the PCB/frame — identical on every feeder of
this design — while (a) is the only thing that actually differs per reel
(leader tape length before the first real component). Redoing a camera
fine-tune every time you load a new reel was redoing (b) for no reason.

Fixed now: **`PICK_OFFSET_MM`** (near the wheel-geometry constants) holds
(b) as a single hardcoded constant, shared by every feeder running this
firmware. It is **not** part of `FeederConfig`/EEPROM — it isn't per-unit
or per-component data, it's a firmware-wide build constant. `tapeZeroRaw`
now stores only (a) — a pure "which hole" reference — and
`moveToTapeZeroPlusMm()` adds `PICK_OFFSET_MM` automatically at move time,
so `GOTOZERO`/`GOMM` always land at the true pick point without it being
baked into the stored zero value.

**Practical effect on the calibration flow:**
- **One-time, ever, on a reference unit:** use `MOVEMM` with a camera to
  find the exact mm offset from a seated hole to the real pick point,
  hardcode that as `PICK_OFFSET_MM`, reflash. Every feeder built to this
  design shares it.
- **Per reel, routinely:** load a reel → `COMPONENT <id>` (resets zero/
  pitch if it's a different component) → jog with `STEP`/`T` (whole/half-
  tooth increments only — no camera, no `MOVEMM`) until the first
  pocket's hole is seated → `ZEROHERE` → `PITCH <mm>` for this reel's
  pitch → ready. `FEED` advances one pick at a time from here, and
  `tapeZeroRaw`/`feedHalfTeeth` persist across power cycles for as long
  as `componentId` doesn't change.

`PICK_OFFSET_MM` currently defaults to `0.0` (not yet measured on real
hardware) — see the `TODO` comment at its definition.

---

## Open questions (not resolved here)

- `PICK_OFFSET_MM` is a placeholder (`0.0`) — needs measuring once with a
  camera on real hardware, per "Tape zero calibration, v2" above.
- Modbus RTU vs. continuing/extending `OrionProtocol` — the flash/RAM/
  timing budget above says either is affordable; the choice is now a
  tooling/ecosystem one (Modbus gives off-the-shelf host libraries and
  bus analyzers; OrionProtocol is already partly implemented and
  purpose-fit). The discovery scheme above isn't Modbus-specific and
  would need adapting either way (Modbus has no native discovery
  concept — this'd sit as a pre-step before switching into Modbus
  framing, or inform a custom protocol directly).
- Interrupt-driven UART0 RX (ring buffer) — needed before real bus
  traffic coexists with blocking moves; not yet implemented in alpha01.
  Also now needed for `handleFrame()`'s `delay(random(...))` jitter
  during discovery, which blocks byte processing for up to 200ms — fine
  with polling today since nothing else needs the CPU then, but worth
  revisiting once RX is interrupt-driven.
- Discovery round timing/retry policy (how often the host re-polls, how
  many rounds before giving up on a round with a collision) isn't
  designed — alpha01 implements the feeder side of one exchange, not a
  host-side discovery loop.
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
