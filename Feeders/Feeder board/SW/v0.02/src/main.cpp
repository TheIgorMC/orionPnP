#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <math.h>
#include <Adafruit_NeoPixel.h>
#include "pins_config.h"

/*
  v0.02 - forked from v0.01a. Adds, on top of everything below:
  - Per-feeder peel calibration: the peel run time that suits THIS
    feeder's mechanics is stored in the ATmega's internal EEPROM
    (PeelCal), set with CMD_SET_PEEL_TIME / PEELCAL <ms>, read with
    CMD_GET_PEEL_TIME, and used by CMD_PEEL with only a direction byte
    (PEELRUN on the debug port). Independent of the loaded component, so
    it survives component changes and RESETCFG.
  - CMD_JOG: relative move over the bus in 0.1mm units (signed), replying
    with the new raw angle - lets a host seat a sprocket hole without the
    debug port, then CMD_ZERO_HERE.
  - AT24C02 support: a plain AT24C02 has no factory serial page (0x58),
    which is why CMD_GET_SERIAL NACKed on a board that otherwise talks
    to the EEPROM fine. CMD_I2C_SCAN shows what actually answers on the
    bus; CMD_SET_SERIAL programs a 16-byte serial into the normal EEPROM
    (read back and verified), used whenever no factory serial exists.
  - AT24 writes are now split on 8-byte page boundaries (the AT24C02
    wraps within a page, so a write crossing one corrupts the start of
    that page).

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

  Also v0.01a: status RGB = yellow booting / blue ready / red error /
  purple moving; peel motor runnable on its own (SW2 hold, PEEL, CMD_PEEL)
  for tape tensioning; compact debug output (TRACE off by default); no
  String class in the debug parser. See project.md items 5-9.

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
// Peel motor is also run on its own to tension the cover tape: SW2 runs it
// for as long as the button is held (PEEL_HOLD_MAX_MS safety cap), PEEL
// debug command / CMD_PEEL run it for a set time.
const int   PEEL_DUTY = FAST_DUTY;
const unsigned long PEEL_HOLD_MAX_MS = 10000;
const unsigned long PEEL_CMD_MAX_MS = 5000;
const unsigned long PEEL_CAL_MIN_MS = 10;

const bool BUTTONS_ACTIVE_LOW = true;
const unsigned long DEBOUNCE_MS = 20;

// Feed direction (motor A). invertMotorA flips the motor drive AND the
// AS5600 angle together - flipping only one of them would make the closed
// loop drive away from its target and never settle. Logical "forward"
// (increasing angle) always means "feed tape forward".
//
// Base state (invertMotorA = false): motor drive inverted, encoder
// normal - the combination the loop was validated with, and confirmed on
// the real v0.01a board to settle fine but feed the tape BACKWARDS.
// invertMotorA = true flips both from there (motor not inverted, encoder
// mirrored), so it's the default. INVERTA / CMD_SET_INVERT_DIR still flip
// it at runtime (RAM-only) - flipping mirrors the angle, so any saved tape
// zero no longer points at the same hole.
//
// invertMotorB is a plain motor-polarity flip (peel motor has no encoder).
constexpr bool MOTOR_A_BASE_INVERT = true;
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
// Per-feeder peel calibration (v0.02): how long the peel motor should run
// to take up the cover tape released by one feed on THIS feeder. Depends
// on this unit's mechanics (spool/roller, belt, motor), not on which reel
// is loaded, so like AnalogCalibration it is deliberately separate from
// FeederConfig - a component change or RESETCFG must never clear it.
// Internal EEPROM, next to the analog calibration.
// ---------------------------
struct PeelCal {
  uint16_t peelMs; // PEEL_MS_UNSET until calibrated
  uint8_t crc;
};

constexpr int EEPROM_PEEL_CAL_LOCATION = 32; // after AnalogCalibration (16..24)
constexpr uint16_t PEEL_MS_UNSET = 0xFFFF;

PeelCal peelCal;

uint8_t peelCalCrc(const PeelCal &p) {
  return crc8(reinterpret_cast<const uint8_t *>(&p), sizeof(PeelCal) - 1);
}

void savePeelCal() {
  peelCal.crc = peelCalCrc(peelCal);
  EEPROM.put(EEPROM_PEEL_CAL_LOCATION, peelCal);
}

void loadPeelCal() {
  EEPROM.get(EEPROM_PEEL_CAL_LOCATION, peelCal);
  if (peelCal.crc != peelCalCrc(peelCal) || peelCal.peelMs < PEEL_CAL_MIN_MS || peelCal.peelMs > PEEL_CMD_MAX_MS) {
    peelCal.peelMs = PEEL_MS_UNSET; // blank/garbage EEPROM: uncalibrated, not a guessed default
  }
}

bool setPeelCalMs(uint16_t ms) {
  if (ms < PEEL_CAL_MIN_MS || ms > PEEL_CMD_MAX_MS) return false;
  peelCal.peelMs = ms;
  savePeelCal();
  return true;
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

// The AT24C02/AT24CS02 write buffer is 8 bytes and wraps WITHIN the page:
// a write that crosses an 8-byte boundary overwrites the start of the same
// page instead of continuing. Split every write on page boundaries.
constexpr uint8_t AT24_PAGE_SIZE = 8;

bool at24csWriteBytes(uint8_t i2cAddr, uint8_t memAddr, const uint8_t *data, uint8_t len) {
  while (len > 0) {
    uint8_t chunk = AT24_PAGE_SIZE - (memAddr % AT24_PAGE_SIZE);
    if (chunk > len) chunk = len;
    Wire.beginTransmission(i2cAddr);
    Wire.write(memAddr);
    Wire.write(data, chunk);
    if (Wire.endTransmission() != 0) return false;
    delay(AT24CS02_WRITE_CYCLE_MS);
    memAddr += chunk;
    data += chunk;
    len -= chunk;
  }
  return true;
}

// Serial number, two possible sources:
//  - AT24CS02: read-only factory identification page at 0x58 (can't be
//    written - it is what makes the part an "-CS").
//  - plain AT24C02 (no serial page, 0x58 never answers): a serial
//    programmed by us with CMD_SET_SERIAL / SETSERIAL, stored in the
//    normal EEPROM at AT24_USER_SERIAL_MEM_ADDR with a CRC.
// The factory serial wins whenever it is readable.
constexpr uint8_t FACTORY_SERIAL_LEN = 16; // 128 bits
constexpr uint8_t AT24_USER_SERIAL_MEM_ADDR = 0x10; // page-aligned, clear of hwInfo at 0x00
uint8_t factorySerial[FACTORY_SERIAL_LEN];
bool factorySerialValid = false; // true = AT24CS02 factory page readable
bool userSerialValid = false;    // true = serial found/programmed in plain EEPROM

bool serialAvailable() { return factorySerialValid || userSerialValid; }

uint8_t serialBlockCrc(const uint8_t *s) { return crc8(s, FACTORY_SERIAL_LEN); }

void loadFactorySerial() {
  factorySerialValid = at24csReadBytes(AT24CS02_SERIAL_ADDR, 0x00, factorySerial, FACTORY_SERIAL_LEN);
  userSerialValid = false;
  if (factorySerialValid) return;

  // No factory page: look for a serial we programmed ourselves.
  uint8_t block[FACTORY_SERIAL_LEN + 1];
  if (at24csReadBytes(AT24CS02_EEPROM_ADDR, AT24_USER_SERIAL_MEM_ADDR, block, sizeof(block))
      && block[FACTORY_SERIAL_LEN] == serialBlockCrc(block)) {
    memcpy(factorySerial, block, FACTORY_SERIAL_LEN);
    userSerialValid = true;
    return;
  }
  for (uint8_t i = 0; i < FACTORY_SERIAL_LEN; i++) factorySerial[i] = 0;
  Serial1.println(F("WARN: no serial (no AT24CS02 page, none programmed)"));
}

// Program a serial into the normal EEPROM and read it back to verify.
// Returns ERR_NONE, ERR_LOCKED (a factory serial exists - nothing to do)
// or ERR_I2C (chip not answering / readback mismatch).
uint8_t programUserSerial(const uint8_t *serial16);

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
    Serial1.println(F("WARN: AT24CS02 not responding"));
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
void runPeel(bool forward, unsigned long ms);
uint8_t moveByMm(float mm, unsigned long timeoutMs);
extern float targetAngleDeg; // defined with the rest of motion runtime state
uint8_t commandMoveTo(float target, unsigned long timeoutMs);
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
constexpr uint8_t ERR_I2C = 0x07;          // v0.02: AT24 EEPROM didn't answer, or readback didn't match
constexpr uint8_t ERR_LOCKED = 0x08;       // v0.02: factory serial present, can't be overridden

uint8_t lastMoveError = ERR_NONE;

uint8_t programUserSerial(const uint8_t *serial16) {
  if (factorySerialValid) return ERR_LOCKED;
  uint8_t block[FACTORY_SERIAL_LEN + 1];
  memcpy(block, serial16, FACTORY_SERIAL_LEN);
  block[FACTORY_SERIAL_LEN] = serialBlockCrc(block);
  if (!at24csWriteBytes(AT24CS02_EEPROM_ADDR, AT24_USER_SERIAL_MEM_ADDR, block, sizeof(block))) return ERR_I2C;
  uint8_t back[sizeof(block)];
  if (!at24csReadBytes(AT24CS02_EEPROM_ADDR, AT24_USER_SERIAL_MEM_ADDR, back, sizeof(back))) return ERR_I2C;
  if (memcmp(block, back, sizeof(block)) != 0) return ERR_I2C;
  memcpy(factorySerial, serial16, FACTORY_SERIAL_LEN);
  userSerialValid = true;
  return ERR_NONE;
}

// Responding 7-bit I2C addresses, for diagnosing what is actually on the
// bus (AS5600 0x36, AT24 EEPROM 0x50-0x57, AT24CS02 serial page 0x58-0x5F).
// Fills up to maxOut addresses, returns how many were found.
uint8_t i2cScan(uint8_t *out, uint8_t maxOut) {
  uint8_t n = 0;
  for (uint8_t a = 0x08; a < 0x78 && n < maxOut; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) out[n++] = a;
  }
  return n;
}

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
constexpr uint8_t CMD_PEEL = 0x34;          // payload: [dir(0=fwd,1=rev), duration x10ms (1-255)] -> CMD_ACK after the run / CMD_NACK - v0.01a+, peel motor alone (tape tensioning)
                                            // v0.02: payload [dir] alone runs the calibrated time (CMD_SET_PEEL_TIME); NACK ERR_NOT_READY if uncalibrated
constexpr uint8_t CMD_SET_PEEL_TIME = 0x35; // payload: [msHi,msLo] (10-5000) -> CMD_ACK (echo) / CMD_NACK ERR_BAD_PARAM - v0.02, persisted per feeder
constexpr uint8_t CMD_GET_PEEL_TIME = 0x36; // -> CMD_PEEL_TIME_INFO
constexpr uint8_t CMD_PEEL_TIME_INFO = 0xA4; // payload: [msHi,msLo] (0xFFFF = uncalibrated)
constexpr uint8_t CMD_JOG = 0x37;           // payload: [hi,lo] signed 0.1mm units (|v|<=1600) -> CMD_ACK [angleRawHi,angleRawLo] / CMD_NACK [err] - v0.02
constexpr uint8_t CMD_I2C_SCAN = 0x38;      // -> CMD_I2C_SCAN_INFO - v0.02
constexpr uint8_t CMD_I2C_SCAN_INFO = 0xA5; // payload: responding 7-bit addresses (up to 16)
constexpr uint8_t CMD_SET_SERIAL = 0x39;    // payload: 16 serial bytes -> CMD_ACK / CMD_NACK [ERR_LOCKED|ERR_I2C] - v0.02, plain AT24C02 only

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
      if (payload[0] == 0) { invertMotorA = (payload[1] != 0); targetAngleDeg = readAngleDeg(); } // angle mirrors with it
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
      if (!serialAvailable()) { const uint8_t e = ERR_I2C; sendFrame(CMD_NACK, &e, 1); break; }
      sendFrame(CMD_SERIAL_INFO, factorySerial, FACTORY_SERIAL_LEN);
      break;
    }
    case CMD_PEEL: {
      if (len == 1 && payload[0] <= 1) { // direction only: calibrated time
        if (peelCal.peelMs == PEEL_MS_UNSET) { const uint8_t e = ERR_NOT_READY; sendFrame(CMD_NACK, &e, 1); break; }
        runPeel(payload[0] == 0, peelCal.peelMs);
        sendFrame(CMD_ACK, payload, 1);
        break;
      }
      if (len < 2 || payload[0] > 1 || payload[1] == 0) { sendFrame(CMD_NACK, nullptr, 0); break; }
      runPeel(payload[0] == 0, (unsigned long)payload[1] * 10);
      sendFrame(CMD_ACK, payload, 2);
      break;
    }
    case CMD_SET_PEEL_TIME: {
      const uint16_t ms = (len >= 2) ? (((uint16_t)payload[0] << 8) | payload[1]) : 0;
      if (len < 2 || !setPeelCalMs(ms)) { const uint8_t e = ERR_BAD_PARAM; sendFrame(CMD_NACK, &e, 1); break; }
      sendFrame(CMD_ACK, payload, 2);
      break;
    }
    case CMD_GET_PEEL_TIME: {
      const uint8_t reply[2] = { (uint8_t)(peelCal.peelMs >> 8), (uint8_t)(peelCal.peelMs & 0xFF) };
      sendFrame(CMD_PEEL_TIME_INFO, reply, sizeof(reply));
      break;
    }
    case CMD_JOG: {
      const int16_t tenthsMm = (len >= 2) ? (int16_t)(((uint16_t)payload[0] << 8) | payload[1]) : 0;
      if (len < 2 || tenthsMm == 0 || tenthsMm > 1600 || tenthsMm < -1600) {
        const uint8_t e = ERR_BAD_PARAM; sendFrame(CMD_NACK, &e, 1); break;
      }
      const uint8_t err = moveByMm(tenthsMm / 10.0f, MOVE_TIMEOUT_MS);
      if (err != ERR_NONE) { sendFrame(CMD_NACK, &err, 1); break; }
      const uint16_t raw = angleDegToRaw12(readAngleDeg());
      const uint8_t reply[2] = { (uint8_t)(raw >> 8), (uint8_t)(raw & 0xFF) };
      sendFrame(CMD_ACK, reply, sizeof(reply));
      break;
    }
    case CMD_I2C_SCAN: {
      uint8_t found[16];
      const uint8_t n = i2cScan(found, sizeof(found));
      sendFrame(CMD_I2C_SCAN_INFO, found, n);
      break;
    }
    case CMD_SET_SERIAL: {
      if (len < FACTORY_SERIAL_LEN) { const uint8_t e = ERR_BAD_PARAM; sendFrame(CMD_NACK, &e, 1); break; }
      const uint8_t err = programUserSerial(payload);
      if (err != ERR_NONE) { sendFrame(CMD_NACK, &err, 1); break; }
      sendFrame(CMD_ACK, nullptr, 0);
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

bool traceMoveEnabled = false; // per-150ms move trace - TRACE ON for tuning, off by default so the port stays readable
unsigned long moveSeq = 0;
float lastGoodAngleDeg = 0.0f;
unsigned long i2cErrorCount = 0;
unsigned long lastI2cErrorLogMs = 0;

// Fixed buffer, not String - String pulled in malloc/free/realloc/strtod
// (~2KB flash) plus heap churn on every received byte.
constexpr uint8_t DEBUG_LINE_MAX = 40;
char debugLineBuf[DEBUG_LINE_MAX + 1];
uint8_t debugLineLen = 0;

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
    if (invertMotorA) raw = (4096 - raw) & 0x0FFF; // see invertMotorA - encoder flips with the motor
    lastGoodAngleDeg = (raw * 360.0f) / 4096.0f;
  } else if (millis() - lastI2cErrorLogMs > 500) {
    lastI2cErrorLogMs = millis();
    Serial1.print(F("WARN: AS5600 I2C fail #"));
    Serial1.println(i2cErrorCount);
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
// DRV8833 sleep (nSLEEP) - the driver sleeps whenever neither motor has
// been driven for MOTOR_SLEEP_DELAY_MS. Asleep, its outputs are Hi-Z
// (motors coast, no brake) and it draws ~2uA instead of ~2mA. The delay
// keeps it awake long enough for the end-of-move brake to actually stop
// the wheel before it's let go. driveMotorA()/driveMotorB() wake it on
// demand (tWAKE <= 1ms per datasheet, 2ms used).
// ---------------------------
constexpr unsigned long MOTOR_SLEEP_DELAY_MS = 300;
constexpr unsigned long MOTOR_WAKE_MS = 2;

bool motorsAwake = false;
unsigned long lastMotorDriveMs = 0;

void motorsWake() {
  lastMotorDriveMs = millis();
  if (motorsAwake) return;
  digitalWrite(PIN_nSLEEP, HIGH);
  delay(MOTOR_WAKE_MS);
  motorsAwake = true;
}

// Called from loop(): puts the driver to sleep once both motors are idle.
void motorsSleepIfIdle() {
  if (!motorsAwake || millis() - lastMotorDriveMs < MOTOR_SLEEP_DELAY_MS) return;
  digitalWrite(PIN_nSLEEP, LOW);
  motorsAwake = false;
}

// ---------------------------
// Motor drive
// ---------------------------
// Soft-start: a motor at a dead stop has no back-EMF yet, so a single
// analogWrite() straight to a real move's target duty is close to
// slamming full voltage across the winding resistance - the locked-
// rotor/breakaway current regime, briefly much higher than running
// current. VMOT sits on the 5V rail, so that spike gets drawn from the
// buck's output and reflected back to its 12V input (roughly scaled by
// the step-down ratio) - exactly what an eFuse's current limit sees.
// Boot alone never produces this (nothing draws a locked-rotor-sized
// current at power-up); every move/peel start does, in one hard step.
// softStartDuty() scales a target duty down to a ramp based on how long
// the current drive attempt has been running, reaching the full target
// after MOTOR_SOFTSTART_MS instead of in a single step - spreads the
// same current rise over real time rather than eliminating it (an eFuse
// current LIMIT that's set below the motor's genuine running current
// would still trip regardless of ramp shape; this only tames the
// transient spike at the very start of a move).
constexpr unsigned long MOTOR_SOFTSTART_MS = 100;

int softStartDuty(int targetDuty, unsigned long sinceStartMs) {
  if (sinceStartMs >= MOTOR_SOFTSTART_MS) return targetDuty;
  return (int)((long)targetDuty * (long)sinceStartMs / (long)MOTOR_SOFTSTART_MS);
}

// Tracks whether motor A is currently being driven, so moveToAngle() can
// tell a move starting from a dead stop (ramp it) apart from a
// passThrough hop continuing a fastFeedTurn() sequence the motor's
// already spinning through (don't re-ramp mid-turn - no locked-rotor
// condition exists there, and slowing down between hops would just cost
// speed for no electrical benefit).
bool motorARunning = false;

void brakeMotorA() {
  digitalWrite(PIN_AIN1, HIGH);
  digitalWrite(PIN_AIN2, HIGH);
  motorARunning = false;
}

void driveMotorA(int duty, bool forward) {
  duty = constrain(duty, 0, 255);
  motorsWake();
  motorARunning = true;
  const bool effectiveForward = (MOTOR_A_BASE_INVERT != invertMotorA) ? !forward : forward;
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
  motorsWake();
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

// One-line analog readout shared by IMON/5VSTATUS/I5V/SELFTEST.
void printAnalogReadout() {
  Serial1.print(F("IMON raw=")); Serial1.print(readIMonRaw());
  Serial1.print(F(" ")); Serial1.print(readIMonMilliamps());
  Serial1.print(F("mA | 5V raw=")); Serial1.print(readAdcInternalRef(PIN_5V_READY));
  Serial1.print(F(" ")); Serial1.print(read5vRailMillivolts());
  Serial1.print(F("mV | i5v~")); Serial1.print(estimateI5vMilliamps());
  Serial1.println(F("mA"));
}

void setRelay(bool engaged) {
  digitalWrite(PIN_485_RELAY, engaged == RELAY_ACTIVE_HIGH ? HIGH : LOW);
  relayEngaged = engaged;
}

// Blocking, called once from setup(). Leaves the relay OFF either way if
// it returns false (timed out) - never engages on an unstable/unread rail.
//
// Holds the internal 1.1V reference for the WHOLE wait, rather than
// calling readAdcInternalRef() (which switches INTERNAL<->DEFAULT) on
// every single sample. Toggling the reference mux every 20ms for up to
// RELAY_READY_TIMEOUT_MS risked adding its own settling noise on top of
// whatever the rail is actually doing - a healthy rail could still never
// satisfy RELAY_READY_STABLE_MS if the measurement itself was being
// perturbed every cycle. Nothing else needs an AVCC-referenced read
// during this specific wait (IMON isn't touched here), so there's no
// reason to switch back between samples - only once at the very end.
bool waitFor5vStableAndEngageRelay() {
  analogReference(INTERNAL);
  analogRead(PIN_5V_READY); // discard - reference just changed, needs to settle
  delay(5);

  const unsigned long start = millis();
  uint16_t lastReading = analogRead(PIN_5V_READY);
  unsigned long stableSinceMs = millis();

  while (millis() - start < RELAY_READY_TIMEOUT_MS) {
    delay(20);
    const uint16_t reading = analogRead(PIN_5V_READY);
    if ((uint16_t)abs((int)reading - (int)lastReading) > RELAY_READY_NOISE_BAND) {
      lastReading = reading;
      stableSinceMs = millis(); // moved - restart the stability window
      continue;
    }
    if (millis() - stableSinceMs >= RELAY_READY_STABLE_MS) {
      analogReference(DEFAULT);
      Serial1.print(F("5V ok (adc="));
      Serial1.print(reading);
      Serial1.println(F("), bus relay ON"));
      setRelay(true);
      return true;
    }
  }
  analogReference(DEFAULT);
  Serial1.println(F("WARN: 5V not stable, bus relay stays OFF"));
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

// Release status colors (v0.01a):
//   yellow = booting, not ready yet
//   blue   = ready (booted, magnet detected)
//   red    = error (no magnet, or DRV8833 fault)
//   purple = a motor is moving (feed or peel)
//   white  = IDENTIFY blink
uint32_t shownLedColor = 0xFFFFFFFFUL; // nothing shown yet - forces the first write

// Skips the write when the color is unchanged - loop() repaints every
// iteration, and show() blocks with interrupts off for each call.
void setStatusLedColor(uint8_t r, uint8_t g, uint8_t b) {
  const uint32_t c = statusLed.Color(r, g, b);
  if (c == shownLedColor) return;
  shownLedColor = c;
  statusLed.setPixelColor(0, c);
  statusLed.show();
}

void ledBooting() { setStatusLedColor(255, 255, 0); }
void ledReady()   { setStatusLedColor(0, 0, 255); }
void ledError()   { setStatusLedColor(255, 0, 0); }
void ledMoving()  { setStatusLedColor(160, 0, 255); }
void ledOff()     { setStatusLedColor(0, 0, 0); }

void showStartupLedSequence() {
  statusLed.setBrightness(RGB_MAX_BRIGHTNESS);
  setStatusLedColor(255, 0, 0);
  delay(250);
  setStatusLedColor(0, 255, 0);
  delay(250);
  setStatusLedColor(0, 0, 255);
  delay(250);
  ledOff();
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
    Serial1.println(F("relay ON"));
    setRelay(true);
    setStatusLedColor(0, 255, 0); // green = relay ON
    delay(SELFTEST_RELAY_HOLD_MS);

    Serial1.println(F("relay OFF"));
    setRelay(false);
    setStatusLedColor(255, 0, 0); // red = relay OFF
    delay(SELFTEST_RELAY_HOLD_MS);
  }

  Serial1.println(F("ext LED"));
  setExtLed(true);
  setStatusLedColor(0, 0, 255); // blue = testing ext LED
  delay(300);
  setExtLed(false);

  ledOff();
  // loop() repaints the RGB to the real magnet-detect red/green on its
  // very next iteration - no need to set that here.

  // Calibration readout - not a pass/fail check (no expected value to
  // compare against without a real load/meter attached), just prints
  // what CALI/CALV are currently calibrated to and what they resolve to
  // right now, so a bench tester can sanity-check both at a glance
  // alongside the relay/LED test above. loadAnalogCal() must have run
  // before this (see its call site in setup()) or these divide by zero.
  printAnalogReadout();
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
    ledOff();
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
// Peel motor (motor B, open loop - no encoder) run on its own, e.g. to
// tension the cover tape after loading a reel. Stops early on a DRV8833
// fault. RGB shows purple while running; loop() repaints status after.
// ---------------------------
void runPeel(bool forward, unsigned long ms) {
  ledMoving();
  const unsigned long start = millis();
  // Peel motor has no closed loop and always starts from a dead stop (no
  // passThrough-style continuation like motor A) - always ramp, same
  // reasoning as moveToAngle()'s softStartDuty() use.
  while (millis() - start < ms && digitalRead(PIN_nFAULT) != LOW) {
    driveMotorB(softStartDuty(PEEL_DUTY, millis() - start), forward);
    delay(1);
  }
  brakeMotorB();
}

// SW1: short press = feed one tooth; hold >= SW1_LONG_PRESS_MS = fast
// feed, one full sprocket turn (tape loading). Long press fires as soon as
// the threshold is reached, not on release. Picked over double-press: no
// wait-for-a-second-press delay on every normal single feed.
constexpr unsigned long SW1_LONG_PRESS_MS = 700;
uint8_t fastFeedTurn();

// Buttons are level-checked, not edge-checked, below - "is it down right
// now" rather than "did it just go down." That's fine for an operator
// pressing it after boot, but a button already HELD DOWN through power-up
// (bench instinct: hold it while powering on to see what happens) would
// otherwise satisfy that level check on loop()'s very first pass, with no
// fresh press needed at all. For SW2 especially that fires a full-duty
// driveMotorB() right as the board's least settled after boot - stacking
// on top of the same no-soft-start current spike the ramp above exists
// for. sw1SeenReleased/sw2SeenReleased gate on having observed the
// button in the RELEASED state at least once before any press can ever
// register - a button held from before boot just never arms until it's
// actually let go.
bool sw1SeenReleased = false;
bool sw2SeenReleased = false;

void handleSw1(unsigned long &lastEdgeMs) {
  const int activeLevel = BUTTONS_ACTIVE_LOW ? LOW : HIGH;
  if (digitalRead(PIN_SW1) != activeLevel) { sw1SeenReleased = true; return; }
  if (!sw1SeenReleased) return; // still held from before boot - ignore until released once
  const unsigned long start = millis();
  if (start - lastEdgeMs < DEBOUNCE_MS) return;
  delay(DEBOUNCE_MS);
  if (digitalRead(PIN_SW1) != activeLevel) return; // bounce, not a press

  while (digitalRead(PIN_SW1) == activeLevel && millis() - start < SW1_LONG_PRESS_MS) delay(1);
  if (digitalRead(PIN_SW1) == activeLevel) {
    Serial1.println(F("fast feed"));
    fastFeedTurn();
    while (digitalRead(PIN_SW1) == activeLevel) delay(1); // one turn per hold
  } else {
    targetAngleDeg = normalizeDeg(targetAngleDeg + DEG_PER_TOOTH);
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
  }
  lastEdgeMs = millis();
}

// TODO(button roles): the intended final mapping is SW1 = FEED, SW2 =
// UNFEED (reverse the feed motor, e.g. back tape out / undo an overfeed).
// SW2 is peel-while-held below for now, as a stopgap for tensioning. Still
// to decide before changing it: how peel/tensioning gets triggered once
// SW2 is taken (auto-tension on FEED? a button combo? a long press?), and
// how UNFEED interacts with the peel motor (cover tape slackens when tape
// goes backwards) and with the always-seat-backwards tooth rule.
// SW2: peel runs forward for as long as the button is held (capped at
// PEEL_HOLD_MAX_MS), so the operator can tension the tape by feel.
void peelWhileSw2Held(unsigned long &lastEdgeMs) {
  const int activeLevel = BUTTONS_ACTIVE_LOW ? LOW : HIGH;
  if (digitalRead(PIN_SW2) != activeLevel) { sw2SeenReleased = true; return; }
  if (!sw2SeenReleased) return; // still held from before boot - ignore until released once
  const unsigned long start = millis();
  if (start - lastEdgeMs < DEBOUNCE_MS) return;
  delay(DEBOUNCE_MS);
  if (digitalRead(PIN_SW2) != activeLevel) return; // bounce, not a press

  ledMoving();
  while (digitalRead(PIN_SW2) == activeLevel && digitalRead(PIN_nFAULT) != LOW
         && millis() - start < PEEL_HOLD_MAX_MS) {
    driveMotorB(softStartDuty(PEEL_DUTY, millis() - start), true);
    delay(1);
  }
  brakeMotorB();
  Serial1.print(F("peel ")); Serial1.print(millis() - start); Serial1.println(F("ms"));
  while (digitalRead(PIN_SW2) == activeLevel) delay(1); // cap hit: wait for release, don't restart
  lastEdgeMs = millis();
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
// One result line per move ("move 12 ok 45.00" / "move 12 ERR stall");
// the per-150ms trace only prints with TRACE ON.
uint8_t endMove(unsigned long moveId, uint8_t err, float angle) {
  brakeMotorA();
  lastMoveError = err;
  lastHeartbeatAngleValid = false; // the wheel moved on purpose - don't let the heartbeat flag it
  Serial1.print(F("move ")); Serial1.print(moveId);
  switch (err) {
    case ERR_NONE: Serial1.print(F(" ok ")); Serial1.println(angle, 2); break;
    case ERR_FAULT: Serial1.println(F(" ERR fault")); break;
    case ERR_MAGNET_LOST: Serial1.println(F(" ERR no magnet")); break;
    case ERR_STALL: Serial1.println(F(" ERR stall")); break;
    default: Serial1.println(F(" ERR timeout")); break;
  }
  return err;
}

// passThrough: intermediate hop of a longer move (fastFeedTurn()) - counts
// as reached once within PASS_THROUGH_DEG and returns WITHOUT braking or
// printing, so the next hop continues at full speed. Errors still end the
// move normally.
constexpr float PASS_THROUGH_DEG = 15.0f;

uint8_t moveToAngle(float target, unsigned long timeoutMs, bool passThrough = false) {
  const unsigned long moveId = ++moveSeq;
  const unsigned long start = millis();
  unsigned long lastMoveMs = millis();
  unsigned long lastTraceMs = 0;
  float lastAngle = readAngleDeg();
  // Only ramp if the motor's actually starting from a dead stop here -
  // a passThrough hop continuing a fastFeedTurn() sequence inherits an
  // already-spinning motor (motorARunning stays true across hops, since
  // the passThrough return path below skips brakeMotorA()) and gets the
  // full target duty immediately, same as before this change.
  const bool rampFromStandstill = !motorARunning;

  ledMoving();
  if (traceMoveEnabled) {
    Serial1.print(F("[move ")); Serial1.print(moveId);
    Serial1.print(F("] ")); Serial1.print(lastAngle, 2);
    Serial1.print(F(" -> ")); Serial1.println(target, 2);
  }

  while (true) {
    if (digitalRead(PIN_nFAULT) == LOW) return endMove(moveId, ERR_FAULT, lastAngle);
    if (!magnetDetected()) return endMove(moveId, ERR_MAGNET_LOST, lastAngle);

    const float current = readAngleDeg();
    const float err = angleErrorDeg(target, current);

    if (passThrough && fabsf(err) <= PASS_THROUGH_DEG) return ERR_NONE;
    if (fabsf(err) <= ANGLE_TOLERANCE_DEG) return endMove(moveId, ERR_NONE, current);

    const bool forward = err > 0;
    const int duty = (fabsf(err) > CREEP_THRESHOLD_DEG)
        ? FAST_DUTY
        : (forward ? minMoveDutyFwd : minMoveDutyRev);
    const int appliedDuty = rampFromStandstill ? softStartDuty(duty, millis() - start) : duty;
    driveMotorA(appliedDuty, forward);

    if (fabsf(angleErrorDeg(current, lastAngle)) > STALL_MOVE_THRESHOLD_DEG) {
      lastAngle = current;
      lastMoveMs = millis();
    }

    if (traceMoveEnabled && millis() - lastTraceMs >= 150) {
      lastTraceMs = millis();
      Serial1.print(F("[move ")); Serial1.print(moveId);
      Serial1.print(F("] a=")); Serial1.print(current, 2);
      Serial1.print(F(" e=")); Serial1.print(err, 2);
      Serial1.print(F(" d=")); Serial1.println(forward ? duty : -duty);
    }

    if (millis() - lastMoveMs > STALL_TIMEOUT_MS) return endMove(moveId, ERR_STALL, current);
    if (millis() - start > timeoutMs) return endMove(moveId, ERR_TIMEOUT, current);

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

// Tooth grid: whole teeth counted from the tape zero hole if one is set,
// else from encoder 0.
float toothGridOffsetDeg() {
  return cfg.tapeZeroRaw == TAPE_ZERO_UNSET ? 0.0f : raw12ToAngleDeg(cfg.tapeZeroRaw);
}

// Seat the wheel on the nearest tooth, ALWAYS approaching BACKWARDS (the
// tooth at or behind the current angle, never the one ahead) - moving
// backwards never pushes extra tape forward past the pick point. If
// already within tolerance of a tooth, stays put.
uint8_t snapToToothBackward() {
  const float offset = toothGridOffsetDeg();
  const float rel = normalizeDeg(readAngleDeg() - offset);
  float snapped = floorf(rel / DEG_PER_TOOTH) * DEG_PER_TOOTH;
  if (DEG_PER_TOOTH - (rel - snapped) <= ANGLE_TOLERANCE_DEG) snapped += DEG_PER_TOOTH; // already on the next tooth
  targetAngleDeg = normalizeDeg(offset + snapped);
  return commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
}

// Fast feed: one full sprocket turn forward (TOOTH_COUNT teeth, 160mm) for
// loading tape. A single 360deg target equals the start angle, so it's
// run as four 90deg hops - the first three pass through at full speed,
// only the last one decelerates and settles. Ends on the same tooth it
// started from.
uint8_t fastFeedTurn() {
  const float start = targetAngleDeg;
  for (uint8_t i = 1; i <= 4; i++) {
    targetAngleDeg = normalizeDeg(start + i * 90.0f);
    const uint8_t err = (i < 4) ? moveToAngle(targetAngleDeg, MOVE_TIMEOUT_MS, true)
                                : moveToAngle(targetAngleDeg, MOVE_TIMEOUT_MS);
    if (err != ERR_NONE) { targetAngleDeg = readAngleDeg(); return err; }
  }
  return ERR_NONE;
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
  Serial1.println(F("duty cal..."));
  ledMoving();
  brakeMotorA();
  delay(200);

  if (!magnetDetected()) {
    Serial1.println(F("WARN: no magnet, duty cal skipped"));
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
    Serial1.print(F("duty cal ok fwd=")); Serial1.print(minMoveDutyFwd);
    Serial1.print(F(" rev=")); Serial1.println(minMoveDutyRev);
  } else {
    Serial1.println(F("WARN: duty cal failed, defaults"));
    minMoveDutyFwd = minMoveDutyRev = DEFAULT_MIN_MOVE_DUTY;
    stillDutyMax = DEFAULT_MIN_MOVE_DUTY - CAL_STEP_DUTY;
  }

  moveToAngle(startAngle, CAL_RESTORE_TIMEOUT_MS);
  snapToToothBackward(); // seat on a tooth, approaching backwards
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
  Serial1.println(F("magnet stable, homing"));
  calibrateZero();
}

// ---------------------------
// Debug port (Serial1): status/help + local component config for bench use
// ---------------------------
// Two lines: config/identity, then live hardware state.
void printStatus() {
  Serial1.print(F("addr="));
  if (busAddress == ADDR_UNASSIGNED) Serial1.print('-'); else Serial1.print(busAddress);
  Serial1.print(F(" comp="));
  if (cfg.componentId == COMPONENT_ID_UNSET) Serial1.print('-'); else Serial1.print(cfg.componentId);
  Serial1.print(F(" zero="));
  if (cfg.tapeZeroRaw == TAPE_ZERO_UNSET) Serial1.print('-'); else Serial1.print(cfg.tapeZeroRaw);
  Serial1.print(F(" pitch="));
  if (cfg.feedHalfTeeth == FEED_HALF_TEETH_UNSET) Serial1.print('-'); else Serial1.print(mmForHalfTeeth(cfg.feedHalfTeeth), 1);
  Serial1.print(F("mm width="));
  if (hwInfo.tapeWidthMm == TAPE_WIDTH_UNSET) Serial1.print('-'); else Serial1.print(hwInfo.tapeWidthMm);
  Serial1.print(F("mm inv="));
  Serial1.print(invertMotorA ? 'A' : '-');
  Serial1.print(invertMotorB ? 'B' : '-');
  Serial1.print(F(" serial="));
  Serial1.print(factorySerialValid ? F("factory") : (userSerialValid ? F("user") : F("n/a")));
  Serial1.print(F(" peel="));
  if (peelCal.peelMs == PEEL_MS_UNSET) Serial1.println('-'); else { Serial1.print(peelCal.peelMs); Serial1.println(F("ms")); }

  Serial1.print(F("angle="));
  Serial1.print(readAngleDeg(), 2);
  Serial1.print(F(" target="));
  Serial1.print(targetAngleDeg, 2);
  Serial1.print(' ');
  printMagnetLine(false);
  Serial1.print(F(" relay="));
  Serial1.print(relayEngaged ? F("on") : F("off"));
  Serial1.print(F(" iMon="));
  Serial1.print(readIMonMilliamps());
  Serial1.print(F("mA fault="));
  Serial1.print(digitalRead(PIN_nFAULT) == LOW ? 1 : 0);
  Serial1.print(F(" lastErr="));
  Serial1.print(lastMoveError);
  Serial1.print(F(" i2cErr="));
  Serial1.println(i2cErrorCount);
}

void printHelp() {
  Serial1.println(F(
    "Buttons: SW1=feed 1 tooth, SW1 hold=fast feed 1 turn, SW2 hold=peel\n"
    "Move : STEP <teeth> | T<n> | A<deg> | MOVEMM <mm> | FEED | FASTFEED | SNAP | GOTOZERO | GOMM <mm> | STOP\n"
    "Peel : PEEL <ms>  (negative = reverse, max 5000) | PEELCAL [<ms>] | PEELRUN [REV]\n"
    "Tape : ZEROHERE | PITCH <mm> | COMPONENT <id> | FEEDCFG <raw> <halfTeeth> | RESETCFG\n"
    "Setup: ZERO | INVERTA ON|OFF | INVERTB ON|OFF | SETWIDTH <mm> | SIMADDR <n>\n"
    "Power: IMON | 5VSTATUS | I5V | RELAY ON|OFF | CALI <mA> | CALV <V> | CALSTATUS | CALRESET\n"
    "Info : STATUS | SERIAL | SETSERIAL <32 hex> | I2CSCAN | SELFTEST | IDENTIFY [n] | LED ON|OFF | TRACE ON|OFF | HELP"));
}

// Minimal number parser (sign, digits, optional fraction) - atof()/strtod
// alone would cost ~700 bytes of flash.
float parseNum(const char *p) {
  while (*p == ' ') p++;
  bool neg = false;
  if (*p == '-' || *p == '+') neg = (*p++ == '-');
  float v = 0.0f;
  while (*p >= '0' && *p <= '9') v = v * 10.0f + (*p++ - '0');
  if (*p == '.') {
    p++;
    float scale = 0.1f;
    while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * scale; scale *= 0.1f; }
  }
  return neg ? -v : v;
}

long parseInt(const char *p) { return lround(parseNum(p)); }

// Returns the text after `prefix` if `line` starts with it, else nullptr.
const char *afterPrefix(const char *line, PGM_P prefix) {
  const size_t n = strlen_P(prefix);
  return strncmp_P(line, prefix, n) == 0 ? line + n : nullptr;
}

#define CMD_IS(s) (strcmp_P(line, PSTR(s)) == 0)
#define CMD_ARG(s) afterPrefix(line, PSTR(s))

void printOk() { Serial1.println(F("ok")); }

// `line` is already trimmed and upper-cased by the caller.
void handleDebugLine(const char *line) {
  const char *arg = nullptr;

  if (CMD_IS("STATUS") || CMD_IS("WHOAMI")) { printStatus(); return; }
  if (CMD_IS("HELP") || CMD_IS("?")) { printHelp(); return; }
  if (CMD_IS("ZERO")) { calibrateZero(); return; }
  if (CMD_IS("STOP")) { brakeMotorA(); brakeMotorB(); printOk(); return; }
  if (CMD_IS("LED ON")) { setExtLed(true); printOk(); return; }
  if (CMD_IS("LED OFF")) { setExtLed(false); printOk(); return; }
  if (CMD_IS("RELAY ON")) { setRelay(true); printOk(); return; }
  if (CMD_IS("RELAY OFF")) { setRelay(false); printOk(); return; }
  if (CMD_IS("TRACE ON")) { traceMoveEnabled = true; printOk(); return; }
  if (CMD_IS("TRACE OFF")) { traceMoveEnabled = false; printOk(); return; }
  if (CMD_IS("INVERTA ON")) { invertMotorA = true; targetAngleDeg = readAngleDeg(); printOk(); return; }
  if (CMD_IS("INVERTA OFF")) { invertMotorA = false; targetAngleDeg = readAngleDeg(); printOk(); return; }
  if (CMD_IS("SNAP")) { snapToToothBackward(); return; }
  if (CMD_IS("FASTFEED")) { fastFeedTurn(); return; }
  if (CMD_IS("INVERTB ON")) { invertMotorB = true; printOk(); return; }
  if (CMD_IS("INVERTB OFF")) { invertMotorB = false; printOk(); return; }
  if (CMD_IS("SELFTEST")) {
    runDebugSelfTest(); // ends with the relay OFF - RELAY ON or reboot to rejoin the bus
    Serial1.println(F("selftest done, relay OFF"));
    return;
  }
  if (CMD_IS("IMON") || CMD_IS("5VSTATUS") || CMD_IS("I5V")) { printAnalogReadout(); return; }
  if ((arg = CMD_ARG("CALI "))) {
    const float ma = parseNum(arg);
    const uint16_t raw = readIMonRaw();
    if (ma <= 0 || raw == 0) { Serial1.println(F("ERR: need mA>0 and IMON raw>0")); return; }
    analogCal.imonCalRaw = raw;
    analogCal.imonCalMa = (uint16_t)(ma + 0.5f);
    saveAnalogCal();
    printOk();
    return;
  }
  if ((arg = CMD_ARG("CALV "))) {
    const float v = parseNum(arg);
    const uint16_t raw = readAdcInternalRef(PIN_5V_READY);
    if (v <= 0 || raw == 0) { Serial1.println(F("ERR: need V>0 and 5V raw>0")); return; }
    analogCal.v5vCalRaw = raw;
    analogCal.v5vCalMv = (uint16_t)(v * 1000.0f + 0.5f);
    saveAnalogCal();
    printOk();
    return;
  }
  if (CMD_IS("CALSTATUS")) {
    Serial1.print(F("cal IMON ")); Serial1.print(analogCal.imonCalRaw);
    Serial1.print('='); Serial1.print(analogCal.imonCalMa);
    Serial1.print(F("mA, 5V ")); Serial1.print(analogCal.v5vCalRaw);
    Serial1.print('='); Serial1.print(analogCal.v5vCalMv); Serial1.println(F("mV"));
    return;
  }
  if (CMD_IS("CALRESET")) { resetAnalogCalToFactoryDefaults(); saveAnalogCal(); printOk(); return; }
  if (CMD_IS("IDENTIFY") || (arg = CMD_ARG("IDENTIFY "))) {
    const long n = arg ? parseInt(arg) : 0;
    identifyBlink(n > 0 && n < 256 ? (uint8_t)n : 3);
    return;
  }
  if ((arg = CMD_ARG("SETWIDTH "))) {
    if (setTapeWidthMm((uint8_t)parseInt(arg))) printOk();
    else Serial1.println(F("ERR: 8/12/16/24/32/44/56 only, or AT24CS02 absent"));
    return;
  }
  if (CMD_IS("SERIAL")) {
    if (!serialAvailable()) { Serial1.println(F("serial n/a")); return; }
    for (uint8_t i = 0; i < FACTORY_SERIAL_LEN; i++) {
      if (factorySerial[i] < 0x10) Serial1.print('0');
      Serial1.print(factorySerial[i], HEX);
    }
    Serial1.println();
    return;
  }
  if ((arg = CMD_ARG("SIMADDR "))) {
    const long n = parseInt(arg);
    if (n < ADDR_MIN || n > ADDR_MAX) { Serial1.println(F("ERR: 1-247")); return; }
    busAddress = (uint8_t)n;
    printOk();
    return;
  }
  if ((arg = CMD_ARG("COMPONENT "))) {
    const long id = parseInt(arg);
    if (id < 0 || id > 65534) { Serial1.println(F("ERR: 0-65534")); return; }
    setComponentId((uint16_t)id);
    printOk();
    return;
  }
  if ((arg = CMD_ARG("FEEDCFG "))) {
    const char *sep = strchr(arg, ' ');
    const long zero = parseInt(arg);
    const long half = sep ? parseInt(sep) : 0;
    if (zero < 0 || zero > 4095 || half < 1 || half > 255) { Serial1.println(F("ERR: FEEDCFG <0-4095> <1-255>")); return; }
    setFeedConfig((uint16_t)zero, (uint8_t)half);
    printOk();
    return;
  }
  if (CMD_IS("RESETCFG")) { manualResetFeedConfig(); printOk(); return; }
  if (CMD_IS("ZEROHERE")) {
    setTapeZeroHere();
    Serial1.print(F("zero=")); Serial1.println(cfg.tapeZeroRaw);
    return;
  }
  if ((arg = CMD_ARG("PITCH "))) {
    const float mm = parseNum(arg);
    if (mm <= 0) { Serial1.println(F("ERR: mm>0")); return; }
    setFeedPitchMm(mm);
    Serial1.print(F("halfTeeth=")); Serial1.println(cfg.feedHalfTeeth);
    return;
  }
  if ((arg = CMD_ARG("PEEL "))) {
    const long ms = parseInt(arg);
    const unsigned long dur = (unsigned long)labs(ms);
    if (dur == 0 || dur > PEEL_CMD_MAX_MS) { Serial1.println(F("ERR: PEEL <1-5000> (neg=rev)")); return; }
    runPeel(ms > 0, dur);
    printOk();
    return;
  }
  if (CMD_IS("PEELCAL")) {
    if (peelCal.peelMs == PEEL_MS_UNSET) Serial1.println(F("peelcal unset - PEELCAL <ms>"));
    else { Serial1.print(F("peelcal ")); Serial1.print(peelCal.peelMs); Serial1.println(F("ms")); }
    return;
  }
  if ((arg = CMD_ARG("PEELCAL "))) {
    const long ms = parseInt(arg);
    if (ms < 0 || !setPeelCalMs((uint16_t)ms)) { Serial1.println(F("ERR: PEELCAL <10-5000>")); return; }
    printOk();
    return;
  }
  if (CMD_IS("PEELRUN") || CMD_IS("PEELRUN REV")) {
    if (peelCal.peelMs == PEEL_MS_UNSET) { Serial1.println(F("ERR: no peelcal, PEELCAL <ms> first")); return; }
    runPeel(!CMD_IS("PEELRUN REV"), peelCal.peelMs);
    printOk();
    return;
  }
  if (CMD_IS("I2CSCAN")) {
    uint8_t found[16];
    const uint8_t n = i2cScan(found, sizeof(found));
    Serial1.print(F("i2c:"));
    for (uint8_t i = 0; i < n; i++) { Serial1.print(F(" 0x")); Serial1.print(found[i], HEX); }
    Serial1.println(n ? F("") : F(" none"));
    return;
  }
  if ((arg = CMD_ARG("SETSERIAL "))) {
    // 32 hex digits (16 bytes); spaces ignored. The line was uppercased on input.
    uint8_t ser[FACTORY_SERIAL_LEN];
    uint8_t digits = 0;
    bool ok = true;
    for (const char *p = arg; *p && ok; p++) {
      if (*p == ' ') continue;
      uint8_t v;
      if (*p >= '0' && *p <= '9') v = *p - '0';
      else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
      else { ok = false; break; }
      if (digits >= 2 * FACTORY_SERIAL_LEN) { ok = false; break; }
      if (digits % 2 == 0) ser[digits / 2] = v << 4; else ser[digits / 2] |= v;
      digits++;
    }
    if (!ok || digits != 2 * FACTORY_SERIAL_LEN) { Serial1.println(F("ERR: SETSERIAL needs 32 hex digits")); return; }
    const uint8_t err = programUserSerial(ser);
    if (err == ERR_LOCKED) Serial1.println(F("ERR: factory serial present, can't override"));
    else if (err != ERR_NONE) Serial1.println(F("ERR: EEPROM write/verify failed - I2CSCAN to check the chip"));
    else printOk();
    return;
  }
  if ((arg = CMD_ARG("JOG "))) {
    const float mm = parseNum(arg);
    if (mm == 0.0f || mm > 160.0f || mm < -160.0f) { Serial1.println(F("ERR: JOG <+-0.1..160 mm>")); return; }
    moveByMm(mm, MOVE_TIMEOUT_MS);
    return;
  }
  if (CMD_IS("GOTOZERO")) {
    if (cfg.tapeZeroRaw == TAPE_ZERO_UNSET) { Serial1.println(F("ERR: no zero, ZEROHERE first")); return; }
    moveToTapeZeroPlusMm(0.0f, MOVE_TIMEOUT_MS);
    return;
  }
  if ((arg = CMD_ARG("GOMM "))) {
    if (cfg.tapeZeroRaw == TAPE_ZERO_UNSET) { Serial1.println(F("ERR: no zero, ZEROHERE first")); return; }
    moveToTapeZeroPlusMm(parseNum(arg), MOVE_TIMEOUT_MS);
    return;
  }
  if ((arg = CMD_ARG("MOVEMM "))) { moveByMm(parseNum(arg), MOVE_TIMEOUT_MS); return; }
  if (CMD_IS("FEED")) {
    if (feedOnePitch(MOVE_TIMEOUT_MS) == ERR_NOT_READY) Serial1.println(F("ERR: no pitch, PITCH first"));
    return;
  }
  if ((arg = CMD_ARG("STEP"))) {
    targetAngleDeg = normalizeDeg(targetAngleDeg + parseNum(arg) * DEG_PER_TOOTH);
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
    return;
  }
  if ((arg = CMD_ARG("T"))) {
    long idx = parseInt(arg);
    idx = ((idx % TOOTH_COUNT) + TOOTH_COUNT) % TOOTH_COUNT;
    targetAngleDeg = idx * DEG_PER_TOOTH;
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
    return;
  }
  if ((arg = CMD_ARG("A"))) {
    targetAngleDeg = normalizeDeg(parseNum(arg));
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
    return;
  }
  Serial1.println(F("? (HELP)"));
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

  // showStartupLedSequence() (red/green/blue splash) disabled for
  // v0.01a - same reasoning as the test aids above: an arbitrary color
  // pattern that doesn't reflect real status isn't "test scaffolding"
  // in the same sense as SELFTEST/the relay button-test, but it's the
  // same category of "boot behavior that doesn't mean anything" this
  // release is meant to drop. Solid yellow instead - "booting, not
  // ready yet" - until loop() overwrites it with the real status
  // (blue ready / red error) on its very first iteration.
  // showStartupLedSequence();
  statusLed.setBrightness(RGB_MAX_BRIGHTNESS);
  ledBooting();

  brakeMotorA();
  lockMotorBOff();
  digitalWrite(PIN_nSLEEP, LOW); // DRV8833 starts asleep - driveMotorA()/B() wake it on demand

  Wire.begin();
  Wire.setClock(I2C_CLOCK_HZ);

  rs485Init();
  loadPeelCal();
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

  targetAngleDeg = readAngleDeg();
  // Homing (calibrateZero()) is intentionally NOT called here - see
  // checkHoming() in loop(): a boot-settle/stagger delay plus a
  // magnet-placement stability check both have to pass first, and neither
  // should block the debug port or RS485 bus from responding meanwhile.

  Serial1.println(F("v0.02 ready - HELP for commands"));
  printStatus();
  if (!magnetDetected()) Serial1.println(F("WARN: no magnet - homing deferred"));
}

void loop() {
  const unsigned long now = millis();
  const bool magnet = magnetDetected();

  // Status RGB: blue = ready, red = error (no magnet / driver fault).
  // Purple (moving) is set inside the move/peel functions themselves and
  // gets repainted here on the next iteration once they return.
  if (magnet && digitalRead(PIN_nFAULT) != LOW) ledReady();
  else ledError();

  if (now - lastHeartbeatMs >= 3000) {
    lastHeartbeatMs = now;
    // Without a magnet the AS5600 angle is noise - skip the check and drop
    // the baseline so the next real reading starts a fresh comparison.
    if (magnet) {
      const float angleNow = readAngleDeg();
      if (lastHeartbeatAngleValid && fabsf(angleErrorDeg(angleNow, lastHeartbeatAngle)) > 1.0f) {
        Serial1.println(F("WARN: wheel moved while idle"));
      }
      lastHeartbeatAngle = angleNow;
      lastHeartbeatAngleValid = true;
    } else {
      lastHeartbeatAngleValid = false;
    }
  }

  if (digitalRead(PIN_nFAULT) == LOW) {
    brakeMotorA();
    brakeMotorB();
    digitalWrite(PIN_FAULT_LED, HIGH);
    if (now - lastFaultLogMs >= 2000) {
      lastFaultLogMs = now;
      Serial1.println(F("FAULT: DRV8833 nFAULT, motors braked"));
    }
    return;
  }
  digitalWrite(PIN_FAULT_LED, LOW);

  checkHoming(); // no-op once homingDone; see its own comment for the boot-delay/stagger + magnet-stability gate

  handleSw1(lastSw1EdgeMs);
  peelWhileSw2Held(lastSw2EdgeMs);
  motorsSleepIfIdle();

  rs485Poll();

  while (Serial1.available()) {
    const char c = toupper(Serial1.read());
    if (c == '\n' || c == '\r') {
      while (debugLineLen > 0 && debugLineBuf[debugLineLen - 1] == ' ') debugLineLen--;
      if (debugLineLen > 0) {
        debugLineBuf[debugLineLen] = '\0';
        handleDebugLine(debugLineBuf);
        debugLineLen = 0;
      }
    } else if ((c != ' ' || debugLineLen > 0) && debugLineLen < DEBUG_LINE_MAX) {
      debugLineBuf[debugLineLen++] = c; // leading spaces dropped, overlong lines truncated
    }
  }
}
