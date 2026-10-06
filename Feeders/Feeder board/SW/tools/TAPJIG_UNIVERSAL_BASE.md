# TAP-Jig universal base: considerations

Status: **concept notes, nothing designed or built.** Written from a design
discussion; every number below is a starting proposal to challenge, not a
requirement. The current feeder jig (see `TAPJIG.md` and the spec doc
"OrionPnP Feeder Programming & Test Jig") is the first product to use this
idea, not the finished universal base.

## Idea

One **universal base** that offers a fixed, generous set of named hardware
resources, and **per-product fixtures** (a pogo adapter plus any ancillary
parts: motors, LDRs, sensors, servo) that simply connect product signals to
known base pins. What a pin *does* is chosen in configuration, not in
hardware. The test routine talks about **signals** (`SW1`, `V5`,
`FIBER_LDR`), a fixture profile maps signals to base resources, and a jig
profile says what the base can do.

```
Pi (UI, routine, logs, avrdude/OpenOCD) --USB--> hub --> base controller (real time, safety)
                                                    \--> programmers (USBasp, J-Link, ...)
base controller --> power rails, analog, digital I/O, buses --> fixture connector
                                                                  --> pogo board --> DUT
                                                                  --> ancillary (motors, LDRs, sensors, servo)
```

Fixture = pogo board + mechanics + ancillary parts + an ID EEPROM. Swapping
products = slide the fixture in, clamp it, select (or auto-detect) the
product.

## Is "bigger and universal" the right call?

Yes if more than one product will be tested, with caveats:

- It moves variability from hardware to config. That is the point, and it is
  what makes a second product cheap.
- It is a real project: more parts, more protection, more firmware. Build the
  current feeder jig first and keep its pin names and fixture connector
  convention compatible with this, rather than waiting for the universal base.
- The 32u4 will not fit a base this size (pins, peripherals). Pick the
  controller for the budget below (RP2040/RP2350, an STM32, or a larger AVR),
  or let the Pi drive I2C/SPI expanders directly and keep only the real-time
  and safety work (watchdog, current limits, capture timing) on a small MCU.
- **Alternative worth considering:** a backplane with plug-in cards (power
  card, digital I/O card, analog card, comms card) instead of one big board.
  Same resources, but you build and debug one card at a time and only
  populate what a product needs.

## Resource budget (proposal)

| Resource | Proposal | Notes |
|---|---|---|
| Power rails to DUT | **5 V, 12 V, 24 V**, "beefy" (several A each) | each switchable, with soft start, hardware overcurrent limit (programmable), current and voltage sense, discharge on off; Kelvin (4-wire) sense out to the fixture |
| Aux 3.3 V | on signal pins only | low current; external 3.3 V loads are expected to be few and small |
| Logic reference | per-bank Vio (1.8 / 3.3 / 5 V), set by the fixture | all digital I/O follows it |
| Digital I/O | 64 lines via several expanders | each line: input / push-pull / open-drain low / weak pull-up or pull-down; series resistor and clamp at the connector |
| Analog inputs | 16 channels, 16-bit class ADCs | fixture provides scaling; base measures a clean range (e.g. 0-5 V and a 0-2.5 V precision range) |
| Analog outputs | 2-4 DAC channels | thresholds, bias, simulated sensors |
| UART | 4 TTL, 2 RS-485 (switchable termination), 1 CAN | named ports, configurable baud |
| I2C | 2 buses + a mux | pull-ups selectable, Vio-referenced |
| SPI | 2 buses with chip selects | |
| Programming | **SWD** (J-Link or CMSIS-DAP/ST-Link) **and AVR ISP** (USBasp) | programmers are USB devices on the Pi hub; base only routes their lines (and RESET) to fixture pins |
| PWM / servo | 4 outputs | magnet arm, motor speed |
| Timer capture / counters | 4 inputs | encoder timing, frequency |
| Optical/other sensors | via analog inputs | LDRs live on the fixture |
| Button/reset forcing | digital lines in open-drain low mode | replaces dedicated MOSFETs per product |
| Fixture ID | I2C EEPROM on the fixture | read on insertion; holds fixture type, revision, serial and a profile pointer |
| Interlock | "fixture seated and closed" input | no rail enables without a valid ID and a closed fixture |

J-Link note: the cheap EDU/EDU Mini variants are non-commercial licence only.
For production use buy the right licence or use an alternative probe
(CMSIS-DAP, ST-Link) driven through OpenOCD/pyOCD.

## Fixture connector and pin naming

- One rugged connector (board-to-board or card edge), multiple contacts per
  power line, locating dowels, keyed so it cannot be inserted rotated.
  Order of magnitude: 150-200 contacts for the budget above; a bigger number
  argues for the backplane idea.
- Base pins have **fixed names**: `D00..D63`, `A00..A15`, `AO0..AO3`,
  `UART0..UART3`, `RS485_0..1`, `I2C0..1`, `SPI0..1`, `SWD`, `ISP`,
  `PWM0..3`, `CAP0..3`, `P5V`, `P12V`, `P24V`, `V3V3`, `GND`...
