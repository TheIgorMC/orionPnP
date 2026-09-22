# beta1

Forked from `alpha03`, which was itself already written ahead of a
hardware revision that didn't exist yet (the ext LED move, AT24CS02).
`beta1` continues that pattern with the actual beta1 schematic additions:
a current-sense input, a 5V-rail-stable RS485 bus-connect relay, and a
motor A direction default flipped for the final board's swapped DRV8833
outputs. Most of this hasn't been on real hardware yet, but one thing has:
the first beta1 bench test found the two DRV8833 channels wired to the
opposite motor connectors from what alpha01-03 assumed — see item 5 below.

## What's different from alpha03

1. **`PIN_I_MON` (A6/PE2)** — analog, the IMON output of a TPS26600 eFuse
   on the 12V rail (RIMON=309kΩ, 1%, per TI's calculator), voltage
   proportional to load current: ~4.07V at the 200mA design max load,
   linear through the origin. `readIMonRaw()` (raw ADC) and
   `readIMonMilliamps()` (converted, through a hand-calibratable point —
   see "Hand calibration" below) are both exposed — `IMON` debug command,
   `iMonMa`/`iMonRaw` in `STATUS`, `iMonRaw` still what goes out over
   `CMD_STATUS_INFO` (mA conversion happens on the receiving end from that
   same raw value). TPS26600's `EN`/`FLT#` pins are not wired to the MCU —
   can't be, since the MCU only runs once the eFuse is already on, so
   there's no fault state where firmware could still be alive to report
   it.
2. **`PIN_5V_READY` (A7/PE3) + `PIN_485_RELAY` (D13/PB5)** — a power-up
   sequencing feature: the 5V rail is monitored via a 4.7k/1k divider until
   it reads stable for `RELAY_READY_STABLE_MS` (500ms), and only then is
   `PIN_485_RELAY` engaged, physically connecting this feeder's RS485
   lines to the shared bus. Point is to stop a feeder that's still
   mid-power-up (or hot-plugged) from electrically disturbing a bus other
   feeders/the host are already using. See `waitFor5vStableAndEngageRelay()`
   in `src/main.cpp` and the "5V rail reading" section below for a real
   electrical gotcha this ran into.
3. **Motor A direction defaults to inverted** (`invertMotorA = true`) —
   the final board's DRV8833 OUT1/OUT2 (motor-output side) are swapped
   relative to the bench units this control loop was tuned against.
   Reusing the existing runtime-flag infrastructure from alpha02/03
   rather than touching `PIN_AIN1`/`PIN_AIN2` (those are MCU-to-driver
   input pins, unaffected by an output-side swap) — see pins_config.h if
   this interpretation of "OUT1/OUT2 swapped" turns out to be wrong.
4. **Boot-time homing is deferred and gated**, not run synchronously from
   `setup()` like alpha02/03's did — see "Boot-time homing gate" below.
5. **`PIN_AIN1`/`PIN_AIN2` swapped with `PIN_BIN1`/`PIN_BIN2`** in
   `pins_config.h`, relative to alpha01-03. Found on the first real beta1
   bench test: commanding "motor A" (the closed-loop feed/sprocket logic)
   moved the peel motor instead — the two DRV8833 channels drive the
   opposite motor connectors from what alpha01-03's pin numbers assumed.
   Different bug from item 3's direction inversion (that's within-channel
   polarity; this is which channel drives which motor at all) — fixed by
   swapping the pin *numbers* in `pins_config.h`, not any logic in
   `main.cpp`: `driveMotorA()` is still "the closed-loop feed motor,"
   `driveMotorB()` still "the open-loop peel motor," just backed by the
   other physical channel now. Both pin pairs use plain `analogWrite()`
   (Timer0 for one channel, Timer1 for the other), so nothing else cares
   which is which. Item 3's `invertMotorA = true` was only ever validated
   against the old (wrong) channel assignment — unverified against this
   fix, re-test direction on real hardware before trusting it.

D13/PB5 (the relay pin) is the same pin `alpha03` freed up by moving
`PIN_EXT_LED` off it. Reusing it for the relay is a deliberate, different
tradeoff than the LED had: the relay is meant to default to disconnected
at reset and during ISP programming anyway, so sharing the ISP header's
SCK line here doesn't cost anything the design didn't already want.

