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

## Open questions

- One big board or a backplane with cards?
- Controller: which MCU, and how much stays on it versus the Pi?
- Rail current/voltage ratings per rail (and is 24 V really needed at several
  amps for the mainboard, or can heavy loads use an external supply with only
  control lines in the base)?
- How many contacts on the fixture connector, and which connector family?
- Which probes beyond AVR ISP and SWD (JTAG? UART bootloaders?).
- Vio banks: how many independent logic-level banks?
- Fixture ID format and where fixture profiles live (on the fixture EEPROM,
  or on the share keyed by fixture ID, or both).
