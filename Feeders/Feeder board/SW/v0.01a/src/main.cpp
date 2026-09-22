#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <math.h>
#include <Adafruit_NeoPixel.h>
#include "pins_config.h"

/*
  v0.01a - forked from beta1 at the point PIN_5V_READY/PIN_485_RELAY were
  corrected (point 6 below). First MAJOR-version release: the stepping
  stone going forward, not another bench-iteration alpha/beta. Only
  change from that beta1 state: the bench-only auto-run test aids
  (runDebugSelfTest(), runRelayButtonTest()) are commented out of
  setup() rather than running unconditionally on every boot - see their
  call sites below. Both functions are still defined and still reachable
  on demand (SELFTEST debug command, RELAY ON/OFF), just not forced on a
  release build's every power-up. The real safety gate -
  waitFor5vStableAndEngageRelay(), which the relay only ever actually
  engages through - is untouched.

  PIN_5V_READY's divider still isn't correct on real hardware as of this
  fork (the PCB needs the 4.7k/1k resistor swap - see beta1/project.md,
  "Open questions") - kept implemented anyway rather than stripped out,
  since the logic itself is right and just needs the matching hardware
  fix.

  Below is beta1's own history at the point of the fork, describing the
  additions this inherits from alpha03:

  1. PIN_I_MON (A6/PE2) - analog, the IMON output of a TPS26600 eFuse on
     the 12V rail (RIMON=309k, 1%), a voltage proportional to load
     current: ~4.07V at the 200mA design max load, linear through the
     origin. Read with the default AVCC reference; see the "Power
     sequencing" section below and readIMonMilliamps(). TPS26600's EN/
     FLT# pins are NOT wired to the MCU - it can't be, since the MCU only
     runs once the eFuse is already on, so there's no fault state where
     firmware could still be reading a pin to report it.
  2. PIN_5V_READY (A2/PC2) - analog, 5V rail via an external 4.7k/1k
     divider (4.7k to the rail, 1k to GND - picked over 10k/1k for ~2x
     the ADC resolution, see pins_config.h). Read against the ATmega's
     INTERNAL 1.1V reference, not the default - see "Power sequencing"
     for why the default reference cannot work for this specific signal.
  1b. Both of the above convert their raw ADC reading to a real-world
     mA/mV through AnalogCalibration, a single (raw, real-world) point per
     signal, persisted in the ATmega's internal EEPROM and hand-settable
     on the bench with the CALI/CALV debug commands against a real
     ammeter/multimeter - useful once a bed-of-nails test jig exists to
     make that fast. Falls back to the factory-calculated defaults
     (derived from the same numbers as points 1/2 above) until then. See
     the struct's comment further down for why this is deliberately not
     folded into FeederConfig or FeederHardwareInfo. estimateI5vMilliamps()
     also derives a rough 5V-rail current estimate from these two readings
     (IMON + 12V-nominal power balance) - debug/bench convenience only,
     not a real measurement; see that function's comment for what it
     ignores.
  3. PIN_485_RELAY (A7/PE3) - a MOSFET-driven relay coil that physically
     connects/disconnects this feeder's RS485 lines from the shared bus.
     Held disconnected until PIN_5V_READY has read stable for
     RELAY_READY_STABLE_MS, so a feeder that's still mid-power-up can't
     electrically disturb a bus other feeders/the host are already using.
  4. Motor A's effective direction defaults to inverted (invertMotorA =
     true) - the final board's DRV8833 OUT1/OUT2 (motor-output side, not
     the MCU-to-driver AIN side) are swapped relative to the bench units
     this control loop was tuned against. Net effect on firmware is the
     same as any other "wired backwards" case.
  5. PIN_AIN1/PIN_AIN2 <-> PIN_BIN1/PIN_BIN2 are swapped in pins_config.h
     relative to alpha01-03 - the first real beta1 bench test found that
     commanding "motor A" (closed-loop feed/sprocket logic) actually
     moved the peel motor instead, i.e. the two DRV8833 channels drive
     the opposite motor connectors from what alpha01-03's pin numbers
     assumed. Different bug from point 4's direction inversion - see
     pins_config.h for the fix and why invertMotorA=true above is
     unverified against it (validated against the old channel assignment,
     not this one).
  6. PIN_5V_READY and PIN_485_RELAY (points 2/3 above) were originally
     speced backwards: PE3/A7 was PIN_5V_READY (an analog input) and
     PIN_485_RELAY was on D13/PB5. Real hardware has it the other way -
     PE3/A7 is the relay drive (digital OUTPUT), and PIN_5V_READY is on
     A2/PC2 instead. D13/PB5 is now unused again. Also broke
     seedSessionNonce()'s entropy source, which assumed A2 was a floating
     ADC pin - it's PIN_5V_READY now, a real signal, so that XOR was
     dropped (see its own comment).

  Points 5 and 6 are the only entries on this list confirmed against real
  beta1 hardware so far; the rest (new pins otherwise, the OUT1/OUT2
  direction swap) are still ahead of any board built so far - same
  situation alpha03 was already in for its own additions, firmware
  written for a schematic revision that doesn't exist as a built board
  yet.

  Also carries forward everything alpha03 added on top of alpha02: AT24CS02
  I2C EEPROM + factory serial number (FeederHardwareInfo now lives there,
  not the ATmega's internal EEPROM), CMD_GET_SERIAL. See alpha03/project.md
  for that reasoning in full.

  Everything else (SW1 closed-loop / SW2 open-loop, RS485 transport,
  addressing/discovery, tape-zero/distance-based motion, FeederConfig) is
  unchanged from alpha01/02 - see those projects' project.md files for the
  full design rationale, and alpha01's for the Modbus feasibility
  evaluation.

  What beta1 deliberately does NOT do yet (inherited from alpha01-03):
  - No interrupt-driven UART RX. Bytes are polled in loop(). Since
    moveToAngle() blocks synchronously for up to MOVE_TIMEOUT_MS, and
    USART0 only has a 2-byte hardware buffer, any bus traffic arriving
    mid-move today WILL be lost. Fine for bench-testing in isolation, but
    must be fixed (RX ISR + ring buffer) before real bus traffic coexists
    with motion - see project.md "Open questions".
  - No Modbus RTU. The frame format below is a placeholder.
*/

// ---------------------------
// AS5600 registers
// ---------------------------
const uint8_t AS5600_ADDR = 0x36;
const uint8_t AS5600_REG_STATUS = 0x0B;
const uint8_t AS5600_REG_ANGLE = 0x0E; // + 0x0F (filtered/hysteresis)
const uint8_t AS5600_REG_AGC = 0x1A;
const uint8_t AS5600_REG_MAGNITUDE = 0x1B; // + 0x1C

// ---------------------------
// Wheel geometry
//
// Sprocket holes on carrier tape are spaced 4mm apart regardless of tape
// width (EIA-481 standard) - one sprocket tooth engages one hole, so one
// tooth of travel is always 4mm of tape, independent of what's on the
// tape. Everything below derives from that one physical constant plus the
// wheel's tooth count.
// ---------------------------
const int TOOTH_COUNT = 40;
const float DEG_PER_TOOTH = 360.0f / TOOTH_COUNT;       // 9.0 deg
const float DEG_PER_HALF_TOOTH = DEG_PER_TOOTH / 2.0f;  // 4.5 deg
const float SPROCKET_HOLE_PITCH_MM = 4.0f;              // EIA-481: fixed, all tape widths
const float DEG_PER_MM = DEG_PER_TOOTH / SPROCKET_HOLE_PITCH_MM; // 2.25 deg/mm

// Fixed mechanical offset from "a sprocket tooth is seated" to "the pick
// window/camera position" - purely geometric (PCB/frame layout), so it's
// the same on every feeder of this exact design, not per-unit or
// per-component. Measure ONCE with a camera on a reference unit (jog with
// MOVEMM to find it), then hardcode the result here - see project.md
// "Tape zero calibration, v2". Left at 0.0 until that measurement exists;
// ZEROHERE/GOTOZERO will be off by this amount until it's filled in.
const float PICK_OFFSET_MM = 0.0f; // TODO: measure once, see comment above

// ---------------------------
// Motion / control tuning (unchanged from TestBench03/04)
// ---------------------------
const float ANGLE_TOLERANCE_DEG = 0.30f;
const float CREEP_THRESHOLD_DEG = 3.0f;
const int   FAST_DUTY = 110;
const int   DEFAULT_MIN_MOVE_DUTY = 60;

const unsigned long MOVE_TIMEOUT_MS = 6000;
const unsigned long STALL_TIMEOUT_MS = 800;
const float STALL_MOVE_THRESHOLD_DEG = 0.15f;

// SW2/motor B open-loop jog (see file header - motor B has no encoder on
// this design, so there's no closed-loop equivalent to fall back to).
const int   JOG_DUTY = FAST_DUTY;
const unsigned long JOG_DURATION_MS = 300;

const bool BUTTONS_ACTIVE_LOW = true;
const unsigned long DEBOUNCE_MS = 20;

// Runtime-flippable motor direction (replaces alpha01's compile-time
// INVERT_DIRECTION constant). These let direction be corrected from the
// debug port or the bus (INVERTA/INVERTB, CMD_SET_INVERT_DIR) without
// reflashing or re-soldering. Independent per motor since there's no
// reason A and B would necessarily need the same correction. RAM-only for
// now (reset to this default on reboot) - see project.md if either turns
// out to be a fixed-per-unit characteristic worth persisting to EEPROM
// instead.
//
// invertMotorA defaults to true for beta1: the bench units this control
// loop was originally tuned/validated against had DRV8833 OUT1/OUT2
// (motor-output side) swapped relative to what beta1 turned out to need.
// Still overridable at runtime (INVERTA/CMD_SET_INVERT_DIR) if this
// turns out to be wrong.
//
// Separate from, and NOT a fix for, the channel-level swap the first
// beta1 bench test actually found: commanding motor A moved the peel
// motor instead of the feed motor. That's which DRV8833 channel drives
// which physical motor connector - fixed in pins_config.h (PIN_AIN1/
// PIN_AIN2 <-> PIN_BIN1/PIN_BIN2), not here. Because that fix changes
// which physical channel "motor A" now drives, invertMotorA=true above
// is UNVERIFIED against it - it was only ever validated against the old,
// swapped channel assignment. Re-test direction on real hardware; don't
// assume it's still correct.
bool invertMotorA = true;
bool invertMotorB = false;

// Both UARTs share the same 8MHz internal RC oscillator (~2% tolerance),
// so both stay at 9600 - see TestBench04/project.md for why 115200 is
// unreliable here. RS485_BAUD also doubles as the bus's initial working
// assumption; revisit once a crystal is on the board or Modbus baud is
// decided.
const unsigned long RS485_BAUD = 9600;
const unsigned long DEBUG_BAUD = 9600;

const uint32_t I2C_CLOCK_HZ = 100000;

// ---------------------------
// Zero-point (still-duty) calibration tuning
// ---------------------------
const int   CAL_START_DUTY = 15;
const int   CAL_MAX_DUTY = 200;
const int   CAL_STEP_DUTY = 5;
const unsigned long CAL_STEP_MS = 60;
const float CAL_MOVE_THRESHOLD_DEG = 0.6f;
const int   CAL_MARGIN_DUTY = 6;
const unsigned long CAL_RESTORE_TIMEOUT_MS = 4000;

// ---------------------------
// Addressing (see project.md "Addressing scheme", v2)
//
// The bus address is disposable and RAM-only: every boot starts
// unaddressed (0x00) and re-earns an address via CMD_DISCOVER, so nothing
// about the address survives a power cycle or needs EEPROM at all. What
// DOES persist is the feeder's loaded-component config (below) - that's
// the thing worth not losing on reinsertion, not the address.
// ---------------------------
constexpr uint8_t ADDR_UNASSIGNED = 0x00; // also the broadcast address
constexpr uint8_t ADDR_MIN = 1;
constexpr uint8_t ADDR_MAX = 247;

uint8_t busAddress = ADDR_UNASSIGNED;
uint16_t sessionNonce = 0; // random per boot, only used to disambiguate discovery replies

bool isValidAssignedAddress(uint8_t a) {
  return a >= ADDR_MIN && a <= ADDR_MAX;
}

// Seeds from boot-to-boot jitter in micros(). Used to XOR in an
// analogRead(A2) too, on the assumption A2 was a floating/unconnected
// ADC pin - it isn't: A2 is PIN_5V_READY, a real connected signal (see
// pins_config.h), and every other analog-capable pin on this board
// (A0-A7) is spoken for as well (buttons, I2C, IMON, relay drive) - none
// left floating to harvest noise from. Not cryptographically unique -
// doesn't need to be. It only has to avoid colliding with whichever
// other feeders happen to be replying to the same CMD_DISCOVER round,
// and a fresh value is drawn every boot anyway.
void seedSessionNonce() {
  randomSeed(micros());
  sessionNonce = (uint16_t)random(0, 65536);
}