### 5V rail reading: a reference gotcha

`PIN_5V_READY`'s divider taps the same 5V rail that, by default, IS the
ATmega's own ADC reference (AVCC) on this board. Reading a divided-down
5V against a reference that's the same 5V would report the same fixed
ratio (~50%) no matter what the rail's actual absolute value is —
including while it's still ramping up from 0V — so the default reference
genuinely cannot detect "not stable yet" at all here. Fixed by switching
to the ATmega's internal 1.1V bandgap reference just for this read
(`readAdcInternalRef()`), which decouples the measurement from the rail
being measured.

The divider ratio matters against that 1.1V full-scale: a 1k/1k divider
(the original spec) gives ~2.5V at the pin when the rail is healthy,
which clips the reading to max once the rail crosses roughly 2.2V — well
short of 5V, and unable to distinguish a healthy 5V from a sagging-but-
stopped ~3V. Weighed two fixes:

- **10k (rail) / 1k (GND):** ~0.45V at 5V nominal, ~422/1023 — safe up to
  a ~12.1V rail before clipping, but only uses ~41% of the ADC's range.
- **4.7k (rail) / 1k (GND) — what's used:** ~0.88V at 5V nominal,
  ~816/1023 — clips only above ~6.27V rail, comfortably clear of a 5V
  rail's normal tolerance, while using ~80% of the ADC's range: roughly
  2x the resolution of the 10k/1k option. There's no realistic scenario
  on a "5V" rail that needs headroom all the way to 12V, so the extra
  resolution is worth taking.

No firmware logic needed to change for this — `waitFor5vStableAndEngageRelay()`'s
stability check compares consecutive raw ADC readings to each other (a
plateau detector), not against a hardcoded voltage, so it works with
whatever ratio is populated as long as it doesn't clip too early. Only
the comments/constants describing the expected reading needed updating.

### Hand calibration

The datasheet/calculator math above gives a good starting point, but the
real scale of both signals depends on the actual resistor/RIMON tolerance
populated on a given physical board — 1% parts, but still not exact.
`AnalogCalibration` stores a single `(raw ADC, real-world value)` point
per signal — `imonCalRaw`/`imonCalMa` and `v5vCalRaw`/`v5vCalMv` — and both
conversions (`readIMonMilliamps()`, `read5vRailMillivolts()`) are linear
through the origin from that point, which is true of the underlying
hardware either way (a resistor divider, and a current-mirror IMON
output). Lives in the ATmega's own internal EEPROM, separate from both
`FeederConfig` and `FeederHardwareInfo` — same reasoning as
`FeederHardwareInfo` already has for not being touched by a component
change or `RESETCFG`, just for a different flavor of "this shouldn't
reset": it's bench/electrical calibration for this board's analog
frontend, not tape-handling identity, so it doesn't need the AT24CS02 —
an EEPROM read/write already works before any I2C peripheral is even
populated.

Debug-port-only commands (bench/jig use, same category as `RELAY`/`IMON`/
`5VSTATUS` — deliberately no bus opcodes, since a feeder that still needs
calibrating wouldn't have a working bus link to reach anyway):

- **`CALI <mA>`** — capture the current `PIN_I_MON` raw ADC reading
  against a real load current read off a bench ammeter right now.
- **`CALV <V>`** — capture the current `PIN_5V_READY` raw ADC reading
  (internal 1.1V reference) against a real 5V-rail reading off a bench
  multimeter right now.
- **`CALSTATUS`** — print both stored calibration points.
- **`CALRESET`** — revert both to the factory-calculated defaults
  (`IMON_CAL_DEFAULT_*`/`V5V_CAL_DEFAULT_*` constants, derived from the
  same numbers as the sections above).

Meant for once a bed-of-nails test jig exists to make "apply a known
load/rail voltage, read a real meter, run one command" fast and
repeatable across boards, rather than trusting the nominal/datasheet math
forever.

### Estimating 5V-rail current (debug only)

There's no direct current sense on the 5V rail — only IMON on the 12V
input side. `estimateI5vMilliamps()` gives a rough stand-in from a simple
power balance: `i5vEst = (12V_nominal * iMonMa) / v5vActualMv`, i.e.
"assume the whole 12V input current is being converted down to 5V." That
assumption actually holds well here: the DRV8833's `VMOT` is tied to the
5V rail (through a ferrite bead for noise isolation), not 12V, so the
motor is itself a 5V-rail load rather than something bypassing the
regulator — there's no significant 12V-only load on this board to throw
the balance off. The one thing the estimate does ignore is buck
conversion efficiency (assumes ~100%), so the true 5V-rail current is
somewhat *lower* than `i5vEstMa`, not higher. Good enough as a "does this
look roughly sane" bench check (`I5V` debug command, `i5vEstMa` in
`STATUS`/`5VSTATUS`), not a real measurement — if that's ever needed,
it'd want its own current-sense hardware on the 5V rail.