- A signal that needs more than the base can give (very high current, a
  special analog front end) stays on the fixture, not in the base.

## Configuration model

Three layers, all data, none of it code:

1. **Jig profile** (the base): what exists and its limits: rails and current
   limits, ADC ranges, counts of each bus, supported commands. The base
   reports its type and revision in `HELLO`.
2. **Fixture profile** (stored on or alongside the fixture): which base pin
   carries which product signal, and in what mode.
3. **Product package** (routine, test and production firmware, fuses/option
   bytes, per-unit options): refers to **signals only**, never to base pins.

Illustrative fixture profile fragment (format not fixed):

```json
{
  "fixture": "feeder_v03_cradle", "rev": 1, "jig": "universal_v1",
  "rails": { "VIN": {"rail": "P12V", "limit_mA": 300}, "V5": {"sense": "A02"} },
  "signals": {
    "SW1":   {"pin": "D07", "mode": "od_low"},
    "SW2":   {"pin": "D08", "mode": "od_low"},
    "RESET": {"pin": "D09", "mode": "od_low"},
    "FAULT": {"pin": "D10", "mode": "in", "active": "low"},
    "FB":    {"pin": "A00", "range": "2.5V", "scale": 1.0},
    "VIN_S": {"pin": "A01", "range": "5V", "scale": 4.3},
    "RGB_LDR": {"pin": "A08", "range": "5V"},
    "SERVO": {"pin": "PWM0"},
    "BUS":   {"port": "RS485_0", "baud": 9600}
  },
  "programmer": { "type": "avr_isp", "lines": {"MOSI": "D20", "MISO": "D21", "SCK": "D22", "RESET": "D23"} }
}
```

The routine then says `jig.sw SW1 pressed` or `jig.adc FB`; the engine looks
the signal up in the fixture profile. The current routine already uses names
rather than pins, which is why this is a small step.

## Safety and power (the big difference from the feeder jig)

The feeder jig's spec chose software-only fault detection because the
currents are small and a pre-power resistance check screens out dead shorts.
That reasoning does not carry over to several-amp 24 V rails:

- hardware current limit and trip per rail (hot-swap controller / eFuse
  class), software only sets and reads it;
- hardware watchdog that drops every rail if the controller or the USB link
  stops talking;
- the interlock above, plus a physical e-stop that cuts the rails
  independently of firmware;
- pre-power cold-resistance check on every rail the product uses, soft start,
  and a discharge path so a board is not handled while a rail is still up;
- latched faults that need an explicit re-arm, as in the feeder jig;
- ESD protection and series resistance on every connector line, since pogo
  contacts are touched by people and by boards.

## Software impact (see the TODO in `TAPJIG.md`)

- Split the engine into a package and add the fixture-profile lookup between
  routine signals and the jig's line protocol.
- Jig line protocol grows from a handful of named nodes to generic
  commands over named resources (`ADC <resource>`, `DIO <resource> <mode>
  <value>`, `RAIL <name> <on|off> <limit>`, `BUS ...`), with the jig
  profile saying which exist. A routine that needs something the connected
  jig or fixture lacks is rejected before the run starts.
- Product-specific DUT communication (feeder RS-485 frames, mainboard TBD)
  stays a per-product plugin kept in the repo, not on the SMB share.
- Auto-select: the fixture ID picks the product when only one product matches
  that fixture.

## Decisions and findings from the follow-up discussion

**Parts chosen so far:** MCP23017 (I/O expanders), ADS1115 (ADC),
CA-IS3092VW (isolated RS-485), CAN as an option only, "decent" relays for the
power stages and miniature relays for detection (so the 10 kOhm cold-resistance
sniff can be switched out and never loads the rail afterwards), 2 or 3 card-edge
connectors of 60 pins each.

### Is the 32u4 enough? No

26 usable pins, one UART, one I2C, one SPI, 2.5 KB RAM and 8-bit 16 MHz. It
cannot host two RS-485 ports, CAN, several I2C buses and a block framework, and
it has no spare pins for the budget above. Move to a bigger MCU. Two sensible
choices:

| | RP2350 (e.g. RP2350B, 48 GPIO) | STM32G474 (or a similar G4/H5) |
|---|---|---|
| Strength | **PIO state machines can make an encoder, capture, UART, SPI or custom serial on almost any pin**: matches "pins P122/P123 are A and B, set up an encoder" best | real hardware encoder interface, capture and PWM on timers; built-in CAN-FD; many 5 V-tolerant pins |
| Weakness | CAN needs an external controller (e.g. MCP2518FD over SPI); 3.3 V pins; 48 GPIO | each peripheral only works on its alternate-function pins, so "any block on any pin" is limited by the pin mux |
| Pick if | arbitrary pin-to-function mapping matters most | you want the most robust hardware blocks and CAN on-chip |

