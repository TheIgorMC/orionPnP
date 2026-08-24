#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "pins_config.h"

/*
  DRV8833 + AS5600 closed-loop wheel-position test bench (ATmega328PB-AU)

  Ported from TestBench03 (STM32F411CC Blackpill). Behavior and command
  protocol are identical; only the MCU-specific bits changed:

  - Pin assignments moved out to pins_config.h.
  - No native USB: this talks over hardware USART0 (D0/D1) at SERIAL_BAUD,
    through an external USB-serial adapter (or later, an RS485 transceiver
    on the same UART).
  - Runs on the ATmega328PB's internal 8MHz RC oscillator (no crystal on
    this board). SERIAL_BAUD defaults to 9600 because the internal
    oscillator's ~2% factory tolerance makes 115200 unreliable; 9600
    tolerates that error comfortably and matches the planned RS485 link
    speed.
  - I2C clock dropped to 100kHz (standard mode) instead of TestBench03's
    400kHz: at 8MHz F_CPU the TWBR value needed for 400kHz is very low,
    which distorts the SCL duty cycle. 100kHz has plenty of margin for the
    AS5600's read rate needs here.

  - Motor A drives the 40-tooth sprocket wheel. An AS5600 magnetic encoder
    (mounted on the wheel shaft) reports absolute angle over I2C so firmware
    can close the loop and stop exactly on target.
  - Motor B channel exists on the DRV8833 but is intentionally NEVER driven.
    Its inputs are held braked for the whole program lifetime.
  - On boot (and on demand via the ZERO command) firmware auto-calibrates the
    "still" duty: the highest PWM duty that does NOT move the wheel, and the
    breakaway duty just above it, separately for each direction. Closed-loop
    moves use that calibration instead of a guessed constant.
  - Target position can be commanded as an absolute angle, an absolute tooth
    index, or a relative +/-1 tooth / +/-0.5 tooth step, over serial.
  - Two jog buttons: one jogs +1 tooth (forward), the other -1 tooth
    (reverse). There is no hardware abort button - see the STOP command
    note near handleSerialLine().
*/

// ---------------------------
// AS5600 registers
// ---------------------------
const uint8_t AS5600_ADDR = 0x36;
const uint8_t AS5600_REG_STATUS = 0x0B;
const uint8_t AS5600_REG_RAW_ANGLE = 0x0C; // + 0x0D
const uint8_t AS5600_REG_ANGLE = 0x0E;     // + 0x0F (filtered/hysteresis)
const uint8_t AS5600_REG_AGC = 0x1A;
const uint8_t AS5600_REG_MAGNITUDE = 0x1B; // + 0x1C

// ---------------------------
// Wheel geometry
// ---------------------------
const int TOOTH_COUNT = 40;
const float DEG_PER_TOOTH = 360.0f / TOOTH_COUNT; // 9.0 deg
const float DEG_PER_HALF_TOOTH = DEG_PER_TOOTH / 2.0f; // 4.5 deg

// ---------------------------
// Motion / control tuning
// ---------------------------
const float ANGLE_TOLERANCE_DEG = 0.30f;    // stop window (AS5600 step ~0.088 deg)
const float CREEP_THRESHOLD_DEG = 3.0f;     // switch to creep duty inside this error
const int   FAST_DUTY = 110;                // duty used while error is large
const int   DEFAULT_MIN_MOVE_DUTY = 60;     // fallback creep duty if calibration fails

const unsigned long MOVE_TIMEOUT_MS = 6000;
const unsigned long STALL_TIMEOUT_MS = 800;
const float STALL_MOVE_THRESHOLD_DEG = 0.15f;

const bool BUTTONS_ACTIVE_LOW = true;
const unsigned long DEBOUNCE_MS = 20;

// Flip if the wheel turns opposite to what you commanded.
const bool INVERT_DIRECTION = false;

// Comms: internal 8MHz RC oscillator has ~2% factory tolerance, which makes
// 115200 unreliable. 9600 has plenty of margin and matches the planned
// RS485 link speed.
const unsigned long SERIAL_BAUD = 9600;

// I2C clock: kept at standard mode (100kHz) rather than TestBench03's
// 400kHz - see file header comment.
const uint32_t I2C_CLOCK_HZ = 100000;

