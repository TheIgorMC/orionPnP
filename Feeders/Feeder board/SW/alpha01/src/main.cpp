#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <math.h>
#include "pins_config.h"

/*
  alpha01 - first feeder firmware targeting the real board (V0.2a/V0.2b)

  Supersedes TestBench04. Same AS5600 + DRV8833 closed-loop wheel-position
  control (logic unchanged, only pins moved to pins_config.h to match the
  real schematic), plus two things TestBench04 didn't have:

  1. RS485 transport on USART0 (D0/D1), with RE/DE direction control on D2
     (PIN_RS485_RE), instead of USART0 doubling as an ad-hoc debug port.
     All human-readable output moved to Serial1 (USART1, D11/D12 - shared
     with the ISP programming header) so debug prints never leak onto the
     live bus.

  2. A minimal, deliberately NOT-Modbus framed protocol on top of that
     transport, just enough to validate byte-level transport (RE/DE
     switching timing, addressing, CRC) on real silicon before committing
     to Modbus RTU or continuing OrionProtocol. See project.md for the
     Modbus feasibility evaluation and the addressing scheme this
     implements (persistent EEPROM address + a reserved "parking" address
     for never-assigned feeders).

  What alpha01 deliberately does NOT do yet:
  - No interrupt-driven UART RX. Bytes are polled in loop() same as
    TestBench04. Since moveToAngle() blocks synchronously for up to
    MOVE_TIMEOUT_MS, and USART0 only has a 2-byte hardware buffer, any bus
    traffic arriving mid-move today WILL be lost. This is fine for
    bench-testing transport/addressing in isolation, but must be fixed
    (RX ISR + ring buffer) before real bus traffic coexists with motion -
    see project.md "Open questions".
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
// ---------------------------
const int TOOTH_COUNT = 40;
const float DEG_PER_TOOTH = 360.0f / TOOTH_COUNT; // 9.0 deg

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

const bool BUTTONS_ACTIVE_LOW = true;
const unsigned long DEBOUNCE_MS = 20;
const bool INVERT_DIRECTION = false;

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
constexpr uint8_t CMD_DISCOVER_HERE = 0x90; // payload: [nonceHi,nonceLo,componentIdHi,componentIdLo]
constexpr uint8_t CMD_ASSIGN_ADDR = 0x11;   // broadcast, payload: [nonceHi,nonceLo,newAddr]
                                             // only the matching nonce adopts newAddr

constexpr uint8_t CMD_GET_COMPONENT = 0x20; // -> CMD_COMPONENT_INFO
constexpr uint8_t CMD_COMPONENT_INFO = 0xA0; // payload: [idHi,idLo,zeroHi,zeroLo,feedHalfTeeth]
constexpr uint8_t CMD_SET_COMPONENT = 0x21;  // payload: [idHi,idLo] -> CMD_ACK
constexpr uint8_t CMD_SET_FEED_CONFIG = 0x22; // payload: [zeroHi,zeroLo,feedHalfTeeth] -> CMD_ACK
constexpr uint8_t CMD_RESET_CONFIG = 0x23;    // no payload -> CMD_ACK

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
      uint8_t reply[4] = {
        (uint8_t)(sessionNonce >> 8), (uint8_t)(sessionNonce & 0xFF),
        (uint8_t)(cfg.componentId >> 8), (uint8_t)(cfg.componentId & 0xFF)
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
// Motor drive
// ---------------------------
void brakeMotorA() {
  digitalWrite(PIN_AIN1, HIGH);
  digitalWrite(PIN_AIN2, HIGH);
}

void driveMotorA(int duty, bool forward) {
  duty = constrain(duty, 0, 255);
  const bool effectiveForward = INVERT_DIRECTION ? !forward : forward;
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
bool moveToAngle(float target, unsigned long timeoutMs) {
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
      return false;
    }
    if (!magnetDetected()) {
      brakeMotorA();
      Serial1.print(F("[move ")); Serial1.print(moveId);
      Serial1.println(F("] ERROR: magnet lost during move (MD=0), move aborted"));
      return false;
    }

    const float current = readAngleDeg();
    const float err = angleErrorDeg(target, current);

    if (fabsf(err) <= ANGLE_TOLERANCE_DEG) {
      brakeMotorA();
      Serial1.print(F("[move ")); Serial1.print(moveId);
      Serial1.print(F("] Reached target: ")); Serial1.println(current, 2);
      return true;
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
      return false;
    }
    if (millis() - start > timeoutMs) {
      brakeMotorA();
      Serial1.print(F("[move ")); Serial1.print(moveId);
      Serial1.println(F("] ERROR: move timeout, aborted"));
      return false;
    }

    delay(5);
  }
}

bool commandMoveTo(float target, unsigned long timeoutMs) {
  const bool ok = moveToAngle(target, timeoutMs);
  if (!ok) targetAngleDeg = readAngleDeg();
  return ok;
}

// ---------------------------
// Zero-point calibration (unchanged from TestBench04)
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
  Serial1.print(F(" feedHalfTeeth="));
  if (cfg.feedHalfTeeth == FEED_HALF_TEETH_UNSET) Serial1.print(F("UNSET"));
  else Serial1.print(cfg.feedHalfTeeth);
  Serial1.print(F(" angle="));
  Serial1.print(readAngleDeg(), 2);
  Serial1.print(F(" target="));
  Serial1.print(targetAngleDeg, 2);
  Serial1.print(F(" | "));
  printMagnetLine(false);
  Serial1.print(F(" | i2cErrors="));
  Serial1.println(i2cErrorCount);
}

void printHelp() {
  Serial1.println(F("Debug port commands (newline terminated):"));
  Serial1.println(F("  A<deg> / T<index> / STEP+1 / STEP-1 / STEP+0.5 / STEP-0.5"));
  Serial1.println(F("  ZERO / STOP / STATUS / TRACE ON / TRACE OFF / HELP"));
  Serial1.println(F("  WHOAMI          print bus address + loaded component config"));
  Serial1.println(F("  SIMADDR <n>     force bus address n locally (1-247), bench-only,"));
  Serial1.println(F("                  bypasses CMD_DISCOVER/CMD_ASSIGN_ADDR - for testing"));
  Serial1.println(F("                  motion/config commands without a host on the bus yet"));
  Serial1.println(F("  COMPONENT <id>  set loaded component id (mirrors CMD_SET_COMPONENT);"));
  Serial1.println(F("                  resets feed config if id actually changed"));
  Serial1.println(F("  FEEDCFG <zeroRaw> <halfTeeth>   set tape zero + step size, persisted"));
  Serial1.println(F("  RESETCFG        clear tape zero + step size only (keeps component id)"));
  Serial1.println(F("  FEED            advance by the configured feedHalfTeeth (next pocket)"));
}

void handleDebugLine(String line) {
  line.trim();
  if (line.length() == 0) return;
  String upper = line; upper.toUpperCase();

  if (upper == "STATUS") { printStatus(); return; }
  if (upper == "ZERO") { calibrateZero(); return; }
  if (upper == "STOP") { brakeMotorA(); Serial1.println(F("Stopped.")); return; }
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
  if (upper == "FEED") {
    if (cfg.feedHalfTeeth == FEED_HALF_TEETH_UNSET) {
      Serial1.println(F("Refused: feedHalfTeeth not configured, run FEEDCFG first."));
    } else {
      targetAngleDeg = normalizeDeg(targetAngleDeg + cfg.feedHalfTeeth * (DEG_PER_TOOTH / 2.0f));
      commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
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

  brakeMotorA();
  lockMotorBOff();
  digitalWrite(PIN_nSLEEP, HIGH); // wake DRV8833

  Wire.begin();
  Wire.setClock(I2C_CLOCK_HZ);

  Serial1.begin(DEBUG_BAUD);
  rs485Init();

  seedSessionNonce();
  loadConfig(); // busAddress always starts ADDR_UNASSIGNED - re-earned via CMD_DISCOVER each boot

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
    targetAngleDeg = normalizeDeg(targetAngleDeg - DEG_PER_TOOTH);
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
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