// ---------------------------
// Persistent per-feeder config (EEPROM) - what SHOULD survive reinsertion.
//
// componentId: OpenPnP part id (see openPnP/parts.xml) currently loaded.
// tapeZeroRaw: AS5600 raw angle (0-4095) marking the current tape's
//   "ready" pocket position - meaningless across a component change, since
//   a different reel is now engaged with the sprocket at an arbitrary
//   rotational offset.
// feedHalfTeeth: advance per pick, in half-tooth (4.5 deg / 2mm) units.
//   2 = 4mm/1 tooth (standard EIA-481 sprocket pitch, "raw"), 1 = 2mm
//   ("fine", sub-tooth pitch), anything else = "custom" for wider-pitch
//   parts (8/12/16/24mm reels etc). Not three separate stored presets -
//   just one active step size; changing which preset is "active" is a
//   host/UI choice about what value to write here.
//
// All three reset to "unset" together whenever componentId changes (a
// different reel means the old zero/step are meaningless), or on an
// explicit manual reset - never on a bare reinsertion of the same
// component. NOT to be confused with calibrateZero()/stillDutyMax below,
// which is the DRV8833 motor-duty characterization - electrical, unrelated
// to which tape is loaded, and still re-run on every boot regardless.
// ---------------------------
struct FeederConfig {
  uint16_t componentId;
  uint16_t tapeZeroRaw;
  uint8_t feedHalfTeeth;
  uint8_t crc;
};

constexpr int EEPROM_CONFIG_LOCATION = 0;
constexpr uint16_t COMPONENT_ID_UNSET = 0xFFFF;
constexpr uint16_t TAPE_ZERO_UNSET = 0xFFFF;
constexpr uint8_t FEED_HALF_TEETH_UNSET = 0xFF;
constexpr uint8_t FEED_HALF_TEETH_STANDARD = 2; // 4mm / 1 tooth, EIA-481 default

FeederConfig cfg;

uint8_t crc8(const uint8_t *data, uint8_t len); // fwd decl, defined below

uint8_t configCrc(const FeederConfig &c) {
  return crc8(reinterpret_cast<const uint8_t *>(&c), sizeof(FeederConfig) - 1);
}

void resetFeedConfig() {
  cfg.tapeZeroRaw = TAPE_ZERO_UNSET;
  cfg.feedHalfTeeth = FEED_HALF_TEETH_UNSET;
}

void saveConfig() {
  cfg.crc = configCrc(cfg);
  EEPROM.put(EEPROM_CONFIG_LOCATION, cfg); // EEPROM.put only rewrites changed bytes
}

void loadConfig() {
  EEPROM.get(EEPROM_CONFIG_LOCATION, cfg);
  if (cfg.crc != configCrc(cfg)) {
    // Blank/garbage EEPROM (factory-fresh board) - start fully unset.
    cfg.componentId = COMPONENT_ID_UNSET;
    resetFeedConfig();
    saveConfig();
  }
}

// ---------------------------
// Analog calibration - beta1 only. IMON (TPS26600) and 5V_READY are both
// resistor-divider/current-mirror signals whose actual scale depends on
// real component tolerances on THIS physical board, not just the nominal
// datasheet/calculator math baked into the constants below. Each is
// stored as a single (raw ADC, real-world value) point and the firmware
// assumes linear-through-the-origin from there (true for both signals'
// underlying hardware - a resistor divider and a current-mirror IMON
// output). Hand calibration (CALI/CALV debug commands) overwrites the
// point with one measured against a real ammeter/multimeter on the bench
// - e.g. once a bed-of-nails test jig exists to make that fast and
// repeatable. Independent of FeederConfig - a component change or
// RESETCFG must never touch this, same reasoning as FeederHardwareInfo.
// Lives in the ATmega's own internal EEPROM (not the AT24CS02) - it's
// bench/electrical calibration data for this board's analog frontend,
// not part of the AT24CS02's tape-width/serial-number "what this unit
// physically is for tape handling" role, and calibrating it doesn't
// require an AT24CS02 to even be populated.
// ---------------------------
struct AnalogCalibration {
  uint16_t imonCalRaw; // PIN_I_MON raw ADC reading at imonCalMa (default reference)
  uint16_t imonCalMa;  // actual measured current (mA) at imonCalRaw, from a bench ammeter
  uint16_t v5vCalRaw;  // PIN_5V_READY raw ADC reading at v5vCalMv (internal 1.1V reference)
  uint16_t v5vCalMv;   // actual measured 5V-rail voltage (mV) at v5vCalRaw, from a bench multimeter
  uint8_t crc;
};

constexpr int EEPROM_ANALOG_CAL_LOCATION = 16; // gap left after EEPROM_CONFIG_LOCATION, same pattern alpha02 used for hw info

// Factory defaults, derived from the same TI-calculator/divider math as
// the header comment and pins_config.h: TPS26600 IMON reads ~4070mV at
// the 200mA design max load (RIMON=309k), and the 4.7k/1k 5V divider
// reads ~816/1023 (against the internal 1.1V ref) at a healthy 5000mV
// rail. Used only until CALI/CALV actually calibrate against real
// hardware - see those commands' comments for the raw-count math.
constexpr uint16_t IMON_CAL_DEFAULT_RAW = 833; // (4070mV * 1023) / 5000mV, default AVCC reference
constexpr uint16_t IMON_CAL_DEFAULT_MA = 200;
constexpr uint16_t V5V_CAL_DEFAULT_RAW = 816; // (5000mV / 5.7 divider * 1023) / 1100mV, internal 1.1V reference
constexpr uint16_t V5V_CAL_DEFAULT_MV = 5000;

AnalogCalibration analogCal;

uint8_t analogCalCrc(const AnalogCalibration &a) {
  return crc8(reinterpret_cast<const uint8_t *>(&a), sizeof(AnalogCalibration) - 1);
}

void resetAnalogCalToFactoryDefaults() {
  analogCal.imonCalRaw = IMON_CAL_DEFAULT_RAW;
  analogCal.imonCalMa = IMON_CAL_DEFAULT_MA;
  analogCal.v5vCalRaw = V5V_CAL_DEFAULT_RAW;
  analogCal.v5vCalMv = V5V_CAL_DEFAULT_MV;
}

void saveAnalogCal() {
  analogCal.crc = analogCalCrc(analogCal);
  EEPROM.put(EEPROM_ANALOG_CAL_LOCATION, analogCal);
}

void loadAnalogCal() {
  EEPROM.get(EEPROM_ANALOG_CAL_LOCATION, analogCal);
  if (analogCal.crc != analogCalCrc(analogCal) || analogCal.imonCalRaw == 0 || analogCal.v5vCalRaw == 0) {
    // Blank/garbage EEPROM, or a stored point that would divide by zero -
    // both start from the factory-default calibration point.
    resetAnalogCalToFactoryDefaults();
    saveAnalogCal();
  }
}

// ---------------------------
// AT24CS02 - I2C EEPROM (256 bytes) + factory-programmed, read-only 128-bit
// unique serial number, on the same I2C bus as the AS5600 (different
// address, no new pins - see pins_config.h). NOT present on any V0.2a
// board built so far; alpha03 is the first firmware to expect it, ahead of
// the beta1 schematic actually adding it, so every access here is written
// to degrade gracefully (checked endTransmission/requestFrom, same pattern
// as the AS5600 code) rather than hang if the chip isn't populated.
//
// Two separate I2C device-select addresses per the AT24CS02 datasheet:
// the ordinary EEPROM (read/write, general storage) and a distinct
// "identification page" address that only ever reads back the factory
// serial - it's a different address on the bus, not a memory offset
// within the EEPROM, and there's no way to write it.
// ---------------------------
constexpr uint8_t AT24CS02_EEPROM_ADDR = 0x50;  // general-purpose EEPROM, read/write
constexpr uint8_t AT24CS02_SERIAL_ADDR = 0x58;  // identification page, read-only, factory-programmed
constexpr unsigned long AT24CS02_WRITE_CYCLE_MS = 5; // internal write cycle time; no ack-polling implemented, just a fixed delay

bool at24csReadBytes(uint8_t i2cAddr, uint8_t memAddr, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(i2cAddr);
  Wire.write(memAddr);
  if (Wire.endTransmission(false) != 0) return false; // repeated start, keep the bus
  if (Wire.requestFrom((uint8_t)i2cAddr, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool at24csWriteBytes(uint8_t i2cAddr, uint8_t memAddr, const uint8_t *data, uint8_t len) {
  Wire.beginTransmission(i2cAddr);
  Wire.write(memAddr);
  Wire.write(data, len);
  if (Wire.endTransmission() != 0) return false;
  delay(AT24CS02_WRITE_CYCLE_MS);
  return true;
}

constexpr uint8_t FACTORY_SERIAL_LEN = 16; // 128 bits
uint8_t factorySerial[FACTORY_SERIAL_LEN];
bool factorySerialValid = false;

void loadFactorySerial() {
  factorySerialValid = at24csReadBytes(AT24CS02_SERIAL_ADDR, 0x00, factorySerial, FACTORY_SERIAL_LEN);
  if (!factorySerialValid) {
    for (uint8_t i = 0; i < FACTORY_SERIAL_LEN; i++) factorySerial[i] = 0;
    Serial1.println(F("WARN: AT24CS02 factory serial not readable (chip absent on this board?)"));
  }
}

// ---------------------------
// Hardware identity - fixed at manufacturing/assembly time, NOT
// per-component data. Deliberately a separate struct/storage from
// FeederConfig above: tape width is a property of this physical feeder's
// mechanical build (its tape guide/rail width), identical for every reel
// ever loaded into it, so it must never be touched by setComponentId()'s
// reset-on-change or by RESETCFG/CMD_RESET_CONFIG - those are scoped to
// "what's currently loaded," not "what this unit physically is."
//
// Lives on the AT24CS02's EEPROM now (alpha01/02 kept it in the ATmega's
// internal EEPROM) - same reasoning as moving it out of FeederConfig in
// the first place, one level further: "what this unit physically is"
// should survive even a full chip-erase/reflash of the ATmega, not just
// survive a component change.
//
// Tape width doesn't feed into any of this firmware's own math - sprocket
// hole pitch is a fixed 4mm regardless of tape width (EIA-481). Its value
// is purely informational: for a host/OpenPnP to validate that a reel
// someone's about to load is actually compatible with this feeder before
// trying, and for inventory/fleet management (which feeders can take
// which reels). Included in the CMD_DISCOVER_HERE reply so a host learns
// it immediately at discovery time, no separate query needed for the
// common case.
// ---------------------------
struct FeederHardwareInfo {
  uint8_t tapeWidthMm; // EIA-481 standard widths: 8,12,16,24,32,44,56
  uint8_t crc;
};

constexpr uint8_t AT24CS02_HWINFO_MEM_ADDR = 0x00; // byte offset within the AT24CS02 EEPROM
constexpr uint8_t TAPE_WIDTH_UNSET = 0xFF;

FeederHardwareInfo hwInfo;

uint8_t hwInfoCrc(const FeederHardwareInfo &h) {
  return crc8(reinterpret_cast<const uint8_t *>(&h), sizeof(FeederHardwareInfo) - 1);
}

bool isValidTapeWidthMm(uint8_t mm) {
  switch (mm) {
    case 8: case 12: case 16: case 24: case 32: case 44: case 56: return true;
    default: return false;
  }
}

void loadHwInfo() {
  const bool readOk = at24csReadBytes(AT24CS02_EEPROM_ADDR, AT24CS02_HWINFO_MEM_ADDR,
                                       reinterpret_cast<uint8_t *>(&hwInfo), sizeof(hwInfo));
  if (!readOk) {
    Serial1.println(F("WARN: AT24CS02 not responding, hardware info unavailable (chip absent on this board?)"));
  }
  if (!readOk || hwInfo.crc != hwInfoCrc(hwInfo)) {
    // Either the chip didn't answer, or it answered with blank/garbage
    // EEPROM (factory-fresh chip, never set at assembly) - both cases
    // start "unset" rather than trusting a nonsense value.
    hwInfo.tapeWidthMm = TAPE_WIDTH_UNSET;
    hwInfo.crc = hwInfoCrc(hwInfo);
    if (readOk) at24csWriteBytes(AT24CS02_EEPROM_ADDR, AT24CS02_HWINFO_MEM_ADDR,
                                  reinterpret_cast<uint8_t *>(&hwInfo), sizeof(hwInfo));
  }
}

// No "reset" function by design - this is meant to be set once at
// assembly/bench-test time and then left alone for the unit's whole life.
// Validated against known EIA-481 widths so a typo doesn't silently store
// a nonsense value that later confuses a host's compatibility check.
bool setTapeWidthMm(uint8_t mm) {
  if (!isValidTapeWidthMm(mm)) return false;
  hwInfo.tapeWidthMm = mm;
  hwInfo.crc = hwInfoCrc(hwInfo);
  return at24csWriteBytes(AT24CS02_EEPROM_ADDR, AT24CS02_HWINFO_MEM_ADDR,
                           reinterpret_cast<uint8_t *>(&hwInfo), sizeof(hwInfo));
}

// Host is expected to call this whenever it learns/decides what's loaded.
// Resetting the feed config here (rather than leaving stale values from
// whatever was loaded before) is deliberate: a stale tapeZero/feedHalfTeeth
// silently applied to the wrong component is worse than forcing a visible
// recalibration prompt.
void setComponentId(uint16_t newId) {
  if (newId != cfg.componentId) {
    cfg.componentId = newId;
    resetFeedConfig();
  }
  saveConfig();
}

void setFeedConfig(uint16_t tapeZeroRaw, uint8_t feedHalfTeeth) {
  cfg.tapeZeroRaw = tapeZeroRaw;
  cfg.feedHalfTeeth = feedHalfTeeth;
  saveConfig();
}

// Explicit manual reset (debug command / dedicated bus command) - clears
// feed config without requiring a component-id round-trip, e.g. to redo a
// bad calibration on the same reel.
void manualResetFeedConfig() {
  resetFeedConfig();
  saveConfig();
}

// Fwd decls - defined with the rest of tape-zero/distance motion, further
// down near commandMoveTo(); declared here so handleFrame() (RS485
// transport, below) can call them without reordering the whole file.
void setTapeZeroHere();
void setFeedPitchMm(float mm);
uint8_t feedOnePitch(unsigned long timeoutMs);
void setExtLed(bool on);
void identifyBlink(uint8_t count);
float readAngleDeg();
uint8_t readStatus();
uint16_t angleDegToRaw12(float deg);
void brakeMotorA();
void brakeMotorB();
uint16_t readIMonRaw(); // beta1 only - CMD_GET_STATUS's iMonRaw field, defined with the rest of power sequencing
extern bool relayEngaged; // beta1 only - CMD_GET_STATUS's relayEngaged field, defined with setRelay()

// ---------------------------
// Move/feed error codes - returned by moveToAngle()/commandMoveTo() and
// everything built on them (moveByMm, moveToTapeZeroPlusMm, feedOnePitch),
// and echoed in a bus CMD_NACK payload so a host gets a specific reason
// instead of a bare failure. ERR_NONE (0) is the only success value -
// these are NOT booleans, don't treat a nonzero return as "truthy success".
// lastMoveError mirrors whatever the most recent move returned, so
// CMD_GET_STATUS can report it even for a move that wasn't itself
// triggered by the command currently being handled (e.g. a button jog).
// ---------------------------
constexpr uint8_t ERR_NONE = 0x00;
constexpr uint8_t ERR_FAULT = 0x01;        // DRV8833 nFAULT asserted
constexpr uint8_t ERR_MAGNET_LOST = 0x02;  // AS5600 stopped reporting a detected magnet
constexpr uint8_t ERR_STALL = 0x03;        // no encoder motion for STALL_TIMEOUT_MS
constexpr uint8_t ERR_TIMEOUT = 0x04;      // move exceeded its timeout without reaching target
constexpr uint8_t ERR_BAD_PARAM = 0x05;    // malformed/out-of-range command payload
constexpr uint8_t ERR_NOT_READY = 0x06;    // e.g. FEED_NEXT requested before pitch/zero calibrated

uint8_t lastMoveError = ERR_NONE;

// ---------------------------
// RS485 transport (USART0 + RE/DE on PIN_RS485_RE)
// ---------------------------
void rs485Init() {
  pinMode(PIN_RS485_RE, OUTPUT);
  digitalWrite(PIN_RS485_RE, LOW); // start in receive mode
  Serial.begin(RS485_BAUD);
}

void rs485Write(const uint8_t *buf, uint8_t len) {
  digitalWrite(PIN_RS485_RE, HIGH); // transmit enable (DE=1, RE#=1 on the shared pin)
  Serial.write(buf, len);
  Serial.flush(); // block until the last bit has actually left the UART
  digitalWrite(PIN_RS485_RE, LOW); // back to listening
}

uint8_t crc8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0x00;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
    }
  }
  return crc;
}

