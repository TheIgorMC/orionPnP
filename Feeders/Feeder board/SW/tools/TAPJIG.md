# TAP-Jig (test-and-program jig): host software

Host side of the jig described in the spec doc "OrionPnP Feeder Programming &
Test Jig". The **Production** tab of `rs485_gui.py` runs the whole 18-stage
routine (pre-power checks through release) against a feeder seated in the
cradle, logs pass/fail per unit, and programs the unit. The same engine runs
headless with `tapjig_run.py`.

Status: written and exercised against `sim_jig.py` only. **The 32u4 jig
firmware does not exist yet** (the protocol below is what it has to
implement), the test-firmware commands have only been compile-checked, and
every numeric limit in the routine is a placeholder. Nothing here has touched
real hardware.

## Pieces

| File | What |
|---|---|
| `tapjig_engine.py` | `JigLink` (line protocol to the 32u4), `Dut` (feeder via the jig's RS-485 passthrough), `Isp` (avrdude/USBasp), `Runner` (executes a routine), `write_log` |
| `tapjig_routine_feeder_v03.json` | the routine: stages 0-17 from the spec, as data (steps, limits) |
| `tapjig_run.py` | CLI runner (exit 0 = PASS) |
| `rs485_gui.py`, Production tab | the operator UI around the same engine |
| `sim_jig.py` | simulated jig + simulated feeder behind it, for development (`--fail NAME` injects a fault) |
| `../v0.02b` env `atmega328pb_isp_test` | the DUT **test firmware** (`-DTAPJIG_TEST`) |

## Flow

```
host PC ──USB-CDC (line protocol)──> jig ATmega32u4 ──RS-485 (UART1)──> DUT
   │                                      │ ADS1115, LDRs, servo, AO3400 switches, encoders, PSU
   └──avrdude + USBasp ───────────────────┴── ISP pogo pins ───────────> DUT
```

The host talks only to the 32u4. It never needs a second USB-RS485 adapter:
DUT frames are tunnelled through the jig (`RS485` command), built and parsed
with the same `rs485_protocol.py` the rest of the tools use. ISP is run by the
host with avrdude; the jig is told afterwards (`ISPDONE`) so it can release or
power-cycle the DUT.

1. The operator seats the board, picks the **tape width** (a unit option, not
   a measurement), presses **Run all stages**.
2. The engine runs the stages in order, stops at the first failed stage
   (option), and always ends with `SAFE` (PSU off, switches released, servo
   parked), whatever happened.
3. A full run writes `production_logs/<date>/<SN>_<time>_<PASS|FAIL>.json`
   (every check with the values it saw, every saved measurement) and appends a
   line to `production_logs/summary.csv`. A "Run selected stage" run is a
   debugging aid and is not logged.

The serial number comes from the AT24CS02 factory page (stage 10). The unit is
identified by it in the log; a run that fails before stage 10 logs as
`NOSERIAL`.

## Jig line protocol (what the 32u4 must implement)

ASCII, one request line, one reply line, `\n` terminated, 115200 baud.
Reply is `OK key=value ...` or `ERR reason`. Numbers are decimal. Everything
blocks until done unless noted. The host gives up after 3 s (RS485: its own
timeout + 2 s).

| Request | Reply | Meaning |
|---|---|---|
| `HELLO` | `OK fw=<v> rev=<n>` | identify |
| `SAFE` | `OK` | PSU off, SW1/SW2/RESET released, servo parked, stalls released. Must always work |
| `PSU ON <limit_mA>` / `PSU OFF` | `OK` / `ERR TRIP` | jig supply to the DUT with the tight current limit (software fault detection per the spec: deglitch, hysteresis, latch) |
| `STATE` | `OK psu=<0/1> latched=<0/1> ma=<n>` | PSU on, current-limit trip latched, input current (INA293) |
| `REARM` | `OK` | clear a latched trip (explicit, never automatic) |
| `COLD VIN\|V5\|VMOT` | `OK ohms=<n>` | pre-power resistance via the 10 kOhm sniff; 0 = short, large = open; settle delay inside |
| `CONT VIN_PRE_POST\|GNDP\|MODE_RTN\|R485_POST` | `OK ohms=<n>` | continuity/resistance between the named points |
| `ADC FB\|VIN\|V5\|VINPROT\|IMON` | `OK mv=<n>` | ADS1115 reading **at the DUT node** (divider already undone) |
| `ADC IIN` | `OK ma=<n>` | jig's own input-current reading |
| `DIG FAULT\|V5RDY\|FLT\|EN485` | `OK v=<0/1>` | digital reads (FAULT, FLT# are active low: 1 = not asserted) |
| `SW SW1\|SW2 <0/1>` | `OK` | 1 = force low (button pressed) |
| `RESET <ms>` | `OK` | pulse RESET low |
| `LDR FIBER\|RGB <samples>` | `OK raw=<0-1023>` | averaged ADC of the LDR |
| `SERVO <deg>` | `OK` | position the magnet arm (waits for the move) |
| `STALL <1\|2> <0/1>` | `OK` | hold / release reference motor 1 or 2 so the DUT's drive really stalls (**mechanism undefined, see open items**) |
| `ENCARM <ch> <window_ms>` | `OK` | start timing reference encoder `ch` (non-blocking) |
| `ENCREAD <ch>` | `OK pulses=<n> rev_ms=<n>` | result: pulses and measured full-revolution time (0 if it never spun) |
| `RS485 <hex frame> <timeout_ms>` | `OK frames=<hex> de=<0/1> n=<bytes>` | transmit one frame on UART1, collect everything received for `timeout_ms`; `de` = RE/DE pogo was seen asserted while the DUT answered |
| `ISPDONE test\|production` | `OK` | host finished programming; release/power-cycle the DUT so the new firmware runs |

The jig keeps every output in a safe state on boot and on USB disconnect.

## DUT test firmware

`pio run -e atmega328pb_isp_test` builds `v0.02b` with `-DTAPJIG_TEST`. Same
fuses as production (the spec requires no extra fuse pass), but:

- no boot homing and no button actions (buttons are only *reported*);
- the status RGB can be held by the jig;
- extra commands (production firmware ignores them, which stage 16 uses to
  prove the production build is running):

| Command | Code | Payload | Reply |
|---|---|---|---|
| `CMD_T_INPUTS` | `0x40` | - | `0xB0`: `[flags,imonHi,imonLo,v5Hi,v5Lo]`; flags b0 SW1 down, b1 SW2 down, b2 DRV nFAULT low, b3 relay coil on, b4 magnet detected |
| `CMD_T_UPTIME` | `0x41` | - | `0xB1`: `[ms x4 big-endian, MCUSR at boot]` (bit 1 = external reset) |
| `CMD_T_RGB` | `0x42` | `[r,g,b]` (all 0 = release) | `CMD_ACK` |
| `CMD_T_MOTOR` | `0x43` | `[motor 0=A/1=B, dir, ms/10]` | `CMD_ACK [faultSeen]`; open loop, full duty with soft start, stops on a DRV fault |

Everything else the stages need already exists in v0.02b: `CMD_PING`,
`CMD_GET_STATUS`, `CMD_I2C_SCAN`, `CMD_GET_SERIAL`, `CMD_SET_EXT_LED`,
`CMD_SET_HW_INFO`/`CMD_GET_HW_INFO`, `CMD_SET_SERIAL`, discovery/assign.

## Routine format

`tapjig_routine_feeder_v03.json`: `stages[]`, each with `steps[]` and an
optional `cleanup[]` (always runs). A step is one of:

- `{"do": "jig.adc", "node": "FB", "save": "fb"}` an action, result saved as `fb`
- `{"check": "between(fb.mv, 1180, 1260)", "label": "..."}` a check
- `{"wait_ms": 800}`, `{"prompt": "message"}` (operator OK/Cancel)
- any step may carry `"if": "<expression>"`; a check may carry `"advisory": true`
  (recorded, never fails the stage - for limits that are not calibrated yet)
- an action parameter starting with `=` is an expression: `"mm": "=opt.tape_width_mm"`

Expressions are evaluated by a whitelisting evaluator (comparisons, `and/or/not`,
arithmetic, attribute and index access, and `between within abs min max len
round angdiff angdist any all`); nothing else is callable. Names available:
saved results, `opt` (unit options: `tape_width_mm`, `operator`), `p`
(routine `params`), `unit.serial`.

Actions: `jig.hello/safe/rearm/state/cold/cont/psu_on/psu_off/adc/dig/sw/
reset_pulse/ldr/servo/stall/enc_arm/enc_read`, `dut.discover_assign/ping/
status/i2c_scan/serial/inputs/uptime/rgb/ext_led/motor/hw_info/write_width/
write_serial`, `isp.program`, `util.sleep`. Add a limit or a stage by editing
the JSON; add a new kind of action in `Runner._build_actions`.

## Running it

GUI: `python rs485_gui.py`, Production tab: connect the jig port, point at the
routine and the two `.hex` files (`pio run -e atmega328pb_isp_test` and
`pio run -e atmega328pb_isp`; both under `.pio/build/<env>/firmware.hex`) and
avrdude, choose the tape width, **Run all stages**. Click a stage to see its
checks and measured values. **Stop** aborts at the next step and still puts the
jig in its safe state.

CLI: `python tapjig_run.py --port COM7 --test-hex ... --production-hex ... --tape-width 12`.

No hardware: `python sim_jig.py`, then connect to the path it prints with
**Dry-run ISP** ticked (or `--dry-isp`). `--fail cold_short|fb_low|no_as5600|
no_serial|no_fiber|stuck_sw1` makes one thing wrong.

## Mapping to the spec's stages

| # | Stage | Notes |
|---|---|---|
| 0 | Pre-power | cold resistance VIN/5V/VMOT, VIN pre/post-fuse continuity, GNDP, MODE not bridged |
| 1 | Power-up | PSU on at 300 mA, no latched trip, FB, 5V, VIN, VINPROT, 5VRDY, FLT#, idle current |
| 2 | Flash test FW | fuses, erase, write+verify, then the DUT must answer discovery and ping |
| 3 | SW1/SW2 | AO3400 forcing, read back with `CMD_T_INPUTS` |
| 4 | RESET | pulse, DUT re-announces, MCUSR shows an external reset |
| 5 | Fault line | real stall of motor A, DRV fault trips and clears; LDR value recorded as **advisory** |
| 6 | Fiber LED | `CMD_SET_EXT_LED` + LDR on/off delta |
| 7 | RGB LED | `CMD_T_RGB` R/G/B + LDR delta each, distinct |
| 8 | AS5600 flat | I2C scan shows 0x36 and 0x50, status reports no magnet |
| 9 | AS5600 magnet | servo sweep, angle follows in steps |
| 10 | AT24CS02 SN | factory serial read, becomes the unit's identity |
| 11 | Motors + encoders | A -> M1, B -> M2: drive 1 s, encoder revolution time, no DRV fault |
| 12 | RS-485 logic | ping works and RE/DE was seen asserted |
| 13 | RS-485 bus | real frames, relay commanded (EN_485_COIL + `inputs.relay`), post-relay continuity, IMON vs the jig's current |
| 14 | Write EEPROM | tape width + read-back; serial only if the chip has no factory serial |
| 15 | Flash production FW | erase + write + verify, no fuse pass |
| 16 | Final confirm | SN and tape width read back; test-only command no longer answers |
| 17 | Release | PSU off |

## Open items (need a decision or a measurement)

- **Jig firmware**: the 32u4 side of the protocol above is unwritten.
- **Limits**: every number in the routine is a nominal-value guess. Calibrate on
  a known-good unit, in particular LDR deltas (stages 6, 7), encoder revolution
  time (11), IMON agreement (13), idle current (1).
- **Stage 5 stall**: the spec says "real motor stall, not forced". How the jig
  holds a reference motor (`STALL`) is undefined; the routine assumes motor A
  <-> M1. The LDR check is advisory because the "fault LED" has no LDR in the
  spec and the status RGB shows red for several reasons (no magnet, fault).
- **Fuse bytes** in the routine (`E2/D7/FD`) are my reading of what `pio run -t
  fuses` burns for this board: verify with `pio run -e atmega328pb_isp -t
  fuses -v` before the first production run.
- **Stage 14 serial**: the spec says "serial number + tape width" are written,
  but an AT24CS02's serial is factory read-only. The routine writes a serial
  only when none exists (plain AT24C02). If you want jig-assigned serials,
  that needs a decision.
- **TPS26601 SHDN#/pin 14** (spec open item) has no test here: the spec says it
  may not be pogo-toggleable; add a stage once that is settled.
- Which stages the DUT-side commands cover when the board has no magnet
  (stage 5 and 8 assume the servo arm is parked away).

## TODO: portable production station (Raspberry Pi) and multi-product support

Target hardware: Raspberry Pi 3B (1 GB) + 7" 1024x600 IPS capacitive touch
HDMI monitor (USB-powered, USB touch), everything from one 5 V supply
(about 5 V / 4-5 A; the jig needs a 5->12 V boost for the DUT's VIN). Not
started; decisions so far:

- [ ] Hardware direction for a universal base (resource budget, fixture
      connector, fixture/jig profiles, safety): see
      `TAPJIG_UNIVERSAL_BASE.md`.
- [ ] **Separate kiosk app** for production (keep `rs485_gui.py` as the
      engineering tool): fullscreen 1024x600, touch-sized buttons, product
      selector, big PASS/FAIL banner, stage list, run/stop. No packet
      builder, calibration or peel controls. Settings behind a long press.
      On-screen keyboard for the rare text entry (operator name).
- [ ] **Split the engine into a package** (runner, expression checks, ISP,
      logging) shared by both apps and `tapjig_run.py`.
- [ ] **Product folders** (data only): `product.json`, routine, test and
      production hex, fuse bytes, per-unit options (e.g. tape widths),
      `requires_jig`. A `current.json` pointer chooses the release rather than
      "newest file".
- [ ] **Jig profile**: which jig this is and what it supports (commands,
      nodes, ISP part); the jig's `HELLO` reports its type. Reject a routine
      that needs something the jig lacks before the run starts. Selector
      auto-picks the product when only one matches the jig.
- [ ] **Pluggable DUT command layer** per product (feeder: RS-485 frames;
      mainboard: TBD once its test interface is known). Plugins live in the
      repo, never on the share.
- [ ] **SMB share** (`cifs` mount): sync the selected release to local disk
      and verify checksums before a run (a network drop must not interrupt a
      flash); write logs locally first, then copy them to the share; record
      release version and hashes in every unit log.
- [ ] **Pi setup notes**: autostart in kiosk mode, udev rules (jig, USBasp),
      avrdude install, undervoltage check, read-only root or at least a
      writable log partition.
