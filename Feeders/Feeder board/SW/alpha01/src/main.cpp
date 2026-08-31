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
// Addressing (see project.md "Addressing scheme")
// ---------------------------
constexpr int EEPROM_ADDR_LOCATION = 0;
constexpr uint8_t ADDR_UNSET = 0xFF;     // erased-EEPROM sentinel, never a valid address
constexpr uint8_t PARKING_ADDRESS = 0xF8; // 248 - outside 1-247, used only pre-assignment
constexpr uint8_t ADDR_MIN = 1;
constexpr uint8_t ADDR_MAX = 247;

uint8_t feederAddress = PARKING_ADDRESS;

bool isValidAssignedAddress(uint8_t a) {
  return a >= ADDR_MIN && a <= ADDR_MAX;
}

void loadAddressFromEeprom() {
  const uint8_t stored = EEPROM.read(EEPROM_ADDR_LOCATION);
  feederAddress = isValidAssignedAddress(stored) ? stored : PARKING_ADDRESS;
}

// Only meaningful while this feeder is alone on a programming bus (see
// project.md) - nothing here enforces that; it's a physical/topological
// guarantee from the host side, not something firmware can verify.
bool assignAddress(uint8_t newAddr) {
  if (!isValidAssignedAddress(newAddr)) return false;
  EEPROM.update(EEPROM_ADDR_LOCATION, newAddr);
  feederAddress = newAddr;
  return true;
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
// ADDR: 0x00 broadcast, 1-247 unicast, PARKING_ADDRESS = only-unassigned
constexpr uint8_t FRAME_START = 0xAA;
constexpr uint8_t CMD_PING = 0x01;      // payload: none -> reply CMD_PONG with feederAddress
constexpr uint8_t CMD_PONG = 0x81;
constexpr uint8_t CMD_SET_ADDR = 0x02;  // payload: [newAddr] -> reply CMD_ACK/CMD_NACK
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
  buf[n++] = feederAddress; // frames from a feeder are always tagged with its own address
  buf[n++] = cmd;
  buf[n++] = payloadLen;
  for (uint8_t i = 0; i < payloadLen; i++) buf[n++] = payload[i];
  buf[n] = crc8(&buf[1], n - 1); // CRC over ADDR..PAYLOAD (not the start byte)
  n++;
  rs485Write(buf, n);
}

void handleFrame(uint8_t addr, uint8_t cmd, const uint8_t *payload, uint8_t len) {
  // Only react to broadcast, our own assigned address, or (while unassigned)
  // the shared parking address.
  const bool forUs = (addr == 0x00) || (addr == feederAddress) ||
                      (feederAddress == PARKING_ADDRESS && addr == PARKING_ADDRESS);
  if (!forUs) return;

  switch (cmd) {
    case CMD_PING: {
      sendFrame(CMD_PONG, nullptr, 0);
      break;
    }
    case CMD_SET_ADDR: {
      if (feederAddress != PARKING_ADDRESS) {
        // Already has a permanent address - refuse to be silently
        // reassigned by rail traffic. Re-addressing an already-assigned
        // feeder should be a deliberate host action, not implemented here.
        sendFrame(CMD_NACK, nullptr, 0);
        break;
      }
      if (len < 1 || !assignAddress(payload[0])) {
        sendFrame(CMD_NACK, nullptr, 0);
        break;
      }
      sendFrame(CMD_ACK, &payload[0], 1);
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
// Debug port (Serial1): status/help + local address assignment for bench use
// ---------------------------
void printStatus() {
  Serial1.print(F("addr="));
  if (feederAddress == PARKING_ADDRESS) Serial1.print(F("PARKING"));
  else Serial1.print(feederAddress);
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
  Serial1.println(F("  WHOAMI       print current bus address"));
  Serial1.println(F("  ADDR <n>     assign bus address n (1-247), persisted to EEPROM"));
  Serial1.println(F("                only while addr==PARKING (mirrors CMD_SET_ADDR over RS485)"));
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
  if (upper == "WHOAMI") {
    Serial1.print(F("address="));
    Serial1.println(feederAddress == PARKING_ADDRESS ? F("PARKING") : String(feederAddress));
    return;
  }
  if (upper.startsWith("ADDR ")) {
    const int n = upper.substring(5).toInt();
    if (feederAddress != PARKING_ADDRESS) {
      Serial1.println(F("Refused: already has a permanent address, use a host reassignment flow."));
    } else if (n < ADDR_MIN || n > ADDR_MAX || !assignAddress((uint8_t)n)) {
      Serial1.println(F("Refused: address must be 1-247."));
    } else {
      Serial1.print(F("Address assigned: ")); Serial1.println(n);
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

  loadAddressFromEeprom();

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