// Placeholder frame, NOT Modbus:
//   [0xAA][ADDR][CMD][LEN][PAYLOAD...][CRC8 over ADDR..PAYLOAD]
// ADDR: 0x00 = broadcast (also "still unassigned"), 1-247 = unicast.
constexpr uint8_t FRAME_START = 0xAA;

constexpr uint8_t CMD_PING = 0x01;         // -> CMD_PONG
constexpr uint8_t CMD_PONG = 0x81;

// Discovery (see project.md "Addressing scheme, v2"): only a feeder with
// busAddress==ADDR_UNASSIGNED reacts to CMD_DISCOVER. It waits a random
// jitter delay (so simultaneously-unassigned feeders don't all reply at
// once) then announces itself with its session nonce plus whatever
// component it already remembers being loaded with (host can skip
// re-asking "what do you carry" if this is a known/persisted value).
constexpr uint8_t CMD_DISCOVER = 0x10;      // broadcast, no payload
constexpr uint8_t CMD_DISCOVER_HERE = 0x90; // payload: [nonceHi,nonceLo,componentIdHi,componentIdLo,tapeWidthMm]
constexpr uint8_t CMD_ASSIGN_ADDR = 0x11;   // broadcast, payload: [nonceHi,nonceLo,newAddr]
                                             // only the matching nonce adopts newAddr

constexpr uint8_t CMD_GET_COMPONENT = 0x20; // -> CMD_COMPONENT_INFO
constexpr uint8_t CMD_COMPONENT_INFO = 0xA0; // payload: [idHi,idLo,zeroHi,zeroLo,feedHalfTeeth]
constexpr uint8_t CMD_SET_COMPONENT = 0x21;  // payload: [idHi,idLo] -> CMD_ACK
constexpr uint8_t CMD_SET_FEED_CONFIG = 0x22; // payload: [zeroHi,zeroLo,feedHalfTeeth] -> CMD_ACK
constexpr uint8_t CMD_RESET_CONFIG = 0x23;    // no payload -> CMD_ACK
constexpr uint8_t CMD_ZERO_HERE = 0x24;       // no payload: capture current position as tape zero -> CMD_ACK
constexpr uint8_t CMD_SET_PITCH_MM = 0x25;    // payload: [mm] (1 byte, whole mm) -> CMD_ACK
constexpr uint8_t CMD_FEED_NEXT = 0x26;       // no payload: advance by configured pitch -> CMD_ACK/[errCode] on CMD_NACK
constexpr uint8_t CMD_SET_EXT_LED = 0x27;     // payload: [state] (0=off, nonzero=on) -> CMD_ACK/CMD_NACK - standard LED, A3/PC3
constexpr uint8_t CMD_SET_INVERT_DIR = 0x28;  // payload: [motor(0=A,1=B), state(0/1)] -> CMD_ACK/CMD_NACK

// Hardware identity (tape width) - see FeederHardwareInfo above.
constexpr uint8_t CMD_GET_HW_INFO = 0x29;   // -> CMD_HW_INFO
constexpr uint8_t CMD_HW_INFO = 0xA1;       // payload: [tapeWidthMm] (0xFF = unset)
constexpr uint8_t CMD_SET_HW_INFO = 0x2A;   // payload: [tapeWidthMm] -> CMD_ACK/CMD_NACK - assembly/bench-time only, no reset command by design

// Live telemetry/control for real operation (not just bench transport
// validation) - added once alpha02 started heading for an actual PnP
// instead of just the bench.
constexpr uint8_t CMD_GET_STATUS = 0x30;    // -> CMD_STATUS_INFO
constexpr uint8_t CMD_STATUS_INFO = 0xA2;   // payload: [angleRawHi,angleRawLo,as5600Status,faultActive,lastMoveErr,iMonRawHi,iMonRawLo,relayEngaged] - last 3 bytes new in beta1
constexpr uint8_t CMD_STOP = 0x31;          // no payload: immediate brake, both motors -> CMD_ACK
constexpr uint8_t CMD_IDENTIFY = 0x32;      // payload: [blinkCount] (0 => default) -> CMD_ACK after blinking
constexpr uint8_t CMD_GET_SERIAL = 0x33;    // -> CMD_SERIAL_INFO / CMD_NACK if the AT24CS02 isn't readable
constexpr uint8_t CMD_SERIAL_INFO = 0xA3;   // payload: 16 bytes, factory-programmed AT24CS02 serial number

constexpr uint8_t CMD_ACK = 0x82;
constexpr uint8_t CMD_NACK = 0x83;

uint8_t rxFrame[16];
uint8_t rxLen = 0;
enum RxState { WAIT_START, WAIT_ADDR, WAIT_CMD, WAIT_LEN, WAIT_PAYLOAD, WAIT_CRC };
RxState rxState = WAIT_START;
uint8_t rxAddr = 0, rxCmd = 0, rxPayloadLen = 0, rxPayloadIdx = 0;

void sendFrame(uint8_t cmd, const uint8_t *payload, uint8_t payloadLen) {
  uint8_t buf[8 + 16];
  uint8_t n = 0;
  buf[n++] = FRAME_START;
  buf[n++] = busAddress; // frames from a feeder are tagged with its own address (0 while unassigned)
  buf[n++] = cmd;
  buf[n++] = payloadLen;
  for (uint8_t i = 0; i < payloadLen; i++) buf[n++] = payload[i];
  buf[n] = crc8(&buf[1], n - 1); // CRC over ADDR..PAYLOAD (not the start byte)
  n++;
  rs485Write(buf, n);
}

// Random delay before replying to CMD_DISCOVER, so multiple feeders that
// are all still unassigned don't reply in lockstep. Blocking is fine here:
// nothing else needs the CPU while a still-unaddressed feeder waits its
// turn to announce itself.
constexpr unsigned long DISCOVERY_JITTER_MAX_MS = 200;

void handleFrame(uint8_t addr, uint8_t cmd, const uint8_t *payload, uint8_t len) {
  // CMD_DISCOVER/CMD_ASSIGN_ADDR are broadcast-only and only matter to a
  // still-unassigned feeder - handled before the normal address filter
  // below, since an unassigned feeder has no unicast address to match.
  if (addr == ADDR_UNASSIGNED && busAddress == ADDR_UNASSIGNED) {
    if (cmd == CMD_DISCOVER) {
      delay(random(0, DISCOVERY_JITTER_MAX_MS));
      uint8_t reply[5] = {
        (uint8_t)(sessionNonce >> 8), (uint8_t)(sessionNonce & 0xFF),
        (uint8_t)(cfg.componentId >> 8), (uint8_t)(cfg.componentId & 0xFF),
        hwInfo.tapeWidthMm
      };
      sendFrame(CMD_DISCOVER_HERE, reply, sizeof(reply));
      return;
    }
    if (cmd == CMD_ASSIGN_ADDR) {
      if (len < 3) return;
      const uint16_t targetNonce = ((uint16_t)payload[0] << 8) | payload[1];
      if (targetNonce != sessionNonce) return; // not us
      if (!isValidAssignedAddress(payload[2])) return;
      busAddress = payload[2];
      sendFrame(CMD_ACK, &payload[2], 1); // now sent under the new unicast address
      return;
    }
  }

  // Everything else requires a real unicast address - broadcast or our own.
  const bool forUs = (addr == 0x00) || (addr == busAddress);
  if (!forUs || busAddress == ADDR_UNASSIGNED) return;

  switch (cmd) {
    case CMD_PING: {
      sendFrame(CMD_PONG, nullptr, 0);
      break;
    }
    case CMD_GET_COMPONENT: {
      uint8_t reply[5] = {
        (uint8_t)(cfg.componentId >> 8), (uint8_t)(cfg.componentId & 0xFF),
        (uint8_t)(cfg.tapeZeroRaw >> 8), (uint8_t)(cfg.tapeZeroRaw & 0xFF),
        cfg.feedHalfTeeth
      };
      sendFrame(CMD_COMPONENT_INFO, reply, sizeof(reply));
      break;
    }
    case CMD_SET_COMPONENT: {
      if (len < 2) { sendFrame(CMD_NACK, nullptr, 0); break; }
      const uint16_t newId = ((uint16_t)payload[0] << 8) | payload[1];
      setComponentId(newId);
      sendFrame(CMD_ACK, payload, 2);
      break;
    }
    case CMD_SET_FEED_CONFIG: {
      if (len < 3) { sendFrame(CMD_NACK, nullptr, 0); break; }
      const uint16_t zero = ((uint16_t)payload[0] << 8) | payload[1];
      setFeedConfig(zero, payload[2]);
      sendFrame(CMD_ACK, payload, 3);
      break;
    }
    case CMD_RESET_CONFIG: {
      manualResetFeedConfig();
      sendFrame(CMD_ACK, nullptr, 0);
      break;
    }
    case CMD_ZERO_HERE: {
      setTapeZeroHere();
      sendFrame(CMD_ACK, nullptr, 0);
      break;
    }
    case CMD_SET_PITCH_MM: {
      if (len < 1) { sendFrame(CMD_NACK, nullptr, 0); break; }
      setFeedPitchMm((float)payload[0]);
      sendFrame(CMD_ACK, payload, 1);
      break;
    }
    case CMD_FEED_NEXT: {
      const uint8_t err = feedOnePitch(MOVE_TIMEOUT_MS);
      if (err == ERR_NONE) sendFrame(CMD_ACK, nullptr, 0);
      else sendFrame(CMD_NACK, &err, 1); // payload: [errCode] - see ERR_* constants
      break;
    }
    case CMD_SET_EXT_LED: {
      if (len < 1) { sendFrame(CMD_NACK, nullptr, 0); break; }
      setExtLed(payload[0] != 0);
      sendFrame(CMD_ACK, payload, 1);
      break;
    }
    case CMD_SET_INVERT_DIR: {
      if (len < 2 || payload[0] > 1) { sendFrame(CMD_NACK, nullptr, 0); break; }
      if (payload[0] == 0) invertMotorA = (payload[1] != 0);
      else invertMotorB = (payload[1] != 0);
      sendFrame(CMD_ACK, payload, 2);
      break;
    }
    case CMD_GET_HW_INFO: {
      sendFrame(CMD_HW_INFO, &hwInfo.tapeWidthMm, 1);
      break;
    }
    case CMD_SET_HW_INFO: {
      if (len < 1 || !setTapeWidthMm(payload[0])) { sendFrame(CMD_NACK, nullptr, 0); break; }
      sendFrame(CMD_ACK, payload, 1);
      break;
    }
    case CMD_GET_STATUS: {
      const uint16_t angleRaw = angleDegToRaw12(readAngleDeg());
      const uint16_t iMonRaw = readIMonRaw();
      const uint8_t reply[8] = {
        (uint8_t)(angleRaw >> 8), (uint8_t)(angleRaw & 0xFF),
        readStatus(),
        (uint8_t)(digitalRead(PIN_nFAULT) == LOW ? 1 : 0),
        lastMoveError,
        (uint8_t)(iMonRaw >> 8), (uint8_t)(iMonRaw & 0xFF),
        (uint8_t)(relayEngaged ? 1 : 0)
      };
      sendFrame(CMD_STATUS_INFO, reply, sizeof(reply));
      break;
    }
    case CMD_STOP: {
      brakeMotorA();
      brakeMotorB();
      sendFrame(CMD_ACK, nullptr, 0);
      break;
    }
    case CMD_IDENTIFY: {
      identifyBlink(len >= 1 && payload[0] != 0 ? payload[0] : 3);
      sendFrame(CMD_ACK, nullptr, 0);
      break;
    }
    case CMD_GET_SERIAL: {
      if (!factorySerialValid) { sendFrame(CMD_NACK, nullptr, 0); break; }
      sendFrame(CMD_SERIAL_INFO, factorySerial, FACTORY_SERIAL_LEN);
      break;
    }
    default:
      break; // unknown command, ignore
  }
}