### Boot-time homing gate

alpha02/03 called `calibrateZero()` (the DRV8833 still/breakaway duty
characterization — the only motor movement this firmware ever does on its
own, unprompted by a host or debug command) synchronously from `setup()`,
right at boot. beta1 defers it to a `checkHoming()` gate polled from
`loop()` instead, for two reasons that only started to matter once real
hardware — a shared 12V rail feeding multiple feeders through individual
TPS26600 eFuses, hot-pluggable while the bus is live — entered the
picture:

- **Boot-insertion PSU settle + per-feeder stagger.** `HOMING_BOOT_DELAY_MS`
  (1000ms) has to elapse, plus a random `HOMING_BOOT_JITTER_MAX_MS` (up to
  2000ms, drawn from the same `random()` call `seedSessionNonce()` already
  reseeds) before homing is even attempted. The fixed part gives this
  board's own insertion inrush (TPS26600 soft-start, bulk cap charging)
  time to settle before a motor breakaway-current spike stacks on top of
  it; the random part matters on a bus where several feeders get powered
  up together and would otherwise all hit this gate within milliseconds of
  each other — without the jitter, they'd all draw their breakaway current
  spike from the shared rail at the same instant. Same idea as
  `DISCOVERY_JITTER_MAX_MS`, just a much wider window: that one only has
  to avoid a bus-reply collision, this one has to avoid a PSU current
  spike across a whole populated bus.
- **Magnet-placement stability.** Once `magnetDetected()` goes true, it
  has to stay true continuously for `HOMING_MAGNET_STABLE_MS` (5000ms)
  before homing fires — any dropout resets the timer. Covers a magnet
  placed *after* the board's already running (bench test, wheel/sprocket
  dropped on mid-session) that might still be settling into position when
  first seen; a single "detected this instant" read was too easy to catch
  mid-placement. No magnet at all just means the timer never starts, so
  homing never fires for that feeder — `calibrateZero()` already falls
  back to defaults without moving the motor when ungated, but checking
  here first means a feeder with nothing to calibrate against doesn't sit
  through the boot-delay wait for no reason either.

`checkHoming()` runs once per `loop()` iteration and is a no-op after
`homingDone` latches true (at most once per boot). It's called right after
the `PIN_nFAULT` check clears, so a fault condition also holds off homing.

### Debug self-test: relay + ext LED + IMON/5V readout

`runDebugSelfTest()`, called from `setup()` right after `loadAnalogCal()`
and right before `waitFor5vStableAndEngageRelay()`, exercises beta1's new
GPIO-driven hardware with a visual RGB cue each — a bench aid so
`PIN_485_RELAY`/`PIN_EXT_LED` can be confirmed by eye (and ear) without a
meter: relay ON/OFF ×2 (RGB green/red, `SELFTEST_RELAY_HOLD_MS=800`ms
each — the original single 300ms cycle was too quick to reliably hear
the click over) → ext LED ON (RGB blue) → ext LED OFF → an IMON/5V
calibration readout (raw + calibrated value for both, not a pass/fail
check — there's no expected value without a real load/meter attached,
just something to eyeball). `loadAnalogCal()` has to run first or the
readout divides by zero (`analogCal` defaults to all-zero). Always leaves
the relay and ext LED off when it returns — a throwaway bench toggle,
not the real power-sequencing relay engage, which happens right after
via `waitFor5vStableAndEngageRelay()` and starts from that same
known-off state either way.

Runs once at boot unconditionally, and again on demand via the
`SELFTEST` debug command — re-running it disconnects a live bus link
(ends with the relay forced off) until `RELAY ON` or a reboot
re-engages it, same caveat `RELAY ON`/`OFF` already have.

