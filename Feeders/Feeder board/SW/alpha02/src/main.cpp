#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <math.h>
#include <Adafruit_NeoPixel.h>
#include "pins_config.h"

/*
  alpha02 - closed-loop feedback restored for real hardware bring-up with a
  magnet mounted, plus runtime-selectable motor direction.

  Forked from alpha01 after yesterday's bring-up commit (73e7448) had
  temporarily rewired SW1/SW2 to jog motor A/B open-loop, because
  moveToAngle() refuses to run without magnetDetected() and no magnet was
  mounted yet on the bare test board. Now that a magnet is mounted:

  - SW1 goes back to what it was before that bring-up detour: a real
    closed-loop +1 tooth jog via commandMoveTo()/moveToAngle(), so the
    AS5600 feedback loop can actually be exercised and validated end to
    end (stall/timeout/fault handling included, none of it bypassed).
  - SW2 stays an open-loop motor B jog. This isn't a shortcut - motor B
    (peel) has no encoder on this design at all, closed-loop control
    doesn't apply to it the way it does to the sprocket motor, so there's
    nothing to "restore" there.
  - Motor direction (for both A and B independently) is now a RUNTIME flag
    instead of the old compile-time INVERT_DIRECTION constant, since it's
    not yet known whether either motor's leads are wired the way the
    firmware assumes - see invertMotorA/invertMotorB, INVERTA/INVERTB
    debug commands, and CMD_SET_INVERT_DIR below. Flipping a flag beats
    reflashing or re-soldering leads while that's still being figured out
    on the bench.

  Everything else (RS485 transport, addressing/discovery, tape-zero/
  distance-based motion, EEPROM component config) is unchanged from
  alpha01 - see that project's project.md for the full design rationale.
  alpha01 itself keeps evolving in parallel as a dedicated RS485 transport
  test rig (see its own project.md, "RS485 echo test mode").

  What alpha02 deliberately does NOT do yet (inherited from alpha01):
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
// INVERT_DIRECTION constant). Neither motor's lead polarity relative to
// this firmware's "forward" has been confirmed on real hardware yet -
// these let it be corrected from the debug port or the bus (INVERTA/
// INVERTB, CMD_SET_INVERT_DIR) without reflashing or re-soldering.
// Independent per motor since there's no reason A and B would necessarily
// need the same correction. RAM-only for now (reset to false on reboot) -
// see project.md if this turns out to be a fixed-per-unit characteristic
// worth persisting to EEPROM instead.
bool invertMotorA = false;
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

// Seeds from whatever entropy an unloaded AVR has handy: a floating ADC
// pin plus boot-to-boot jitter in micros(). Not cryptographically unique -
// doesn't need to be. It only has to avoid colliding with whichever other
// feeders happen to be replying to the same CMD_DISCOVER round, and a
// fresh value is drawn every boot anyway.
void seedSessionNonce() {
  randomSeed(analogRead(A2) ^ micros());
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
// Hardware identity (EEPROM) - fixed at manufacturing/assembly time, NOT
// per-component data. Deliberately a separate struct/EEPROM slot from
// FeederConfig above: tape width is a property of this physical feeder's
// mechanical build (its tape guide/rail width), identical for every reel
// ever loaded into it, so it must never be touched by setComponentId()'s
// reset-on-change or by RESETCFG/CMD_RESET_CONFIG - those are scoped to
// "what's currently loaded," not "what this unit physically is."
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

constexpr int EEPROM_HWINFO_LOCATION = 16; // separate from EEPROM_CONFIG_LOCATION, room to grow both
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
  EEPROM.get(EEPROM_HWINFO_LOCATION, hwInfo);
  if (hwInfo.crc != hwInfoCrc(hwInfo)) {
    hwInfo.tapeWidthMm = TAPE_WIDTH_UNSET; // factory-fresh board, not yet set at assembly
    hwInfo.crc = hwInfoCrc(hwInfo);
    EEPROM.put(EEPROM_HWINFO_LOCATION, hwInfo);
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
  EEPROM.put(EEPROM_HWINFO_LOCATION, hwInfo);
  return true;
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
constexpr uint8_t CMD_SET_EXT_LED = 0x27;     // payload: [state] (0=off, nonzero=on) -> CMD_ACK/CMD_NACK
constexpr uint8_t CMD_SET_INVERT_DIR = 0x28;  // payload: [motor(0=A,1=B), state(0/1)] -> CMD_ACK/CMD_NACK

// Hardware identity (tape width) - see FeederHardwareInfo above.
constexpr uint8_t CMD_GET_HW_INFO = 0x29;   // -> CMD_HW_INFO
constexpr uint8_t CMD_HW_INFO = 0xA1;       // payload: [tapeWidthMm] (0xFF = unset)
constexpr uint8_t CMD_SET_HW_INFO = 0x2A;   // payload: [tapeWidthMm] -> CMD_ACK/CMD_NACK - assembly/bench-time only, no reset command by design

// Live telemetry/control for real operation (not just bench transport
// validation) - added once alpha02 started heading for an actual PnP
// instead of just the bench.
constexpr uint8_t CMD_GET_STATUS = 0x30;    // -> CMD_STATUS_INFO
constexpr uint8_t CMD_STATUS_INFO = 0xA2;   // payload: [angleRawHi,angleRawLo,as5600Status,faultActive,lastMoveErr]
constexpr uint8_t CMD_STOP = 0x31;          // no payload: immediate brake, both motors -> CMD_ACK
constexpr uint8_t CMD_IDENTIFY = 0x32;      // payload: [blinkCount] (0 => default) -> CMD_ACK after blinking

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
      const uint8_t reply[5] = {
        (uint8_t)(angleRaw >> 8), (uint8_t)(angleRaw & 0xFF),
        readStatus(),
        (uint8_t)(digitalRead(PIN_nFAULT) == LOW ? 1 : 0),
        lastMoveError
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
// External LED (PIN_EXT_LED, PB5/D13) - simple on/off indicator, no PWM.
// ---------------------------
void setExtLed(bool on) {
  digitalWrite(PIN_EXT_LED, on ? HIGH : LOW);
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
  Serial1.println(F("  LED ON / LED OFF  external LED (PB5/D13) on/off"));
  Serial1.println(F("  INVERTA ON/OFF  flip motor A direction (mirrors CMD_SET_INVERT_DIR)"));
  Serial1.println(F("  INVERTB ON/OFF  flip motor B direction (mirrors CMD_SET_INVERT_DIR)"));
  Serial1.println(F("  IDENTIFY [n]    blink status LED white n times (default 3), mirrors"));
  Serial1.println(F("                  CMD_IDENTIFY - find which physical unit an address is"));
  Serial1.println(F("  SETWIDTH <mm>   set this unit's tape width (8/12/16/24/32/44/56),"));
  Serial1.println(F("                  assembly/bench-time only - not reset by anything else,"));
  Serial1.println(F("                  mirrors CMD_SET_HW_INFO"));
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
      Serial1.println(F("Refused: width must be one of 8/12/16/24/32/44/56."));
    } else {
      Serial1.print(F("Tape width set: ")); Serial1.print(mm); Serial1.println(F("mm"));
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
  pinMode(PIN_SW1, INPUT_PULLUP);
  pinMode(PIN_SW2, INPUT_PULLUP);
  pinMode(PIN_AIN1, OUTPUT);
  pinMode(PIN_AIN2, OUTPUT);
  pinMode(PIN_nSLEEP, OUTPUT);
  pinMode(PIN_nFAULT, INPUT_PULLUP);
  pinMode(PIN_FAULT_LED, OUTPUT);
  pinMode(PIN_EXT_LED, OUTPUT);
  pinMode(PIN_RGB_DATA, OUTPUT);
  setExtLed(false);

  statusLed.begin();
  statusLed.clear();
  statusLed.show();
  showStartupLedSequence();

  brakeMotorA();
  lockMotorBOff();
  digitalWrite(PIN_nSLEEP, HIGH); // wake DRV8833

  Wire.begin();
  Wire.setClock(I2C_CLOCK_HZ);

  Serial1.begin(DEBUG_BAUD);
  rs485Init();

  seedSessionNonce();
  loadConfig(); // busAddress always starts ADDR_UNASSIGNED - re-earned via CMD_DISCOVER each boot
  loadHwInfo(); // tape width etc - set once at assembly, never reset by config changes

  Serial1.println(F("alpha01 feeder firmware ready"));
  printStatus();

  if (!magnetDetected()) {
    Serial1.println(F("WARN: AS5600 magnet not detected at startup."));
  }

  targetAngleDeg = readAngleDeg();
  calibrateZero();
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
    const float angleNow = readAngleDeg();
    if (lastHeartbeatAngleValid && fabsf(angleErrorDeg(angleNow, lastHeartbeatAngle)) > 1.0f) {
      Serial1.println(F("WARN: wheel moved between heartbeats with no move in progress"));
    }
    lastHeartbeatAngle = angleNow;
    lastHeartbeatAngleValid = true;
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