// Polled, not interrupt-driven - see file header note on why this is not
// yet safe to rely on during a blocking move.
void rs485Poll() {
  while (Serial.available()) {
    const uint8_t b = Serial.read();
    switch (rxState) {
      case WAIT_START:
        if (b == FRAME_START) { rxLen = 0; rxState = WAIT_ADDR; }
        break;
      case WAIT_ADDR:
        rxAddr = b; rxState = WAIT_CMD;
        break;
      case WAIT_CMD:
        rxCmd = b; rxState = WAIT_LEN;
        break;
      case WAIT_LEN:
        rxPayloadLen = b;
        rxPayloadIdx = 0;
        if (rxPayloadLen > sizeof(rxFrame)) { rxState = WAIT_START; break; } // malformed, resync
        rxState = (rxPayloadLen == 0) ? WAIT_CRC : WAIT_PAYLOAD;
        break;
      case WAIT_PAYLOAD:
        rxFrame[rxPayloadIdx++] = b;
        if (rxPayloadIdx >= rxPayloadLen) rxState = WAIT_CRC;
        break;
      case WAIT_CRC: {
        uint8_t check[3 + sizeof(rxFrame)];
        uint8_t n = 0;
        check[n++] = rxAddr;
        check[n++] = rxCmd;
        check[n++] = rxPayloadLen;
        for (uint8_t i = 0; i < rxPayloadLen; i++) check[n++] = rxFrame[i];
        if (crc8(check, n) == b) {
          handleFrame(rxAddr, rxCmd, rxFrame, rxPayloadLen);
        } // else: silently drop on CRC mismatch
        rxState = WAIT_START;
        break;
      }
    }
  }
}

// ---------------------------
// Runtime state (motion)
// ---------------------------
float targetAngleDeg = 0.0f;
int minMoveDutyFwd = DEFAULT_MIN_MOVE_DUTY;
int minMoveDutyRev = DEFAULT_MIN_MOVE_DUTY;
int stillDutyMax = DEFAULT_MIN_MOVE_DUTY - CAL_STEP_DUTY;

unsigned long lastSw1EdgeMs = 0;
unsigned long lastSw2EdgeMs = 0;
unsigned long lastFaultLogMs = 0;
unsigned long lastHeartbeatMs = 0;
float lastHeartbeatAngle = 0.0f;
bool lastHeartbeatAngleValid = false;

bool traceMoveEnabled = true;
unsigned long moveSeq = 0;
float lastGoodAngleDeg = 0.0f;
unsigned long i2cErrorCount = 0;
unsigned long lastI2cErrorLogMs = 0;

String debugLineBuf;

// ---------------------------
// AS5600 helpers (unchanged from TestBench04)
// ---------------------------
bool i2cReadBytes(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) { i2cErrorCount++; return false; }
  if (Wire.requestFrom((uint8_t)AS5600_ADDR, len) != len) { i2cErrorCount++; return false; }
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool as5600ReadRaw12(uint8_t reg, uint16_t &out) {
  uint8_t buf[2];
  if (!i2cReadBytes(reg, buf, 2)) return false;
  out = ((uint16_t)buf[0] << 8 | buf[1]) & 0x0FFF;
  return true;
}

bool as5600ReadByte(uint8_t reg, uint8_t &out) {
  uint8_t buf[1];
  if (!i2cReadBytes(reg, buf, 1)) return false;
  out = buf[0];
  return true;
}

float readAngleDeg() {
  uint16_t raw;
  if (as5600ReadRaw12(AS5600_REG_ANGLE, raw)) {
    lastGoodAngleDeg = (raw * 360.0f) / 4096.0f;
  } else if (millis() - lastI2cErrorLogMs > 500) {
    lastI2cErrorLogMs = millis();
    Serial1.print(F("WARN: AS5600 I2C read failed ("));
    Serial1.print(i2cErrorCount);
    Serial1.println(F(" total), using last known angle"));
  }
  return lastGoodAngleDeg;
}

uint8_t readStatus() { uint8_t v = 0; as5600ReadByte(AS5600_REG_STATUS, v); return v; }
uint8_t readAgc() { uint8_t v = 0; as5600ReadByte(AS5600_REG_AGC, v); return v; }
uint16_t readMagnitude() { uint16_t v = 0; as5600ReadRaw12(AS5600_REG_MAGNITUDE, v); return v; }
bool magnetDetected() { return (readStatus() & 0x20) != 0; }

const char *magnetHealthStr(uint8_t status) {
  if (!(status & 0x20)) return "NONE";
  if (status & 0x08) return "STRONG";
  if (status & 0x10) return "WEAK";
  return "OK";
}

void printMagnetLine(bool endLine) {
  const uint8_t status = readStatus();
  Serial1.print(F("magnet MD="));
  Serial1.print((status & 0x20) ? 1 : 0);
  Serial1.print(F(" ML="));
  Serial1.print((status & 0x10) ? 1 : 0);
  Serial1.print(F(" MH="));
  Serial1.print((status & 0x08) ? 1 : 0);
  Serial1.print(F(" AGC="));
  Serial1.print(readAgc());
  Serial1.print(F(" MAG="));
  Serial1.print(readMagnitude());
  Serial1.print(F(" health="));
  Serial1.print(magnetHealthStr(status));
  if (endLine) Serial1.println();
}

// ---------------------------
// Angle math
// ---------------------------
float normalizeDeg(float deg) {
  float d = fmodf(deg, 360.0f);
  if (d < 0) d += 360.0f;
  return d;
}

float angleErrorDeg(float target, float current) {
  return fmodf(target - current + 540.0f, 360.0f) - 180.0f;
}

// ---------------------------
// Distance <-> angle/step helpers
//
// The point of these: nothing that calls into motion should have to know
// "a step is 9 degrees" - it should be able to say "advance 4mm" or
// "advance 2 teeth" and get the same answer. mm is the source of truth
// (it's what a tape/component datasheet actually specifies); everything
// else derives from it via DEG_PER_MM.
// ---------------------------
float degForMm(float mm) { return mm * DEG_PER_MM; }
float mmForDeg(float deg) { return deg / DEG_PER_MM; }

// Rounds to the nearest half-tooth (2mm) - the finest step this wheel can
// resolve, since DEG_PER_HALF_TOOTH is what feedHalfTeeth counts in.
// Clamped to >=1 half-tooth: a zero-length feed isn't a valid pitch.
uint8_t halfTeethForMm(float mm) {
  const long n = lround(mm / (SPROCKET_HOLE_PITCH_MM / 2.0f));
  return (uint8_t)constrain(n, 1, 255);
}

float degForHalfTeeth(uint8_t halfTeeth) { return halfTeeth * DEG_PER_HALF_TOOTH; }
float mmForHalfTeeth(uint8_t halfTeeth) { return halfTeeth * (SPROCKET_HOLE_PITCH_MM / 2.0f); }

// ---------------------------
// Motor drive
// ---------------------------
void brakeMotorA() {
  digitalWrite(PIN_AIN1, HIGH);
  digitalWrite(PIN_AIN2, HIGH);
}

void driveMotorA(int duty, bool forward) {
  duty = constrain(duty, 0, 255);
  const bool effectiveForward = invertMotorA ? !forward : forward;
  if (effectiveForward) {
    analogWrite(PIN_AIN1, duty);
    digitalWrite(PIN_AIN2, LOW);
  } else {
    digitalWrite(PIN_AIN1, LOW);
    analogWrite(PIN_AIN2, duty);
  }
}

void lockMotorBOff() {
  pinMode(PIN_BIN1, OUTPUT);
  pinMode(PIN_BIN2, OUTPUT);
  digitalWrite(PIN_BIN1, HIGH);
  digitalWrite(PIN_BIN2, HIGH);
}

void brakeMotorB() {
  digitalWrite(PIN_BIN1, HIGH);
  digitalWrite(PIN_BIN2, HIGH);
}

void driveMotorB(int duty, bool forward) {
  duty = constrain(duty, 0, 255);
  const bool effectiveForward = invertMotorB ? !forward : forward;
  if (effectiveForward) {
    analogWrite(PIN_BIN1, duty);
    digitalWrite(PIN_BIN2, LOW);
  } else {
    digitalWrite(PIN_BIN1, LOW);
    analogWrite(PIN_BIN2, duty);
  }
}

// ---------------------------
// External LED (PIN_EXT_LED, A3/PC3) - standard LED + series resistor,
// ~20mA direct GPIO drive, plain on/off. Same job as alpha02's ext LED
// (lit "on request" via CMD_SET_EXT_LED / LED ON-OFF, never auto-updated)
// just moved off the ISP header's SCK line onto its own pin - see
// pins_config.h for why 20mA direct drive doesn't need a transistor here.
// (alpha03 briefly used a dedicated SK6812 on this pin instead of a plain
// LED; reverted once a standard LED was chosen for beta1.)
// ---------------------------
void setExtLed(bool on) {
  digitalWrite(PIN_EXT_LED, on ? HIGH : LOW);
}

// ---------------------------
// Power sequencing: 5V rail stability + RS485 bus-connect relay - new in
// beta1. Gates joining the shared RS485 bus (PIN_485_RELAY) until
// PIN_5V_READY has read stable for RELAY_READY_STABLE_MS, so a feeder
// that's still mid-power-up can't electrically disturb a bus other
// feeders/the host are already using.
//
// PIN_5V_READY MUST be read against the internal 1.1V bandgap reference,
// not the default AVCC reference - the divider taps the same 5V rail
// that (by default) IS the ADC's own reference on this board, so an
// AVCC-referenced read of that divider reports the same fixed ratio
// throughout the whole power-up ramp (numerator and reference shrink
// together) and could never detect "still ramping" - it would look
// "stable" from the very first sample.
//
// The 4.7k/1k divider (see pins_config.h) gives ~0.88V at the pin at the
// healthy 5V nominal - ~80% of the 1.1V reference's full scale, clipping
// only above ~6.27V rail. That's comfortably clear of a 5V rail's normal
// tolerance, so unlike an under-sized divider this one does NOT clip
// during normal operation - the stability check below is comparing real,
// non-clipped ADC deltas across the whole ramp, not just detecting "past
// some early threshold and stuck."
// ---------------------------
constexpr unsigned long RELAY_READY_STABLE_MS = 500;
constexpr unsigned long RELAY_READY_TIMEOUT_MS = 5000; // give up and move on (relay stays OFF) rather than hang forever
constexpr uint16_t RELAY_READY_NOISE_BAND = 8; // +/- ADC counts still considered "not moving"
constexpr bool RELAY_ACTIVE_HIGH = true; // HIGH = MOSFET on = relay energized = bus connected - flip if the board inverts this