// ---------------------------
// Zero-point (still-duty) calibration tuning
// ---------------------------
const int   CAL_START_DUTY = 15;
const int   CAL_MAX_DUTY = 200;
const int   CAL_STEP_DUTY = 5;
const unsigned long CAL_STEP_MS = 60;
const float CAL_MOVE_THRESHOLD_DEG = 0.6f;  // must clear AS5600 noise floor
const int   CAL_MARGIN_DUTY = 6;            // headroom added above measured breakaway
const unsigned long CAL_RESTORE_TIMEOUT_MS = 4000;

// ---------------------------
// Runtime state
// ---------------------------
float targetAngleDeg = 0.0f;
int minMoveDutyFwd = DEFAULT_MIN_MOVE_DUTY;
int minMoveDutyRev = DEFAULT_MIN_MOVE_DUTY;
int stillDutyMax = DEFAULT_MIN_MOVE_DUTY - CAL_STEP_DUTY; // informational "zero point"

unsigned long lastGoFwdEdgeMs = 0;
unsigned long lastGoRevEdgeMs = 0;
unsigned long lastFaultLogMs = 0;
unsigned long lastHeartbeatMs = 0;
float lastHeartbeatAngle = 0.0f;
bool lastHeartbeatAngleValid = false;

// Debug/diagnostics state
bool traceMoveEnabled = true;      // print periodic progress during moves
unsigned long moveSeq = 0;         // move counter, tags trace/abort lines
float lastGoodAngleDeg = 0.0f;     // last successfully read AS5600 angle
unsigned long i2cErrorCount = 0;
unsigned long lastI2cErrorLogMs = 0;

String serialLineBuf;

// ---------------------------
// AS5600 helpers
// ---------------------------
bool i2cReadBytes(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    i2cErrorCount++;
    return false;
  }
  if (Wire.requestFrom((uint8_t)AS5600_ADDR, len) != len) {
    i2cErrorCount++;
    return false;
  }
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

// Returns the last known-good angle and logs (throttled) if the live read
// failed, instead of silently feeding garbage into the stall/tolerance math.
float readAngleDeg() {
  uint16_t raw;
  if (as5600ReadRaw12(AS5600_REG_ANGLE, raw)) {
    lastGoodAngleDeg = (raw * 360.0f) / 4096.0f;
  } else if (millis() - lastI2cErrorLogMs > 500) {
    lastI2cErrorLogMs = millis();
    Serial.print(F("WARN: AS5600 I2C read failed ("));
    Serial.print(i2cErrorCount);
    Serial.println(F(" total), using last known angle"));
  }
  return lastGoodAngleDeg;
}

uint8_t readStatus() {
  uint8_t v = 0;
  as5600ReadByte(AS5600_REG_STATUS, v);
  return v;
}

uint8_t readAgc() {
  uint8_t v = 0;
  as5600ReadByte(AS5600_REG_AGC, v);
  return v;
}

uint16_t readMagnitude() {
  uint16_t v = 0;
  as5600ReadRaw12(AS5600_REG_MAGNITUDE, v);
  return v;
}

bool magnetDetected() {
  return (readStatus() & 0x20) != 0; // MD bit
}

// Interprets the MD/ML/MH status bits into a one-word verdict, instead of
// leaving the operator to decode raw bits by hand.
const char *magnetHealthStr(uint8_t status) {
  if (!(status & 0x20)) return "NONE";   // MD=0, no magnet seen at all
  if (status & 0x08) return "STRONG";    // MH=1, too close / saturating
  if (status & 0x10) return "WEAK";      // ML=1, too far / weak field
  return "OK";
}

// Shared one-line magnet diagnostic, reused by STATUS and by move-abort logs.
void printMagnetLine(bool endLine) {
  const uint8_t status = readStatus();
  const uint8_t agc = readAgc();
  const uint16_t magnitude = readMagnitude();
  Serial.print(F("magnet MD="));
  Serial.print((status & 0x20) ? 1 : 0);
  Serial.print(F(" ML="));
  Serial.print((status & 0x10) ? 1 : 0);
  Serial.print(F(" MH="));
  Serial.print((status & 0x08) ? 1 : 0);
  Serial.print(F(" AGC="));
  Serial.print(agc);
  Serial.print(F(" MAG="));
  Serial.print(magnitude);
  Serial.print(F(" health="));
  Serial.print(magnetHealthStr(status));
  if (endLine) Serial.println();
}