### Relay button-test mode

`runDebugSelfTest()`'s relay portion is a fixed 800ms×2 auto-cycle -
useful as a quick boot-time sanity check, but not great for actually
standing there listening to confirm the click, since it only runs once
and on its own schedule. `runRelayButtonTest()`, called right after
`statusLed.begin()`/before `showStartupLedSequence()` in `setup()`,
covers that instead: hold either `SW1` or `SW2` while powering up, and
it takes over indefinitely rather than returning — RGB goes fixed blue
to confirm entry, then once the boot-time button press is released and
debounced, goes red with the relay forced off (starting state). From
there, every `SW1`/`SW2` press toggles the relay and the RGB color
(green=on/red=off) for as long as wanted. No exit path back to normal
operation short of a power cycle without holding a button - this is a
dedicated bench mode, not something meant to hand back into the boot
sequence. If neither button is held when it's called, it returns
immediately and changes nothing, so a normal boot is unaffected.

Needed `Serial1.begin(DEBUG_BAUD)` moved to the very top of `setup()`
(was after `Wire.begin()`) so this mode's prints work - it has to be
checked well before setup() would otherwise reach that line.

---

# Inherited from alpha03: PIN_EXT_LED move, AT24CS02

Forked from `alpha02`. Where `alpha01`→`alpha02` was purely a firmware
change (same board, closed-loop feedback restored once a magnet was
mounted), `alpha03` is the first firmware written **ahead of a hardware
revision that doesn't exist yet** — it's what beta1's schematic should be
built to match, not a description of any board that's actually been
populated so far.

## What's different from alpha02

1. **`PIN_EXT_LED` moves from D13/PB5 to A3/PC3.** The old pin shared the
   ISP header's SCK line — mutually exclusive with flashing at any given
   instant. A3/PC3 is unused/unpopulated on every V0.2a board built so far
   (it carried the old optical-interrupter signals before the switch to
   the AS5600), and isn't shared with anything else. Still a **standard
   LED, plain on/off, direct GPIO drive** — ~20mA is comfortably inside the
   ATmega328PB's 40mA absolute-maximum per-I/O-pin rating (Microchip
   datasheet), the same current class as, e.g., the Arduino Uno's own
   onboard LED, so no transistor is needed. Same command interface as
   before (`CMD_SET_EXT_LED` / `LED ON`/`OFF`), unchanged. (This pin
   briefly carried a dedicated SK6812 pixel instead of a plain LED during
   initial beta1 planning — reverted once a standard LED was chosen, before
   any hardware existed either way, so nothing built was ever affected.)
2. **AT24CS02 support** — an I2C EEPROM plus a factory-programmed,
   read-only 128-bit unique serial number, sharing the existing I2C bus
   with the AS5600 (different address, no new pins). `FeederHardwareInfo`
   (tape width) moved from the ATmega's internal EEPROM onto this chip's
   EEPROM — same reasoning as splitting it out of `FeederConfig` in the
   first place, one level further: "what this unit physically is" should
   survive even a full chip-erase/reflash of the ATmega. `CMD_GET_SERIAL`
   (`SERIAL` on the debug port) reads the factory serial. See
   `../PROTOCOL.md` for the full command reference.

Neither of these is on any board built so far. Every AT24CS02 access
degrades gracefully (checked `endTransmission`/`requestFrom`, same defensive
pattern as the AS5600 code) if the chip isn't actually populated — this
firmware should still boot and run motion/RS485 normally on a plain V0.2a
board, just with hardware-info/serial queries coming back empty/`NACK`.
None of this has been validated against real AT24CS02 silicon yet.

---

# Inherited from alpha02: closed-loop feedback, runtime motor direction