bool relayEngaged = false;

// Switches to the internal 1.1V reference, discards one conversion (the
// reference/mux needs to settle after switching - AVR datasheet note),
// reads, then restores the default AVCC reference so every OTHER analog
// read in this firmware (PIN_I_MON included) is unaffected.
uint16_t readAdcInternalRef(uint8_t pin) {
  analogReference(INTERNAL);
  analogRead(pin);
  delay(2);
  const uint16_t v = analogRead(pin);
  analogReference(DEFAULT);
  return v;
}

uint16_t readIMonRaw() {
  return analogRead(PIN_I_MON); // default AVCC reference - independent current-sense signal, not the rail itself
}

// TPS26600 IMON (12V-side eFuse) and PIN_5V_READY (5V-rail divider) both
// convert through AnalogCalibration's stored (raw, real-world) point,
// linear through the origin - see the struct's comment above for why.
// Falls back to the factory-calculated defaults until CALI/CALV are run
// against real hardware. No EN/FLT# pins from the TPS26600 are wired to
// the MCU - it can't be, since the MCU only runs once the eFuse is
// already on, so there's no fault state where firmware could still be
// reading a pin to report it.
uint16_t readIMonMilliamps() {
  const uint32_t raw = readIMonRaw();
  return (uint16_t)((raw * (uint32_t)analogCal.imonCalMa) / analogCal.imonCalRaw);
}

// Actual 5V-rail voltage, using the same internal-1.1V-reference read
// waitFor5vStableAndEngageRelay() uses for its plateau check, converted
// through the calibrated (raw, mV) point instead of left as a raw count.
uint16_t read5vRailMillivolts() {
  const uint32_t raw = readAdcInternalRef(PIN_5V_READY);
  return (uint16_t)((raw * (uint32_t)analogCal.v5vCalMv) / analogCal.v5vCalRaw);
}

// Rough debug-only estimate of the 5V rail's load current, from a simple
// power balance: assumes the 12V input rail is at its nominal value and
// that ALL of the measured 12V-side input current (IMON) is being
// converted down to the 5V rail. That's actually the right assumption
// here - the DRV8833's VMOT is tied to the 5V rail (through a ferrite
// bead for noise isolation), not 12V, so the motor is itself a 5V-rail
// load, not something bypassing the regulator. The only thing this
// ignores is conversion (buck) efficiency - real efficiency is under
// 100%, so the true i5v is somewhat LOWER than this estimate, not
// higher. Good enough for "is the 5V rail roughly where I'd expect," not
// a real current measurement - there's no direct 5V-side current sense
// on this board.
constexpr uint32_t V_IN_NOMINAL_MV = 12000;

uint16_t estimateI5vMilliamps() {
  const uint32_t iInMa = readIMonMilliamps();
  const uint32_t v5vMv = read5vRailMillivolts();
  if (v5vMv == 0) return 0;
  return (uint16_t)((V_IN_NOMINAL_MV * iInMa) / v5vMv);
}

void setRelay(bool engaged) {
  digitalWrite(PIN_485_RELAY, engaged == RELAY_ACTIVE_HIGH ? HIGH : LOW);
  relayEngaged = engaged;
}

// Blocking, called once from setup(). Leaves the relay OFF either way if
// it returns false (timed out) - never engages on an unstable/unread rail.
bool waitFor5vStableAndEngageRelay() {
  const unsigned long start = millis();
  uint16_t lastReading = readAdcInternalRef(PIN_5V_READY);
  unsigned long stableSinceMs = millis();

  while (millis() - start < RELAY_READY_TIMEOUT_MS) {
    delay(20);
    const uint16_t reading = readAdcInternalRef(PIN_5V_READY);
    if ((uint16_t)abs((int)reading - (int)lastReading) > RELAY_READY_NOISE_BAND) {
      lastReading = reading;
      stableSinceMs = millis(); // moved - restart the stability window
      continue;
    }
    if (millis() - stableSinceMs >= RELAY_READY_STABLE_MS) {
      Serial1.print(F("5V rail stable (adc="));
      Serial1.print(reading);
      Serial1.println(F("), RS485 relay engaged."));
      setRelay(true);
      return true;
    }
  }
  Serial1.println(F("WARN: 5V rail never stabilized within timeout, RS485 relay stays disconnected."));
  setRelay(false);
  return false;
}

// ---------------------------
// Onboard status RGB (SK6812, PIN_RGB_DATA/PD3) - single pixel.
//
// For now, keep the LED to a simple startup sequence and then off. This is
// easier to use for board bring-up/debugging than the old hue-cycle animation.
// ---------------------------
// KHZ800, not KHZ400: the SK6812SIDE-A-RVS datasheet (T0H max 0.40us, T1H
// 0.65-1.00us, period >=1.20us) matches standard 800kHz WS2812-family
// timing. KHZ400's wider "0" pulse (~0.5us) exceeds this part's T0H max and
// risks being latched as a 1 - that's what was producing solid white.
// Requires the CKDIV8 fuse to be cleared (real 8MHz) - see fuses target.
Adafruit_NeoPixel statusLed(1, PIN_RGB_DATA, NEO_GRB + NEO_KHZ800);

const uint8_t RGB_MAX_BRIGHTNESS = 128; // 0-255, halved for now to reduce current

void setStatusLedColor(uint8_t r, uint8_t g, uint8_t b) {
  statusLed.setPixelColor(0, statusLed.Color(r, g, b));
  statusLed.show();
}

void showStartupLedSequence() {
  statusLed.setBrightness(RGB_MAX_BRIGHTNESS);
  setStatusLedColor(255, 0, 0);
  delay(250);
  setStatusLedColor(0, 255, 0);
  delay(250);
  setStatusLedColor(0, 0, 255);
  delay(250);
  statusLed.clear();
  statusLed.show();
}

// ---------------------------
// Debug self-test: relay + ext LED - beta1 only, bench bring-up aid.
// Exercises the two new GPIO-driven bits of beta1 hardware
// (PIN_485_RELAY, PIN_EXT_LED) with a visual RGB cue each, so a bench
// tester can confirm both actually toggle without reaching for a meter.
//
// Runs unconditionally every boot, called from setup() right before
// waitFor5vStableAndEngageRelay() - this is a throwaway bench toggle,
// NOT the real power-sequencing relay engage (that's
// waitFor5vStableAndEngageRelay() itself, right after). Always leaves
// the relay OFF and the ext LED off when it returns, so the real gate
// starts from a known, disconnected state either way.
// ---------------------------
// Relay hold time and cycle count: 300ms/1 cycle turned out to be too
// quick to reliably hear the click over - longer hold plus a second
// on/off cycle makes it much easier to confirm by ear.
constexpr unsigned long SELFTEST_RELAY_HOLD_MS = 800;
constexpr uint8_t SELFTEST_RELAY_CYCLES = 2;

void runDebugSelfTest() {
  for (uint8_t i = 0; i < SELFTEST_RELAY_CYCLES; i++) {
    Serial1.println(F("Self-test: relay ON"));
    setRelay(true);
    setStatusLedColor(0, 255, 0); // green = relay ON
    delay(SELFTEST_RELAY_HOLD_MS);

    Serial1.println(F("Self-test: relay OFF"));
    setRelay(false);
    setStatusLedColor(255, 0, 0); // red = relay OFF
    delay(SELFTEST_RELAY_HOLD_MS);
  }

  Serial1.println(F("Self-test: ext LED ON"));
  setExtLed(true);
  setStatusLedColor(0, 0, 255); // blue = testing ext LED
  delay(300);
  setExtLed(false);
  Serial1.println(F("Self-test: ext LED OFF"));

  statusLed.clear();
  statusLed.show();
  // loop() repaints the RGB to the real magnet-detect red/green on its
  // very next iteration - no need to set that here.

  // Calibration readout - not a pass/fail check (no expected value to
  // compare against without a real load/meter attached), just prints
  // what CALI/CALV are currently calibrated to and what they resolve to
  // right now, so a bench tester can sanity-check both at a glance
  // alongside the relay/LED test above. loadAnalogCal() must have run
  // before this (see its call site in setup()) or these divide by zero.
  Serial1.print(F("Self-test: IMON cal raw=")); Serial1.print(analogCal.imonCalRaw);
  Serial1.print(F("=")); Serial1.print(analogCal.imonCalMa); Serial1.print(F("mA, now raw="));
  Serial1.print(readIMonRaw()); Serial1.print(F(" = ")); Serial1.print(readIMonMilliamps()); Serial1.println(F("mA"));
  Serial1.print(F("Self-test: 5V_READY cal raw=")); Serial1.print(analogCal.v5vCalRaw);
  Serial1.print(F("=")); Serial1.print(analogCal.v5vCalMv); Serial1.print(F("mV, now raw="));
  Serial1.print(readAdcInternalRef(PIN_5V_READY)); Serial1.print(F(" = ")); Serial1.print(read5vRailMillivolts()); Serial1.println(F("mV"));
}

// CMD_IDENTIFY / debug IDENTIFY: white flashes, distinct from the
// magnet-detect green/red loop() shows normally - so an operator managing
// several feeders on a live rail can pick the right physical unit for a
// given bus address. Blocking (a few hundred ms) is fine: this is a rare,
// deliberate operator action, not something time-critical happens during.
// No need to restore the prior LED color afterward - loop() repaints the
// magnet-status color on its very next iteration anyway.
void identifyBlink(uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    setStatusLedColor(255, 255, 255);
    delay(150);
    statusLed.clear();
    statusLed.show();
    delay(150);
  }
}

bool buttonPressed(int pin, unsigned long &lastEdgeMs) {
  const int activeLevel = BUTTONS_ACTIVE_LOW ? LOW : HIGH;
  if (digitalRead(pin) == activeLevel) {
    const unsigned long now = millis();
    if (now - lastEdgeMs >= DEBOUNCE_MS) {
      lastEdgeMs = now;
      while (digitalRead(pin) == activeLevel) delay(1);
      return true;
    }
  }
  return false;
}

// ---------------------------
// Relay button-test mode - beta1 only, bench bring-up aid. Hold either
// SW1 or SW2 while powering up: RGB goes fixed blue to confirm the mode
// was entered, then once the boot-press is released and things settle,
// goes red with the relay forced OFF (the mode's starting state). From
// there, pressing either button toggles the relay for as long as you
// want to listen to it click - RGB tracks it (green = on, red = off,
// same convention as everywhere else in this firmware).
//
// Simpler and more useful for confirming the click by ear than
// runDebugSelfTest()'s fixed-timing auto-cycle - this replaces that
// piece specifically (SELFTEST/boot still cover ext LED + IMON/5V on
// their own). Deliberately blocks forever once entered: a dedicated
// bench mode, not something that hands back to normal operation - power
// cycle without holding a button to get a normal boot instead. If
// neither button is held when this is called, returns immediately and
// changes nothing.
// ---------------------------
void runRelayButtonTest() {
  const int activeLevel = BUTTONS_ACTIVE_LOW ? LOW : HIGH;
  if (digitalRead(PIN_SW1) != activeLevel && digitalRead(PIN_SW2) != activeLevel) return;

  Serial1.println(F("Relay button-test mode: SW1/SW2 held at boot."));
  setStatusLedColor(0, 0, 255); // blue - confirms entry

  while (digitalRead(PIN_SW1) == activeLevel || digitalRead(PIN_SW2) == activeLevel) delay(10);
  delay(200); // debounce the release before accepting the first toggle press

  bool relayOn = false;
  setRelay(relayOn);
  setStatusLedColor(255, 0, 0); // red - relay OFF, starting state
  Serial1.println(F("Relay OFF. Press SW1 or SW2 to toggle - power-cycle to exit."));

  unsigned long lastEdge1 = 0, lastEdge2 = 0;
  while (true) {
    if (buttonPressed(PIN_SW1, lastEdge1) || buttonPressed(PIN_SW2, lastEdge2)) {
      relayOn = !relayOn;
      setRelay(relayOn);
      setStatusLedColor(relayOn ? 0 : 255, relayOn ? 255 : 0, 0);
      Serial1.println(relayOn ? F("Relay ON.") : F("Relay OFF."));
    }
  }
}