// ---------------------------
// Angle math
// ---------------------------
float normalizeDeg(float deg) {
  float d = fmodf(deg, 360.0f);
  if (d < 0) d += 360.0f;
  return d;
}

// Shortest signed path from current to target, in (-180, 180].
float angleErrorDeg(float target, float current) {
  float diff = fmodf(target - current + 540.0f, 360.0f) - 180.0f;
  return diff;
}

// ---------------------------
// Motor drive
// ---------------------------
// On AVR, Arduino's digitalWrite() already tears down a pin's PWM/timer
// compare-output state when writing to it directly (turnOffPWM() in the
// core), unlike the STM32 core this firmware was ported from, where that
// isn't guaranteed. The pinMode() calls below are therefore redundant on
// this MCU - kept anyway as a cheap, harmless belt-and-suspenders in case
// of a future core/timer change, and to keep this function identical in
// shape to the STM32 version it was ported from.
void brakeMotorA() {
  pinMode(PIN_AIN1, OUTPUT);
  pinMode(PIN_AIN2, OUTPUT);
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

// Motor B is never driven. Called once from setup() and never again.
void lockMotorBOff() {
  pinMode(PIN_BIN1, OUTPUT);
  pinMode(PIN_BIN2, OUTPUT);
  digitalWrite(PIN_BIN1, HIGH);
  digitalWrite(PIN_BIN2, HIGH); // hard brake / safe hold
}

bool buttonPressed(int pin, unsigned long &lastEdgeMs) {
  const int activeLevel = BUTTONS_ACTIVE_LOW ? LOW : HIGH;
  if (digitalRead(pin) == activeLevel) {
    const unsigned long now = millis();
    if (now - lastEdgeMs >= DEBOUNCE_MS) {
      lastEdgeMs = now;
      while (digitalRead(pin) == activeLevel) {
        delay(1);
      }
      return true;
    }
  }
  return false;
}

// ---------------------------
// Closed-loop move
// ---------------------------

// Shared tail for every abort reason: where we were, where we're going, how
// long it took, what duty/direction was applied, and current magnet health.
void printMoveAbortDetail(float startAngle, float target, float current,
                           unsigned long start, unsigned long lastMoveMs,
                           int duty, bool forward) {
  Serial.print(F("  start="));
  Serial.print(startAngle, 2);
  Serial.print(F(" target="));
  Serial.print(target, 2);
  Serial.print(F(" current="));
  Serial.print(current, 2);
  Serial.print(F(" err="));
  Serial.print(angleErrorDeg(target, current), 2);
  Serial.print(F(" deg | elapsed="));
  Serial.print(millis() - start);
  Serial.print(F("ms sinceMotion="));
  Serial.print(millis() - lastMoveMs);
  Serial.print(F("ms | duty="));
  Serial.print(duty);
  Serial.print(F(" dir="));
  Serial.print(forward ? "FWD" : "REV");
  Serial.print(F(" | "));
  printMagnetLine(true);
}

bool moveToAngle(float target, unsigned long timeoutMs) {
  const unsigned long moveId = ++moveSeq;
  const unsigned long start = millis();
  unsigned long lastMoveMs = millis();
  unsigned long lastTraceMs = 0;
  float lastAngle = readAngleDeg();
  const float startAngle = lastAngle;

  if (traceMoveEnabled) {
    Serial.print(F("[move "));
    Serial.print(moveId);
    Serial.print(F("] start="));
    Serial.print(startAngle, 2);
    Serial.print(F(" target="));
    Serial.print(target, 2);
    Serial.print(F(" err="));
    Serial.print(angleErrorDeg(target, startAngle), 2);
    Serial.println(F(" deg"));
  }

  while (true) {
    if (digitalRead(PIN_nFAULT) == LOW) {
      brakeMotorA();
      Serial.print(F("[move "));
      Serial.print(moveId);
      Serial.println(F("] ERROR: DRV8833 fault asserted, move aborted"));
      return false;
    }
    if (!magnetDetected()) {
      brakeMotorA();
      Serial.print(F("[move "));
      Serial.print(moveId);
      Serial.println(F("] ERROR: magnet lost during move (MD=0), move aborted"));
      return false;
    }

    const float current = readAngleDeg();
    const float err = angleErrorDeg(target, current);

    if (fabsf(err) <= ANGLE_TOLERANCE_DEG) {
      brakeMotorA();
      Serial.print(F("[move "));
      Serial.print(moveId);
      Serial.print(F("] Reached target: "));
      Serial.print(current, 2);
      Serial.println(F(" deg"));
      return true;
    }

    const bool forward = err > 0;
    const int duty = (fabsf(err) > CREEP_THRESHOLD_DEG)
        ? FAST_DUTY
        : (forward ? minMoveDutyFwd : minMoveDutyRev);
    driveMotorA(duty, forward);

    const float movedSinceLast = fabsf(angleErrorDeg(current, lastAngle));
    if (movedSinceLast > STALL_MOVE_THRESHOLD_DEG) {
      lastAngle = current;
      lastMoveMs = millis();
    }

    if (traceMoveEnabled && millis() - lastTraceMs >= 150) {
      lastTraceMs = millis();
      Serial.print(F("[move "));
      Serial.print(moveId);
      Serial.print(F("] t="));
      Serial.print(millis() - start);
      Serial.print(F("ms angle="));
      Serial.print(current, 2);
      Serial.print(F(" err="));
      Serial.print(err, 2);
      Serial.print(F(" duty="));
      Serial.print(duty);
      Serial.print(F(" dir="));
      Serial.print(forward ? "FWD" : "REV");
      Serial.print(F(" sinceMotion="));
      Serial.print(millis() - lastMoveMs);
      Serial.println(F("ms"));
    }

    if (millis() - lastMoveMs > STALL_TIMEOUT_MS) {
      brakeMotorA();
      Serial.print(F("[move "));
      Serial.print(moveId);
      Serial.println(F("] ERROR: stall detected (no encoder motion), move aborted"));
      printMoveAbortDetail(startAngle, target, current, start, lastMoveMs, duty, forward);
      return false;
    }
    if (millis() - start > timeoutMs) {
      brakeMotorA();
      Serial.print(F("[move "));
      Serial.print(moveId);
      Serial.println(F("] ERROR: move timeout, aborted"));
      printMoveAbortDetail(startAngle, target, current, start, lastMoveMs, duty, forward);
      return false;
    }

    delay(5);
  }
}

// Wraps moveToAngle() for every user-facing command (jog buttons/STEP/T/A).
// If the move fails partway, targetAngleDeg previously stayed at the
// unreached value, so the *next* relative jog would add on top of a
// position the wheel never actually reached, compounding silently across
// failures. Resyncing to the real angle here, and logging when it happens,
// fixes the drift and makes it visible instead of silent.
bool commandMoveTo(float target, unsigned long timeoutMs) {
  const bool ok = moveToAngle(target, timeoutMs);
  if (!ok) {
    const float actual = readAngleDeg();
    const float drift = angleErrorDeg(target, actual);
    if (fabsf(drift) > ANGLE_TOLERANCE_DEG) {
      Serial.print(F("NOTE: resyncing target to actual angle "));
      Serial.print(actual, 2);
      Serial.print(F(" deg (was "));
      Serial.print(fabsf(drift), 2);
      Serial.println(F(" deg off) so the next jog/step tracks real position instead of compounding the miss"));
    }
    targetAngleDeg = actual;
  }
  return ok;
}

// ---------------------------
// Zero-point (still-duty) calibration
// ---------------------------
bool rampFindBreakaway(bool forward, float baselineDeg, int &stillMaxOut, int &breakawayOut) {
  stillMaxOut = CAL_START_DUTY - CAL_STEP_DUTY;
  for (int duty = CAL_START_DUTY; duty <= CAL_MAX_DUTY; duty += CAL_STEP_DUTY) {
    driveMotorA(duty, forward);
    delay(CAL_STEP_MS);
    const float now = readAngleDeg();
    const float moved = fabsf(angleErrorDeg(now, baselineDeg));
    if (moved > CAL_MOVE_THRESHOLD_DEG) {
      brakeMotorA();
      breakawayOut = duty;
      return true;
    }
    stillMaxOut = duty;
  }
  brakeMotorA();
  return false; // never broke away within CAL_MAX_DUTY
}

void calibrateZero() {
  Serial.println(F("Calibrating zero point (still-duty) ..."));
  brakeMotorA();
  delay(200);

  if (!magnetDetected()) {
    Serial.println(F("WARN: AS5600 magnet not detected, skipping calibration, using defaults"));
    minMoveDutyFwd = DEFAULT_MIN_MOVE_DUTY;
    minMoveDutyRev = DEFAULT_MIN_MOVE_DUTY;
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
    Serial.print(F("Calibration OK. still<=duty fwd="));
    Serial.print(stillFwd);
    Serial.print(F(" breakaway fwd="));
    Serial.print(breakFwd);
    Serial.print(F(" | still<=duty rev="));
    Serial.print(stillRev);
    Serial.print(F(" breakaway rev="));
    Serial.println(breakRev);
  } else {
    Serial.println(F("WARN: calibration incomplete (no breakaway found up to CAL_MAX_DUTY), using defaults"));
    minMoveDutyFwd = DEFAULT_MIN_MOVE_DUTY;
    minMoveDutyRev = DEFAULT_MIN_MOVE_DUTY;
    stillDutyMax = DEFAULT_MIN_MOVE_DUTY - CAL_STEP_DUTY;
  }

  // Best-effort: restore the wheel to where it was before calibration nudged it.
  moveToAngle(startAngle, CAL_RESTORE_TIMEOUT_MS);
  targetAngleDeg = readAngleDeg();
}

// ---------------------------
// Status / help
// ---------------------------
void printStatus() {
  const float angle = readAngleDeg();
  const float toothPos = angle / DEG_PER_TOOTH;

  Serial.print(F("angle="));
  Serial.print(angle, 2);
  Serial.print(F(" deg | tooth="));
  Serial.print(toothPos, 2);
  Serial.print(F(" | target="));
  Serial.print(targetAngleDeg, 2);
  Serial.print(F(" deg | "));
  printMagnetLine(false);
  Serial.print(F(" | stillDutyMax="));
  Serial.print(stillDutyMax);
  Serial.print(F(" minMoveFwd="));
  Serial.print(minMoveDutyFwd);
  Serial.print(F(" minMoveRev="));
  Serial.print(minMoveDutyRev);
  Serial.print(F(" | i2cErrors="));
  Serial.print(i2cErrorCount);
  Serial.print(F(" trace="));
  Serial.println(traceMoveEnabled ? "ON" : "OFF");
}

void printHelp() {
  Serial.println(F("Commands (newline terminated):"));
  Serial.println(F("  A<deg>      move to absolute angle, e.g. A90.0"));
  Serial.println(F("  T<index>    move to tooth index 0..39, e.g. T5"));
  Serial.println(F("  STEP+1      move +1 tooth   (9.0 deg)"));
  Serial.println(F("  STEP-1      move -1 tooth"));
  Serial.println(F("  STEP+0.5    move +0.5 tooth (4.5 deg)"));
  Serial.println(F("  STEP-0.5    move -0.5 tooth"));
  Serial.println(F("  ZERO        re-run still-duty (zero point) calibration"));
  Serial.println(F("  STOP        brake motor A immediately"));
  Serial.println(F("  STATUS      print angle/target/calibration/I2C error state"));
  Serial.println(F("  TRACE ON    print live progress (angle/err/duty) during moves"));
  Serial.println(F("  TRACE OFF   silence live move progress, keep abort details"));
  Serial.println(F("  HELP / ?    show this text"));
  Serial.println(F("Hardware: one button jogs +1 tooth (fwd), the other jogs -1 tooth (rev)."));
  Serial.println(F("No hardware abort button: a move ends via target/stall/timeout/fault only."));
}

// ---------------------------
// Serial command handling
// ---------------------------
void handleSerialLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  String upper = line;
  upper.toUpperCase();

  if (upper == "STATUS") { printStatus(); return; }
  if (upper == "ZERO") { calibrateZero(); return; }
  if (upper == "STOP") { brakeMotorA(); Serial.println(F("Stopped.")); return; }
  if (upper == "HELP" || upper == "?") { printHelp(); return; }
  if (upper == "TRACE ON") { traceMoveEnabled = true; Serial.println(F("Move trace: ON")); return; }
  if (upper == "TRACE OFF") { traceMoveEnabled = false; Serial.println(F("Move trace: OFF")); return; }

  if (upper.startsWith("STEP")) {
    const float steps = upper.substring(4).toFloat(); // tooth units, supports 0.5
    const float deltaDeg = steps * DEG_PER_TOOTH;
    targetAngleDeg = normalizeDeg(targetAngleDeg + deltaDeg);
    Serial.print(F("Target -> "));
    Serial.print(targetAngleDeg, 2);
    Serial.println(F(" deg"));
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
    return;
  }

  if (upper.startsWith("T")) {
    int idx = upper.substring(1).toInt();
    idx = ((idx % TOOTH_COUNT) + TOOTH_COUNT) % TOOTH_COUNT;
    targetAngleDeg = idx * DEG_PER_TOOTH;
    Serial.print(F("Target tooth "));
    Serial.print(idx);
    Serial.print(F(" -> "));
    Serial.print(targetAngleDeg, 2);
    Serial.println(F(" deg"));
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
    return;
  }

  if (upper.startsWith("A")) {
    const float deg = upper.substring(1).toFloat();
    targetAngleDeg = normalizeDeg(deg);
    Serial.print(F("Target -> "));
    Serial.print(targetAngleDeg, 2);
    Serial.println(F(" deg"));
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
    return;
  }

  Serial.println(F("Unknown command. Type HELP."));
}