Forked from `alpha01` (commit `73e7448`, "fix status LED timing/color, add
motor B + magnet-detect debug aids"). That commit had temporarily rewired
SW1/SW2 to jog motor A/B open-loop, purely because `moveToAngle()` refuses
to run without `magnetDetected()` and no magnet was mounted on the bare
test board yet. `alpha02` exists for the next stage: **a board with a
magnet actually mounted, so the AS5600 closed-loop feedback path can be
exercised and validated for real** — not just bring-up sanity checks.

## What's different from alpha01

1. **SW1 is closed-loop again.** Back to `commandMoveTo()`/`moveToAngle()`
   (+1 tooth per press) instead of the open-loop timed jog — this is the
   actual thing worth testing now that a magnet exists: stall detection,
   timeout, fault handling, and the AS5600 angle read all get exercised
   end to end, not bypassed.
2. **SW2 stays open-loop**, unchanged from `alpha01`. This isn't a
   shortcut — motor B (peel) has no encoder on this board at all, so
   there's no closed-loop behavior to "restore" for it. It's still a
   plain timed jog for wiring/driver-channel sanity checks.
3. **Motor direction is now a runtime flag, not a compile-time constant.**
   Neither motor's lead polarity relative to this firmware's notion of
   "forward" has been confirmed against real wiring yet. `invertMotorA`/
   `invertMotorB` (independent per motor) can be flipped from the debug
   port (`INVERTA ON/OFF`, `INVERTB ON/OFF`) or the bus
   (`CMD_SET_INVERT_DIR`, payload `[motor(0=A,1=B), state(0/1)]`) without
   reflashing or re-soldering. Both default to `false` (matching the old
   `INVERT_DIRECTION` default) and reset to that on every reboot — this is
   a bring-up convenience, not yet persisted to EEPROM. If it turns out to
   be a fixed characteristic of every unit of this design (e.g. a
   consistent connector/lead convention issue rather than per-unit
   variance), it's a good candidate to hardcode as a constant later, the
   same way `PICK_OFFSET_MM` was pulled out of the tape-zero calibration —
   see that section of this doc for the reasoning pattern. `printStatus()`
   (`STATUS`/`WHOAMI`) shows the current `invertA=`/`invertB=` state.

4. **Full RS485 packet support**, not just enough to validate transport.
   Now that alpha02 is headed for an actual live PnP test rather than
   staying on the bench, the placeholder command set grew into what a real
   host actually needs: `CMD_GET_STATUS` (live angle/magnet/fault/last-
   error telemetry), `CMD_STOP` (remote e-stop, both motors), `CMD_IDENTIFY`
   (blink the status LED so an operator can find a physical unit by bus
   address), and proper `ERR_*` codes in `CMD_NACK` payloads instead of a
   bare failure (so a host can log *why* a feed failed — stall, timeout,
   fault, magnet lost, not-yet-calibrated — instead of just "didn't
   work"). See `../PROTOCOL.md` for the full, current opcode table and
   error code list — that file is now the source of truth for the wire
   protocol, not this doc, so it doesn't drift across two copies.
5. **Tape width is now a stored hardware-identity parameter**
   (`FeederHardwareInfo.tapeWidthMm`, `CMD_GET/SET_HW_INFO`,
   `SETWIDTH <mm>` debug command). Deliberately a separate EEPROM struct
   from `FeederConfig`, with no reset path — see `../PROTOCOL.md` for why
   this is fixed-per-unit data (mechanical, set once at assembly) rather
   than per-component data, and does not affect the sprocket-feed math
   at all (EIA-481 hole pitch is a fixed 4mm regardless of tape width) -
   it exists purely so a host can validate reel/feeder compatibility and
   for fleet inventory. Included in the `CMD_DISCOVER_HERE` reply.

Everything else below (RS485 transport, addressing/discovery, tape-zero/
distance-based motion, EEPROM component config, Modbus feasibility) is
unchanged from `alpha01` and documented here for completeness, since
`alpha02` carries the whole file forward rather than diffing against it.

---

Same ATmega328PB-AU target, same
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

- **"OUT1/OUT2 swapped" interpretation** — turned out to be two separate,
  real issues rather than one or the other: `invertMotorA = true` (net
  direction inversion) is still believed correct for the bench-vs-final
  polarity difference it was meant for, AND the first real beta1 bench
  test separately found `PIN_AIN1`/`PIN_AIN2` needed to swap with
  `PIN_BIN1`/`PIN_BIN2` (a channel-level swap - commanding motor A was
  moving the peel motor). The pin swap is done in `pins_config.h`, but
  `invertMotorA`'s correctness was only ever validated against the old
  channel assignment - **not yet re-confirmed against the pin swap on
  real hardware.** Re-test direction next; flip `invertMotorA` to `false`
  if it now moves the feed motor backwards.
- **A6/A7 = PE2/PE3 not verified** against MiniCore's actual
  ATmega328PB `pins_arduino.h` — no toolchain/package cache available in
  this environment to check. If MiniCore numbers the extra PORTE pins
  differently, only `PIN_I_MON`/`PIN_5V_READY` in `pins_config.h` need to
  change.
- **`PIN_485_RELAY` polarity** (`RELAY_ACTIVE_HIGH = true`) — confirmed:
  a low-side NMOS with a pull-down (GPIO → gate, source → GND, coil
  between drain and supply), so HIGH = on = relay energized matches the
  actual board.
- **5V-rail stability check resolution** — see "5V rail reading: a
  reference gotcha" above; the 4.7k/1k divider gives ~2x the resolution of
  a 10k/1k alternative and doesn't clip until well above 5V, but it's
  still a plateau detector, not an absolute-voltage comparator — a rail
  that stalls partway up would still read as "stable" at whatever level
  it stalled at.
- **PCB has the `PIN_5V_READY` divider resistors backwards** (confirmed
  on the real board) — should be 4.7k rail-side / 1k GND-side, matching
  every default/comment in this codebase; swapping them the other way
  (1k rail-side / 4.7k GND-side) would put ~4.1V at the pin, clipping the
  1.1V-referenced ADC for anything above a ~1.33V rail and making the
  stability check nearly useless. **PCB fix, not firmware** — no code
  change needed once the resistors are corrected.
- **`RELAY_READY_TIMEOUT_MS` (5s) behavior on timeout** — firmware
  continues booting normally with the relay left disconnected and logs a
  warning, rather than retrying or halting. Reasonable default for now;
  no automatic retry-later logic exists if the rail stabilizes after the
  timeout gives up.
- `CMD_STOP` brakes motors but doesn't clear/report a fault condition —
  if `PIN_nFAULT` is still asserted after a stop, the very next move will
  immediately fail with `ERR_FAULT` again. That's arguably correct (don't
  silently paper over a real fault), but a host driving this live should
  know to distinguish "stopped, ready to move again" from "stopped because
  something is actually wrong" via `CMD_GET_STATUS`'s `faultActive` field.
- `hwInfo` moved to the AT24CS02 in alpha03 (byte offset `0x00` in its
  EEPROM); this note is stale for alpha03 but kept for alpha02's own
  history — `FeederConfig` still lives in the ATmega's internal EEPROM
  either way.
- AT24CS02 support (both the general EEPROM and the factory serial page)
  is written but **not validated against real silicon** — no beta1 board
  exists yet to test the address assumptions (`0x50`/`0x58`), the
  identification-page read (`memAddr 0x00`, 16 bytes), or the fixed
  5ms write-cycle delay (no ack-polling implemented) against.
- `invertMotorA`/`invertMotorB` are RAM-only (reset to their compiled-in
  defaults - `true`/`false` on beta1 - every reboot) — once real hardware
  confirms whether either needs inverting, decide whether that's per-unit
  variance (persist to EEPROM) or a fixed design characteristic (hardcode
  as a constant, like `PICK_OFFSET_MM`).
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

Or `./flash.ps1` (PowerShell): build → burn fuses → chip erase → settle
delay → write + verify flash. `-SkipFuses`/`-SkipErase` for routine
reflashes once fuses are already correct on a given chip. Erase and
flash go through `avrdude` directly rather than `pio -t erase`/
`pio -t upload` — matching the actual hand-run bench commands for this
board:

```
avrdude -C <conf> -c usbasp -p m328pb -e
avrdude -C <conf> -c usbasp -p m328pb -D -U flash:w:<hex>:i -U flash:v:<hex>:i
```

(`-e` standalone erase; `-D` on the flash step skips avrdude's own
implicit auto-erase since it was just erased explicitly; the second `-U`
verifies the write.) The settle delay matters because a fuse write is
followed by an avrdude-issued target reset, and fuse bits that affect the
clock (CKDIV8/oscillator selection) only take effect from that reset
onward — hitting the target with another ISP transaction immediately
after can race it. Fuses themselves still go through `pio -t fuses`, not
a raw avrdude call — no hand-run fuse-burn command for this board has
turned up yet to match against.

Debug port (Serial1, 9600 baud) is only reachable through the ISP header
(D11/D12) — see `pins_config.h`. It's mutually exclusive with ISP flashing
on that same header at any given instant.