// ---------------------------
// Closed-loop move (unchanged from TestBench04, Serial -> Serial1)
// ---------------------------
uint8_t moveToAngle(float target, unsigned long timeoutMs) {
  const unsigned long moveId = ++moveSeq;
  const unsigned long start = millis();
  unsigned long lastMoveMs = millis();
  unsigned long lastTraceMs = 0;
  float lastAngle = readAngleDeg();

  if (traceMoveEnabled) {
    Serial1.print(F("[move "));
    Serial1.print(moveId);
    Serial1.print(F("] start="));
    Serial1.print(lastAngle, 2);
    Serial1.print(F(" target="));
    Serial1.println(target, 2);
  }

  while (true) {
    if (digitalRead(PIN_nFAULT) == LOW) {
      brakeMotorA();
      Serial1.print(F("[move ")); Serial1.print(moveId);
      Serial1.println(F("] ERROR: DRV8833 fault asserted, move aborted"));
      lastMoveError = ERR_FAULT;
      return ERR_FAULT;
    }
    if (!magnetDetected()) {
      brakeMotorA();
      Serial1.print(F("[move ")); Serial1.print(moveId);
      Serial1.println(F("] ERROR: magnet lost during move (MD=0), move aborted"));
      lastMoveError = ERR_MAGNET_LOST;
      return ERR_MAGNET_LOST;
    }

    const float current = readAngleDeg();
    const float err = angleErrorDeg(target, current);

    if (fabsf(err) <= ANGLE_TOLERANCE_DEG) {
      brakeMotorA();
      Serial1.print(F("[move ")); Serial1.print(moveId);
      Serial1.print(F("] Reached target: ")); Serial1.println(current, 2);
      lastMoveError = ERR_NONE;
      return ERR_NONE;
    }

    const bool forward = err > 0;
    const int duty = (fabsf(err) > CREEP_THRESHOLD_DEG)
        ? FAST_DUTY
        : (forward ? minMoveDutyFwd : minMoveDutyRev);
    driveMotorA(duty, forward);

    if (fabsf(angleErrorDeg(current, lastAngle)) > STALL_MOVE_THRESHOLD_DEG) {
      lastAngle = current;
      lastMoveMs = millis();
    }

    if (traceMoveEnabled && millis() - lastTraceMs >= 150) {
      lastTraceMs = millis();
      Serial1.print(F("[move ")); Serial1.print(moveId);
      Serial1.print(F("] angle=")); Serial1.print(current, 2);
      Serial1.print(F(" err=")); Serial1.print(err, 2);
      Serial1.print(F(" duty=")); Serial1.print(duty);
      Serial1.println(forward ? F(" dir=FWD") : F(" dir=REV"));
    }

    if (millis() - lastMoveMs > STALL_TIMEOUT_MS) {
      brakeMotorA();
      Serial1.print(F("[move ")); Serial1.print(moveId);
      Serial1.println(F("] ERROR: stall detected, move aborted"));
      lastMoveError = ERR_STALL;
      return ERR_STALL;
    }
    if (millis() - start > timeoutMs) {
      brakeMotorA();
      Serial1.print(F("[move ")); Serial1.print(moveId);
      Serial1.println(F("] ERROR: move timeout, aborted"));
      lastMoveError = ERR_TIMEOUT;
      return ERR_TIMEOUT;
    }

    delay(5);
  }
}

uint8_t commandMoveTo(float target, unsigned long timeoutMs) {
  const uint8_t err = moveToAngle(target, timeoutMs);
  if (err != ERR_NONE) targetAngleDeg = readAngleDeg();
  return err;
}

// ---------------------------
// Tape zero + distance-based motion
//
// cfg.tapeZeroRaw is a 12-bit AS5600 count (0-4095, same representation
// the AS5600 itself uses and the same units sent over the bus in
// CMD_GET/SET_COMPONENT), marking "the currently loaded tape's first
// pocket is aligned and ready to present a part". It's meaningless for a
// different component - see setComponentId()'s reset-on-change behavior
// above. Everything here is built on the mm<->degree helpers above plus
// the existing commandMoveTo()/moveToAngle() safety machinery (stall/
// timeout/fault handling) - none of that is duplicated, just parameterized
// by distance instead of a raw target angle.
// ---------------------------
uint16_t angleDegToRaw12(float deg) {
  return (uint16_t)lround(normalizeDeg(deg) * 4096.0f / 360.0f) & 0x0FFF;
}

float raw12ToAngleDeg(uint16_t raw) {
  return (raw & 0x0FFF) * 360.0f / 4096.0f;
}

// Capture the wheel's current position as the hole-aligned reference for
// whatever component is currently loaded. Call this once the operator has
// jogged the wheel (STEP/T - whole/half-tooth increments, no camera or
// MOVEMM needed) so the sprocket hole belonging to the reel's first real
// pocket is seated. PICK_OFFSET_MM (the fixed mechanical hole-to-pick-
// window distance, same for every feeder of this design) is NOT baked in
// here - it's added at use time by moveToTapeZeroPlusMm(), so
// tapeZeroRaw stays a pure "which hole" reference, reusable even if
// PICK_OFFSET_MM is later re-measured/changed.
// Leaves feedHalfTeeth untouched - set that separately via
// setFeedPitchMm(), independently, since pitch and zero-position are two
// separate calibration steps.
void setTapeZeroHere() {
  cfg.tapeZeroRaw = angleDegToRaw12(readAngleDeg());
  saveConfig();
}

// Sets the per-pick feed distance from a physical pitch in mm (e.g. 2 for
// fine/half-tooth-pitch parts, 4 for standard EIA-481 single-hole pitch,
// 8/12/16/24 for wider multi-hole reels) - this is the "distance as a
// parameter, auto-translated to steps" piece: callers (bus command or
// debug port) never need to know or compute a tooth/half-tooth count.
void setFeedPitchMm(float mm) {
  cfg.feedHalfTeeth = halfTeethForMm(mm);
  saveConfig();
}

// Relative move: advance (or retreat, if mm is negative) by a physical
// distance from wherever the wheel currently is. Returns an ERR_* code
// (ERR_NONE on success), not a bool - see moveToAngle().
uint8_t moveByMm(float mm, unsigned long timeoutMs) {
  targetAngleDeg = normalizeDeg(targetAngleDeg + degForMm(mm));
  return commandMoveTo(targetAngleDeg, timeoutMs);
}

// Absolute move: go to a distance offset from the calibrated tape zero,
// automatically including PICK_OFFSET_MM so mm=0 lands at the actual pick
// point (not just "at a hole") - e.g. moveToTapeZeroPlusMm(0) goes to the
// first pocket under the nozzle; moveToTapeZeroPlusMm(N * pitchMm) goes
// to the Nth pocket from zero. Requires setTapeZeroHere() to have been
// called for the current component - callers should check
// tapeZeroRaw != TAPE_ZERO_UNSET first (CMD_FEED_NEXT/FEED do, returning
// ERR_NOT_READY otherwise rather than moving to a meaningless position).
uint8_t moveToTapeZeroPlusMm(float mm, unsigned long timeoutMs) {
  const float zeroDeg = raw12ToAngleDeg(cfg.tapeZeroRaw) + degForMm(PICK_OFFSET_MM);
  targetAngleDeg = normalizeDeg(zeroDeg + degForMm(mm));
  return commandMoveTo(targetAngleDeg, timeoutMs);
}

// Advance by exactly one configured pick pitch (cfg.feedHalfTeeth) from
// the current position - what a real pick sequence calls between picks.
uint8_t feedOnePitch(unsigned long timeoutMs) {
  if (cfg.feedHalfTeeth == FEED_HALF_TEETH_UNSET) { lastMoveError = ERR_NOT_READY; return ERR_NOT_READY; }
  return moveByMm(mmForHalfTeeth(cfg.feedHalfTeeth), timeoutMs);
}

// ---------------------------
// Motor-duty zero-point calibration (unchanged from TestBench04)
//
// NOT the tape zero above - this is DRV8833 electrical characterization
// (still/breakaway PWM duty), unrelated to which component/tape is
// loaded, and reruns on every boot regardless of tapeZeroRaw/feedHalfTeeth.
// ---------------------------
bool rampFindBreakaway(bool forward, float baselineDeg, int &stillMaxOut, int &breakawayOut) {
  stillMaxOut = CAL_START_DUTY - CAL_STEP_DUTY;
  for (int duty = CAL_START_DUTY; duty <= CAL_MAX_DUTY; duty += CAL_STEP_DUTY) {
    driveMotorA(duty, forward);
    delay(CAL_STEP_MS);
    const float moved = fabsf(angleErrorDeg(readAngleDeg(), baselineDeg));
    if (moved > CAL_MOVE_THRESHOLD_DEG) { brakeMotorA(); breakawayOut = duty; return true; }
    stillMaxOut = duty;
  }
  brakeMotorA();
  return false;
}

void calibrateZero() {
  Serial1.println(F("Calibrating zero point (still-duty) ..."));
  brakeMotorA();
  delay(200);

  if (!magnetDetected()) {
    Serial1.println(F("WARN: AS5600 magnet not detected, skipping calibration, using defaults"));
    minMoveDutyFwd = minMoveDutyRev = DEFAULT_MIN_MOVE_DUTY;
    stillDutyMax = DEFAULT_MIN_MOVE_DUTY - CAL_STEP_DUTY;
    return;
  }

  const float startAngle = readAngleDeg();
  int stillFwd = 0, breakFwd = 0;
  const bool okFwd = rampFindBreakaway(true, startAngle, stillFwd, breakFwd);
  delay(150);
  const float afterFwdAngle = readAngleDeg();
  int stillRev = 0, breakRev = 0;
  const bool okRev = rampFindBreakaway(false, afterFwdAngle, stillRev, breakRev);

  if (okFwd && okRev) {
    minMoveDutyFwd = breakFwd + CAL_MARGIN_DUTY;
    minMoveDutyRev = breakRev + CAL_MARGIN_DUTY;
    stillDutyMax = min(stillFwd, stillRev);
    Serial1.println(F("Calibration OK."));
  } else {
    Serial1.println(F("WARN: calibration incomplete, using defaults"));
    minMoveDutyFwd = minMoveDutyRev = DEFAULT_MIN_MOVE_DUTY;
    stillDutyMax = DEFAULT_MIN_MOVE_DUTY - CAL_STEP_DUTY;
  }

  moveToAngle(startAngle, CAL_RESTORE_TIMEOUT_MS);
  targetAngleDeg = readAngleDeg();
}

// ---------------------------
// Boot-time homing gate - beta1 only. "Homing" here is calibrateZero()
// above (the DRV8833 still/breakaway duty characterization), the only
// motor movement this firmware ever does on its own without a host or
// debug-port command asking for it. Deliberately NOT called synchronously
// from setup() - checkHoming() runs from loop() instead and holds off on
// calling it until BOTH of these pass:
//
// 1. HOMING_BOOT_DELAY_MS plus a per-feeder random jitter
//    (HOMING_BOOT_JITTER_MAX_MS) have elapsed since setup() finished the
//    rest of its work. Two reasons: gives this board's own insertion
//    inrush (TPS26600 soft-start, bulk cap charging) time to settle
//    before stacking a motor breakaway-current spike on top of it, and -
//    since every feeder on a bus that just got powered up together hits
//    this same gate within milliseconds of each other - the random
//    jitter spreads their homing attempts across ~2s instead of all of
//    them hitting the shared 12V rail at once. Same idea as
//    DISCOVERY_JITTER_MAX_MS, just a much wider window: that one only
//    has to avoid a bus collision, this one has to avoid a PSU current
//    spike across a whole populated bus.
// 2. Once a magnet IS seen, it has to stay detected continuously for
//    HOMING_MAGNET_STABLE_MS before homing fires - a magnet that was
//    just placed (bench test, wheel/sprocket dropped on while the board
//    was already running) may still be settling into position, and a
//    bare "detected this instant" read is too easy to catch mid-
//    placement. Any dropout resets the stability timer - only a
//    continuous detection counts. No magnet at all just means this timer
//    never starts, so no homing ever fires - calibrateZero() already
//    falls back to defaults without moving the motor in that case, but
//    gating it here means a feeder with nothing to calibrate against
//    doesn't even sit through the boot-delay wait for no reason.
//
// Fires at most once per boot (homingDone latches true right after).
// ---------------------------
constexpr unsigned long HOMING_BOOT_DELAY_MS = 1000;
constexpr unsigned long HOMING_BOOT_JITTER_MAX_MS = 2000; // wider than DISCOVERY_JITTER_MAX_MS - staggers PSU inrush across a whole bus, not just a bus collision
constexpr unsigned long HOMING_MAGNET_STABLE_MS = 5000;

bool homingDone = false;
unsigned long homingReadyAtMs = 0; // set once in setup(), after seedSessionNonce() reseeds random()
unsigned long magnetStableSinceMs = 0; // millis() the magnet was last (re)detected; only meaningful while magnetTrackedLastLoop
bool magnetTrackedLastLoop = false;