// ---------------------------
// Setup / loop
// ---------------------------
void setup() {
  pinMode(PIN_GO_FWD_INPUT, INPUT_PULLUP);
  pinMode(PIN_GO_REV_INPUT, INPUT_PULLUP);

  pinMode(PIN_AIN1, OUTPUT);
  pinMode(PIN_AIN2, OUTPUT);
  pinMode(PIN_nSLEEP, OUTPUT);
  pinMode(PIN_nFAULT, INPUT_PULLUP);

  brakeMotorA();
  lockMotorBOff(); // motor B pins set once here, never touched again

  digitalWrite(PIN_nSLEEP, HIGH); // wake DRV8833

  Wire.begin();
  Wire.setClock(I2C_CLOCK_HZ);

  Serial.begin(SERIAL_BAUD);

  Serial.println(F("DRV8833 + AS5600 wheel position test bench ready"));
  Serial.print(F("Wheel: "));
  Serial.print(TOOTH_COUNT);
  Serial.print(F(" teeth, "));
  Serial.print(DEG_PER_TOOTH, 2);
  Serial.println(F(" deg/tooth"));

  if (!magnetDetected()) {
    Serial.println(F("WARN: AS5600 magnet not detected at startup. Check magnet gap/alignment."));
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
    // loop() only reaches here between commanded moves (moveToAngle() blocks
    // synchronously while a move runs), so any significant motion between
    // heartbeats with no move in progress means the motor is being driven
    // by something outside the closed-loop logic.
    if (lastHeartbeatAngleValid) {
      const float drift = fabsf(angleErrorDeg(angleNow, lastHeartbeatAngle));
      if (drift > 1.0f) {
        Serial.print(F("WARN: wheel moved "));
        Serial.print(drift, 2);
        Serial.println(F(" deg between heartbeats with no move in progress - motor may be driving unexpectedly (check brake / PWM pin state)"));
      }
    }
    lastHeartbeatAngle = angleNow;
    lastHeartbeatAngleValid = true;
    printStatus();
  }

  if (digitalRead(PIN_nFAULT) == LOW) {
    brakeMotorA();
    if (now - lastFaultLogMs >= 500) {
      lastFaultLogMs = now;
      Serial.println(F("FAULT asserted on DRV8833 -> motor A braked"));
    }
    return;
  }

  if (buttonPressed(PIN_GO_FWD_INPUT, lastGoFwdEdgeMs)) {
    targetAngleDeg = normalizeDeg(targetAngleDeg + DEG_PER_TOOTH);
    Serial.print(F("[GO+] jog +1 tooth -> target "));
    Serial.print(targetAngleDeg, 2);
    Serial.println(F(" deg"));
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
  }

  if (buttonPressed(PIN_GO_REV_INPUT, lastGoRevEdgeMs)) {
    targetAngleDeg = normalizeDeg(targetAngleDeg - DEG_PER_TOOTH);
    Serial.print(F("[GO-] jog -1 tooth -> target "));
    Serial.print(targetAngleDeg, 2);
    Serial.println(F(" deg"));
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
  }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLineBuf.length() > 0) {
        handleSerialLine(serialLineBuf);
        serialLineBuf = "";
      }
    } else {
      serialLineBuf += c;
    }
  }
}