Either way, every connector line needs level translation to the fixture's
logic reference (Vio), so MCU pins do not touch the pogo side directly. A small
FPGA would give fully arbitrary routing, but it is overkill for now.

### Expanders and ADC: what they can and cannot do

- **MCP23017 pins are slow-class**: an I2C transaction per change (about
  100 us at 1 MHz), no timers, no peripherals. Use them for switches, reset
  forcing, level reads, relay drivers and enables. **Encoders, PWM, capture
  and UART/SPI blocks must sit on MCU pins.** The pin table therefore carries a
  capability class (`fast` / `slow`) and the config tool refuses an
  impossible assignment.
- Outputs are push-pull only; emulate "open-drain low" by switching the pin
  between input and driven-low. Run each expander at the bank's Vio. Eight
  addresses per bus (128 pins), so two or three buses leave plenty of room.
- **ADS1115**: 16 bit but slow (860 SPS per device, one conversion at a time),
  four addresses per bus. Fine for DC rails and LDR levels, not for waveforms.
  Its inputs must never exceed its supply, so scaling and clamping belong on
  the fixture (or a protected input stage on the base).
- **Relays**: allow 5-10 ms settle after any relay change, drive coils through
  low-side drivers with flyback protection, and add a "rail off but a downstream
  node still reads voltage" check to catch a welded power relay.
- **CA-IS3092VW** gives isolated RS-485; its integrated isolated supply is
  limited, so budget it per port. The same vendor family has isolated CAN
  transceivers for the optional CAN port.

### Blocks (the "P122 and P123 are an encoder" idea)

A **block** is a typed peripheral instance bound to named pins, created at run
time from the fixture profile. The host never writes to a pin directly for
these; it creates a block and talks to it.

```json
{ "blocks": [
  { "id": "enc_m1",  "type": "encoder",  "pins": {"A": "P122", "B": "P123"}, "counts_per_rev": 400 },
  { "id": "servo0",  "type": "servo",    "pins": {"OUT": "P130"} },
  { "id": "sw1",     "type": "gpio_od_low", "pins": {"OUT": "P207"} },
  { "id": "bus0",    "type": "rs485",    "pins": {"TX": "P010", "RX": "P011", "DE": "P012"}, "baud": 9600 },
  { "id": "fb",      "type": "adc",      "pins": {"IN": "A00"}, "range": "2.5V", "scale": 1.0 }
] }
```

- Block types (initial set to implement, extensible): `gpio_in`, `gpio_out`,
  `gpio_od_low`, `pwm`, `servo`, `encoder`, `freq_counter`/`capture`, `uart`,
  `rs485`, `can`, `spi_dev`, `i2c_dev`, `adc`, `dac`, `relay`, `rail`,
  `pixel` (addressable LED), and later whatever a product needs.
- Firmware keeps a **block registry** (one driver per type). The jig profile
  lists which types it supports and which pins are `fast` or `slow`. The host
  validates the whole fixture profile against that before a run, and the
  protocol is generic: `BLOCK NEW <id> <type> <pin=...> <param=...>`,
  `BLOCK SET`, `BLOCK GET`, `BLOCK FREE`.
- Pin names are global (`P000..` for MCU pins, `P2xx` for expander pins, `A..`
  for analog, rails by name), so a fixture profile is the only place that
  knows how a product is wired.
- The routine still talks about **signals** and a block id, never raw pins.

### Connectors: 2 or 3 x 60-pin card edge

Count the budget first: 64 digital + 16 analog + 4 AO + buses + programming
lines + a ground per few signals is already about 150-180 contacts, so plan for
**3 connectors**:

1. **Power and sense**: rails, Kelvin sense, many grounds. Card-edge contacts
   carry only a few amps each (check the connector datasheet and derate), so a
   multi-amp 24 V rail needs several contacts in parallel per leg, or a
   separate power connector. Keep it physically apart from the analog.
2. **Digital**: expander and MCU digital lines, interleaved with grounds.
3. **Analog, buses and programming**: ADC inputs, DAC, RS-485/CAN/UART, I2C,
   SPI, SWD, ISP, fixture ID EEPROM, interlock.

If three keyed edges plus alignment are too awkward mechanically, that is the
point to prefer the backplane-with-cards option.

## Open questions

- RP2350 (arbitrary pin blocks, external CAN) or STM32G4 (hardware blocks, CAN
  on-chip)?
- One board, or the backplane with cards?
- Rail current/voltage per rail, and is several amps at 24 V really needed in
  the base, or can heavy loads use an external supply with only control lines
  here?
- How many Vio banks, and the level-translation scheme (direction-controlled
  versus per-bank bidirectional) given open-drain and quadrature signals.
- Which block types are needed first (the feeder jig needs: `gpio_od_low`,
  `gpio_in`, `adc`, `servo`, `rs485`, `rail`, `relay`, `capture`).
- Which probes beyond AVR ISP and SWD (JTAG? UART bootloaders?).
- Fixture ID format, and where fixture profiles live (on the fixture EEPROM, on
  the share keyed by fixture ID, or both).