void checkHoming() {
  if (homingDone) return;

  const unsigned long now = millis();
  if (now < homingReadyAtMs) return; // still in the boot settle/stagger window

  if (!magnetDetected()) {
    magnetTrackedLastLoop = false;
    return; // nothing to home against yet - keep waiting, not a failure
  }

  if (!magnetTrackedLastLoop) {
    // Magnet just (re)appeared - start the stability timer fresh.
    magnetTrackedLastLoop = true;
    magnetStableSinceMs = now;
    return;
  }

  if (now - magnetStableSinceMs < HOMING_MAGNET_STABLE_MS) return; // not stable long enough yet

  homingDone = true;
  Serial1.println(F("Magnet stable for HOMING_MAGNET_STABLE_MS - homing now."));
  calibrateZero();
}

// ---------------------------
// Debug port (Serial1): status/help + local component config for bench use
// ---------------------------
void printStatus() {
  Serial1.print(F("addr="));
  if (busAddress == ADDR_UNASSIGNED) Serial1.print(F("UNASSIGNED"));
  else Serial1.print(busAddress);
  Serial1.print(F(" component="));
  if (cfg.componentId == COMPONENT_ID_UNSET) Serial1.print(F("UNSET"));
  else Serial1.print(cfg.componentId);
  Serial1.print(F(" tapeZero="));
  if (cfg.tapeZeroRaw == TAPE_ZERO_UNSET) Serial1.print(F("UNSET"));
  else Serial1.print(cfg.tapeZeroRaw);
  Serial1.print(F(" pitchMm="));
  if (cfg.feedHalfTeeth == FEED_HALF_TEETH_UNSET) Serial1.print(F("UNSET"));
  else Serial1.print(mmForHalfTeeth(cfg.feedHalfTeeth), 1);
  Serial1.print(F(" invertA="));
  Serial1.print(invertMotorA ? F("Y") : F("N"));
  Serial1.print(F(" invertB="));
  Serial1.print(invertMotorB ? F("Y") : F("N"));
  Serial1.print(F(" tapeWidthMm="));
  if (hwInfo.tapeWidthMm == TAPE_WIDTH_UNSET) Serial1.print(F("UNSET"));
  else Serial1.print(hwInfo.tapeWidthMm);
  Serial1.print(F(" serial="));
  Serial1.print(factorySerialValid ? F("OK(SERIAL cmd)") : F("UNAVAILABLE"));
  Serial1.print(F(" relay="));
  Serial1.print(relayEngaged ? F("CONNECTED") : F("disconnected"));
  Serial1.print(F(" iMonRaw="));
  Serial1.print(readIMonRaw());
  Serial1.print(F(" iMonMa="));
  Serial1.print(readIMonMilliamps());
  Serial1.print(F(" i5vEstMa="));
  Serial1.print(estimateI5vMilliamps());
  Serial1.print(F(" angle="));
  Serial1.print(readAngleDeg(), 2);
  Serial1.print(F(" target="));
  Serial1.print(targetAngleDeg, 2);
  Serial1.print(F(" | "));
  printMagnetLine(false);
  Serial1.print(F(" | i2cErrors="));
  Serial1.print(i2cErrorCount);
  Serial1.print(F(" lastMoveErr="));
  Serial1.println(lastMoveError);
}

void printHelp() {
  Serial1.println(F("Debug port commands (newline terminated):"));
  Serial1.println(F("  A<deg> / T<index> / STEP+1 / STEP-1 / STEP+0.5 / STEP-0.5"));
  Serial1.println(F("  ZERO / STOP / STATUS / TRACE ON / TRACE OFF / HELP"));
  Serial1.println(F("  LED ON / LED OFF  external LED (A3/PC3) on/off"));
  Serial1.println(F("  RELAY ON / OFF  force the RS485 bus-connect relay, bench-only override -"));
  Serial1.println(F("                  bypasses the 5V-stable gate from setup(), does not touch it"));
  Serial1.println(F("  SELFTEST        re-run the boot self-test on demand: relay x2 (audible"));
  Serial1.println(F("                  click each way), ext LED, IMON/5V readout - CAUTION: ends"));
  Serial1.println(F("                  with the relay forced OFF, disconnecting a live bus link"));
  Serial1.println(F("                  until RELAY ON or a reboot re-engages it"));
  Serial1.println(F("  (not a typed command) hold SW1 or SW2 while powering up for relay"));
  Serial1.println(F("                  button-test mode: RGB blue -> red/relay off, then each"));
  Serial1.println(F("                  button press toggles the relay (green=on/red=off) for as"));
  Serial1.println(F("                  long as you want - power-cycle without holding to exit"));
  Serial1.println(F("  IMON            print PIN_I_MON raw ADC + calibrated mA (TPS26600 IMON)"));
  Serial1.println(F("  5VSTATUS        print PIN_5V_READY raw ADC (internal 1.1V ref) + calibrated"));
  Serial1.println(F("                  mV + estimated i5v (see I5V) + current relay state"));
  Serial1.println(F("  I5V             print estimated 5V-rail current (mA) from a 12V-nominal"));
  Serial1.println(F("                  power balance against IMON - rough, debug only, ignores"));
  Serial1.println(F("                  buck efficiency (see estimateI5vMilliamps())"));
  Serial1.println(F("  CALI <mA>       calibrate IMON: capture the current PIN_I_MON raw ADC"));
  Serial1.println(F("                  reading against a real load current measured with a bench"));
  Serial1.println(F("                  ammeter right now (e.g. CALI 87.5), persisted in EEPROM"));
  Serial1.println(F("  CALV <V>        calibrate 5V_READY the same way, against a bench multimeter"));
  Serial1.println(F("                  reading of the actual 5V rail right now (e.g. CALV 5.02)"));
  Serial1.println(F("  CALSTATUS       print the stored IMON/5V_READY calibration points"));
  Serial1.println(F("  CALRESET        reset IMON/5V_READY calibration to the factory-calculated"));
  Serial1.println(F("                  defaults (undoes CALI/CALV)"));
  Serial1.println(F("  INVERTA ON/OFF  flip motor A direction (mirrors CMD_SET_INVERT_DIR)"));
  Serial1.println(F("  INVERTB ON/OFF  flip motor B direction (mirrors CMD_SET_INVERT_DIR)"));
  Serial1.println(F("  IDENTIFY [n]    blink status LED white n times (default 3), mirrors"));
  Serial1.println(F("                  CMD_IDENTIFY - find which physical unit an address is"));
  Serial1.println(F("  SETWIDTH <mm>   set this unit's tape width (8/12/16/24/32/44/56),"));
  Serial1.println(F("                  assembly/bench-time only - not reset by anything else,"));
  Serial1.println(F("                  mirrors CMD_SET_HW_INFO"));
  Serial1.println(F("  SERIAL          print the AT24CS02's factory-programmed 128-bit serial"));
  Serial1.println(F("                  number as hex, mirrors CMD_GET_SERIAL"));
  Serial1.println(F("  WHOAMI          print bus address + loaded component config"));
  Serial1.println(F("  SIMADDR <n>     force bus address n locally (1-247), bench-only,"));
  Serial1.println(F("                  bypasses CMD_DISCOVER/CMD_ASSIGN_ADDR - for testing"));
  Serial1.println(F("                  motion/config commands without a host on the bus yet"));
  Serial1.println(F("  COMPONENT <id>  set loaded component id (mirrors CMD_SET_COMPONENT);"));
  Serial1.println(F("                  resets tape zero + pitch if id actually changed"));
  Serial1.println(F("  --- tape zero / pitch calibration ---"));
  Serial1.println(F("  ZEROHERE        jog with STEP/T/buttons (whole/half-tooth, no camera"));
  Serial1.println(F("                  needed) so the first pocket's sprocket hole is seated,"));
  Serial1.println(F("                  then run this - PICK_OFFSET_MM (fixed, same on every"));
  Serial1.println(F("                  feeder) is added automatically at move time, not here"));
  Serial1.println(F("  PITCH <mm>      set per-pick feed distance in mm (e.g. PITCH 4 for"));
  Serial1.println(F("                  standard EIA-481, PITCH 2 for fine pitch, PITCH 8/12/16/24"));
  Serial1.println(F("                  for wider multi-hole reels) - auto-translated to steps"));
  Serial1.println(F("  RESETCFG        clear tape zero + pitch only (keeps component id)"));
  Serial1.println(F("  --- distance-based motion (mm, not degrees/teeth) ---"));
  Serial1.println(F("  GOTOZERO        move to the calibrated pick point (tape zero + PICK_OFFSET_MM)"));
  Serial1.println(F("  GOMM <mm>       move to tape zero + PICK_OFFSET_MM + mm (absolute)"));
  Serial1.println(F("  MOVEMM <mm>     move by mm relative to current position - only needed"));
  Serial1.println(F("                  routinely to (re-)measure PICK_OFFSET_MM itself with a"));
  Serial1.println(F("                  camera on a reference unit, not per-reel anymore"));
  Serial1.println(F("  FEED            advance by the configured PITCH (next pocket)"));
  Serial1.println(F("  --- low-level bus commands, for reference (see project.md) ---"));
  Serial1.println(F("  FEEDCFG <zeroRaw 0-4095> <halfTeeth 1-255>  set both fields directly,"));
  Serial1.println(F("                  same values CMD_SET_FEED_CONFIG takes over the bus"));
}

void handleDebugLine(String line) {
  line.trim();
  if (line.length() == 0) return;
  String upper = line; upper.toUpperCase();

  if (upper == "STATUS") { printStatus(); return; }
  if (upper == "ZERO") { calibrateZero(); return; }
  if (upper == "STOP") { brakeMotorA(); brakeMotorB(); Serial1.println(F("Stopped.")); return; }
  if (upper == "LED ON") { setExtLed(true); Serial1.println(F("External LED ON.")); return; }
  if (upper == "LED OFF") { setExtLed(false); Serial1.println(F("External LED OFF.")); return; }
  if (upper == "RELAY ON") { setRelay(true); Serial1.println(F("RS485 relay forced CONNECTED (bench override).")); return; }
  if (upper == "RELAY OFF") { setRelay(false); Serial1.println(F("RS485 relay forced disconnected (bench override).")); return; }
  if (upper == "SELFTEST") {
    Serial1.println(F("Running self-test (relay x2, ext LED, IMON/5V readout)..."));
    runDebugSelfTest();
    Serial1.println(F("Self-test done."));
    return;
  }
  if (upper == "IMON") {
    Serial1.print(F("I_MON raw=")); Serial1.print(readIMonRaw());
    Serial1.print(F(" mA=")); Serial1.println(readIMonMilliamps());
    return;
  }
  if (upper == "5VSTATUS") {
    Serial1.print(F("5V_READY raw (internal 1.1V ref)="));
    Serial1.print(readAdcInternalRef(PIN_5V_READY));
    Serial1.print(F(" mV=")); Serial1.print(read5vRailMillivolts());
    Serial1.print(F(" i5vEstMa=")); Serial1.print(estimateI5vMilliamps());
    Serial1.print(F(" relay="));
    Serial1.println(relayEngaged ? F("CONNECTED") : F("disconnected"));
    return;
  }
  if (upper == "I5V") {
    Serial1.print(F("i5vEst=")); Serial1.print(estimateI5vMilliamps());
    Serial1.println(F("mA (rough power-balance estimate, see HELP)"));
    return;
  }
  if (upper.startsWith("CALI ")) {
    const float ma = upper.substring(5).toFloat();
    if (ma <= 0) { Serial1.println(F("Refused: CALI needs a positive measured mA (e.g. CALI 87.5).")); return; }
    const uint16_t raw = readIMonRaw();
    if (raw == 0) { Serial1.println(F("Refused: PIN_I_MON reads 0 raw right now, can't calibrate against it.")); return; }
    analogCal.imonCalRaw = raw;
    analogCal.imonCalMa = (uint16_t)(ma + 0.5f);
    saveAnalogCal();
    Serial1.print(F("IMON calibrated: raw=")); Serial1.print(analogCal.imonCalRaw);
    Serial1.print(F(" = ")); Serial1.print(analogCal.imonCalMa); Serial1.println(F("mA"));
    return;
  }
  if (upper.startsWith("CALV ")) {
    const float v = upper.substring(5).toFloat();
    if (v <= 0) { Serial1.println(F("Refused: CALV needs a positive measured volts value (e.g. CALV 5.02).")); return; }
    const uint16_t raw = readAdcInternalRef(PIN_5V_READY);
    if (raw == 0) { Serial1.println(F("Refused: PIN_5V_READY reads 0 raw right now, can't calibrate against it.")); return; }
    analogCal.v5vCalRaw = raw;
    analogCal.v5vCalMv = (uint16_t)(v * 1000.0f + 0.5f);
    saveAnalogCal();
    Serial1.print(F("5V_READY calibrated: raw=")); Serial1.print(analogCal.v5vCalRaw);
    Serial1.print(F(" = ")); Serial1.print(analogCal.v5vCalMv); Serial1.println(F("mV"));
    return;
  }
  if (upper == "CALSTATUS") {
    Serial1.print(F("IMON: raw=")); Serial1.print(analogCal.imonCalRaw);
    Serial1.print(F(" = ")); Serial1.print(analogCal.imonCalMa); Serial1.println(F("mA"));
    Serial1.print(F("5V_READY: raw=")); Serial1.print(analogCal.v5vCalRaw);
    Serial1.print(F(" = ")); Serial1.print(analogCal.v5vCalMv); Serial1.println(F("mV"));
    return;
  }
  if (upper == "CALRESET") {
    resetAnalogCalToFactoryDefaults();
    saveAnalogCal();
    Serial1.println(F("IMON/5V_READY calibration reset to factory-calculated defaults."));
    return;
  }
  if (upper == "INVERTA ON") { invertMotorA = true; Serial1.println(F("Motor A direction inverted.")); return; }
  if (upper == "INVERTA OFF") { invertMotorA = false; Serial1.println(F("Motor A direction normal.")); return; }
  if (upper == "INVERTB ON") { invertMotorB = true; Serial1.println(F("Motor B direction inverted.")); return; }
  if (upper == "INVERTB OFF") { invertMotorB = false; Serial1.println(F("Motor B direction normal.")); return; }
  if (upper == "IDENTIFY" || upper.startsWith("IDENTIFY ")) {
    const int n = upper.length() > 8 ? upper.substring(9).toInt() : 0;
    identifyBlink(n > 0 ? (uint8_t)n : 3);
    return;
  }
  if (upper.startsWith("SETWIDTH ")) {
    const int mm = upper.substring(9).toInt();
    if (!setTapeWidthMm((uint8_t)mm)) {
      Serial1.println(F("Refused: width must be 8/12/16/24/32/44/56, or AT24CS02 write failed (chip absent?)."));
    } else {
      Serial1.print(F("Tape width set: ")); Serial1.print(mm); Serial1.println(F("mm"));
    }
    return;
  }
  if (upper == "SERIAL") {
    if (!factorySerialValid) {
      Serial1.println(F("AT24CS02 factory serial unavailable (chip absent on this board?)."));
    } else {
      Serial1.print(F("Serial: "));
      for (uint8_t i = 0; i < FACTORY_SERIAL_LEN; i++) {
        if (factorySerial[i] < 0x10) Serial1.print('0');
        Serial1.print(factorySerial[i], HEX);
      }
      Serial1.println();
    }
    return;
  }
  if (upper == "HELP" || upper == "?") { printHelp(); return; }
  if (upper == "TRACE ON") { traceMoveEnabled = true; return; }
  if (upper == "TRACE OFF") { traceMoveEnabled = false; return; }
  if (upper == "WHOAMI") { printStatus(); return; }
  if (upper.startsWith("SIMADDR ")) {
    const int n = upper.substring(8).toInt();
    if (n < ADDR_MIN || n > ADDR_MAX) {
      Serial1.println(F("Refused: address must be 1-247."));
    } else {
      busAddress = (uint8_t)n;
      Serial1.print(F("Bench-only address forced: ")); Serial1.println(n);
    }
    return;
  }
  if (upper.startsWith("COMPONENT ")) {
    const long id = upper.substring(10).toInt();
    if (id < 0 || id > 65534) {
      Serial1.println(F("Refused: component id must be 0-65534."));
    } else {
      setComponentId((uint16_t)id);
      Serial1.print(F("Component set: ")); Serial1.println(id);
    }
    return;
  }
  if (upper.startsWith("FEEDCFG ")) {
    const String rest = upper.substring(8);
    const int sep = rest.indexOf(' ');
    if (sep < 0) { Serial1.println(F("Usage: FEEDCFG <zeroRaw 0-4095> <halfTeeth 1-255>")); return; }
    const long zero = rest.substring(0, sep).toInt();
    const long half = rest.substring(sep + 1).toInt();
    if (zero < 0 || zero > 4095 || half < 1 || half > 255) {
      Serial1.println(F("Refused: zeroRaw 0-4095, halfTeeth 1-255."));
    } else {
      setFeedConfig((uint16_t)zero, (uint8_t)half);
      Serial1.println(F("Feed config saved."));
    }
    return;
  }
  if (upper == "RESETCFG") { manualResetFeedConfig(); Serial1.println(F("Feed config reset.")); return; }
  if (upper == "ZEROHERE") {
    setTapeZeroHere();
    Serial1.print(F("Tape zero set at raw=")); Serial1.println(cfg.tapeZeroRaw);
    return;
  }
  if (upper.startsWith("PITCH ")) {
    const float mm = upper.substring(6).toFloat();
    if (mm <= 0) {
      Serial1.println(F("Refused: pitch must be a positive mm value (e.g. PITCH 4)."));
    } else {
      setFeedPitchMm(mm);
      Serial1.print(F("Pitch set: ")); Serial1.print(mm);
      Serial1.print(F("mm -> feedHalfTeeth=")); Serial1.println(cfg.feedHalfTeeth);
    }
    return;
  }
  if (upper == "GOTOZERO") {
    if (cfg.tapeZeroRaw == TAPE_ZERO_UNSET) {
      Serial1.println(F("Refused: tape zero not set, run ZEROHERE first."));
    } else {
      moveToTapeZeroPlusMm(0.0f, MOVE_TIMEOUT_MS);
    }
    return;
  }
  if (upper.startsWith("GOMM ")) {
    moveToTapeZeroPlusMm(upper.substring(5).toFloat(), MOVE_TIMEOUT_MS);
    return;
  }
  if (upper.startsWith("MOVEMM ")) {
    moveByMm(upper.substring(7).toFloat(), MOVE_TIMEOUT_MS);
    return;
  }
  if (upper == "FEED") {
    if (cfg.feedHalfTeeth == FEED_HALF_TEETH_UNSET) {
      Serial1.println(F("Refused: feedHalfTeeth not configured, run PITCH first."));
    } else {
      feedOnePitch(MOVE_TIMEOUT_MS);
    }
    return;
  }
  if (upper.startsWith("STEP")) {
    targetAngleDeg = normalizeDeg(targetAngleDeg + upper.substring(4).toFloat() * DEG_PER_TOOTH);
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
    return;
  }
  if (upper.startsWith("T")) {
    int idx = upper.substring(1).toInt();
    idx = ((idx % TOOTH_COUNT) + TOOTH_COUNT) % TOOTH_COUNT;
    targetAngleDeg = idx * DEG_PER_TOOTH;
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
    return;
  }
  if (upper.startsWith("A")) {
    targetAngleDeg = normalizeDeg(upper.substring(1).toFloat());
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
    return;
  }
  Serial1.println(F("Unknown command. Type HELP."));
}

// ---------------------------
// Setup / loop
// ---------------------------
void setup() {
  Serial1.begin(DEBUG_BAUD); // moved early - runRelayButtonTest() below needs it, well before the rest of setup() would otherwise get to it

  pinMode(PIN_SW1, INPUT_PULLUP);
  pinMode(PIN_SW2, INPUT_PULLUP);
  pinMode(PIN_AIN1, OUTPUT);
  pinMode(PIN_AIN2, OUTPUT);
  pinMode(PIN_nSLEEP, OUTPUT);
  pinMode(PIN_nFAULT, INPUT_PULLUP);
  pinMode(PIN_FAULT_LED, OUTPUT);
  pinMode(PIN_RGB_DATA, OUTPUT);
  pinMode(PIN_EXT_LED, OUTPUT);
  setExtLed(false);
  pinMode(PIN_I_MON, INPUT);
  pinMode(PIN_5V_READY, INPUT);
  pinMode(PIN_485_RELAY, OUTPUT);
  setRelay(false); // disconnected by default - only engaged once the 5V rail is confirmed stable, below

  statusLed.begin();
  statusLed.clear();
  statusLed.show();

  // runRelayButtonTest() disabled for v0.01a - bench-only test aid
  // (hold SW1/SW2 at boot to hijack the relay for manual toggling),
  // inherited from beta1 where it's still available. Function is still
  // defined below and reachable if this ever needs re-enabling; a
  // stepping-stone/MAJOR release shouldn't have boot behavior a stray
  // held button can change by default.
  // runRelayButtonTest();

  showStartupLedSequence();

  brakeMotorA();
  lockMotorBOff();
  digitalWrite(PIN_nSLEEP, HIGH); // wake DRV8833

  Wire.begin();
  Wire.setClock(I2C_CLOCK_HZ);

  rs485Init();
  loadAnalogCal(); // still needed unconditionally - SELFTEST debug command and CALI/CALV/readIMonMilliamps()/read5vRailMillivolts() all depend on it being loaded, not just the disabled auto-self-test below
  // runDebugSelfTest() no longer runs automatically for v0.01a - bench-
  // only test aid (relay/ext LED toggle + IMON/5V readout on every
  // boot), inherited from beta1. Still reachable on demand via the
  // SELFTEST debug command for real diagnostics; just not forced on
  // every power-up anymore.
  // runDebugSelfTest();
  waitFor5vStableAndEngageRelay(); // relay only ever engages once this confirms the 5V rail is stable - the real safety gate, unaffected by the test-aid removals above

  seedSessionNonce(); // also reseeds random() - safe to draw the homing jitter right after
  homingReadyAtMs = millis() + HOMING_BOOT_DELAY_MS + random(0, HOMING_BOOT_JITTER_MAX_MS + 1);
  loadConfig(); // busAddress always starts ADDR_UNASSIGNED - re-earned via CMD_DISCOVER each boot
  loadHwInfo(); // tape width etc - set once at assembly, never reset by config changes
  loadFactorySerial(); // AT24CS02 identification page - read fresh every boot, never cached to EEPROM

  Serial1.println(F("v0.01a feeder firmware ready"));
  printStatus();

  if (!magnetDetected()) {
    Serial1.println(F("WARN: AS5600 magnet not detected at startup - homing deferred until one is."));
  }

  targetAngleDeg = readAngleDeg();
  // Homing (calibrateZero()) is intentionally NOT called here - see
  // checkHoming() in loop(), which it's deferred to: a boot-settle/
  // stagger delay plus a magnet-placement stability check both have to
  // pass first, and neither should block the debug port or RS485 bus
  // from responding while they elapse.
  printHelp();
  printStatus();
}

void loop() {
  const unsigned long now = millis();

  // Alpha hardware bring-up: RGB shows AS5600 magnet-detect status instead
  // of staying off - green = magnet seen, red = not. Not a real feature.
  if (magnetDetected()) {
    setStatusLedColor(0, 255, 0);
  } else {
    setStatusLedColor(255, 0, 0);
  }

  if (now - lastHeartbeatMs >= 3000) {
    lastHeartbeatMs = now;
    // Without a magnet, the AS5600 still ACKs over I2C and readAngleDeg()
    // still returns *something*, but it's just noise with no real
    // magnetic field to lock onto - comparing it across heartbeats would
    // just be comparing noise to noise and firing false "wheel moved"
    // warnings. Skip the check entirely, and drop the baseline
    // (lastHeartbeatAngleValid = false) so that once a magnet reappears,
    // the next heartbeat starts a fresh comparison instead of comparing
    // a real angle against whatever noise was captured before.
    if (magnetDetected()) {
      const float angleNow = readAngleDeg();
      if (lastHeartbeatAngleValid && fabsf(angleErrorDeg(angleNow, lastHeartbeatAngle)) > 1.0f) {
        Serial1.println(F("WARN: wheel moved between heartbeats with no move in progress"));
      }
      lastHeartbeatAngle = angleNow;
      lastHeartbeatAngleValid = true;
    } else {
      lastHeartbeatAngleValid = false;
    }
  }

  if (digitalRead(PIN_nFAULT) == LOW) {
    brakeMotorA();
    digitalWrite(PIN_FAULT_LED, HIGH);
    if (now - lastFaultLogMs >= 500) {
      lastFaultLogMs = now;
      Serial1.println(F("FAULT asserted on DRV8833 -> motor A braked"));
    }
    return;
  }
  digitalWrite(PIN_FAULT_LED, LOW);

  checkHoming(); // no-op once homingDone; see its own comment for the boot-delay/stagger + magnet-stability gate

  if (buttonPressed(PIN_SW1, lastSw1EdgeMs)) {
    targetAngleDeg = normalizeDeg(targetAngleDeg + DEG_PER_TOOTH);
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
  }
  if (buttonPressed(PIN_SW2, lastSw2EdgeMs)) {
    Serial1.println(F("SW2: jogging motor B (open-loop, no encoder on this motor)"));
    driveMotorB(JOG_DUTY, true);
    delay(JOG_DURATION_MS);
    brakeMotorB();
  }

  rs485Poll();

  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n' || c == '\r') {
      if (debugLineBuf.length() > 0) { handleDebugLine(debugLineBuf); debugLineBuf = ""; }
    } else {
      debugLineBuf += c;
    }
  }
}
