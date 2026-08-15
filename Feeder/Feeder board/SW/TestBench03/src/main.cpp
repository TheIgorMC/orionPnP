#include <Arduino.h>
#include <Wire.h>
#include <math.h>

/*
  DRV8833 + AS5600 closed-loop wheel-position test bench (STM32F411CC Blackpill)

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
    index, or a relative +/-1 tooth / +/-0.5 tooth step, over USB serial.
*/

// ---------------------------
// User-configurable pin map
// ---------------------------

// Control inputs (buttons to GND, use INPUT_PULLUP)
// No hardware abort input: a move in progress can only end via reaching
// target, the stall detector, the move timeout, or DRV8833 nFAULT. Serial
// STOP still works, but (like these buttons) only between moves, since
// moveToAngle() blocks and never polls Serial while a move is running.
const int PIN_GO_FWD_INPUT = PB12; // jog +1 tooth (forward)
const int PIN_GO_REV_INPUT = PB13; // jog -1 tooth (reverse)

// DRV8833 channel A -> sprocket wheel motor (the only motor ever driven)
const int PIN_AIN1 = PA8;  // PWM, forward duty
const int PIN_AIN2 = PA9;  // PWM, reverse duty

// DRV8833 channel B -> NOT USED. Wired for board compatibility only, held
// braked in setup() and never touched again.
const int PIN_BIN1 = PA10;
const int PIN_BIN2 = PB9;

// DRV8833 control/status pins
const int PIN_nSLEEP = PB14;
const int PIN_nFAULT = PB15;

// AS5600 magnetic encoder (I2C1)
const int PIN_I2C_SCL = PB6;
const int PIN_I2C_SDA = PB7;

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
    Serial.print("WARN: AS5600 I2C read failed (");
    Serial.print(i2cErrorCount);
    Serial.println(" total), using last known angle");
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
  Serial.print("magnet MD=");
  Serial.print((status & 0x20) ? 1 : 0);
  Serial.print(" ML=");
  Serial.print((status & 0x10) ? 1 : 0);
  Serial.print(" MH=");
  Serial.print((status & 0x08) ? 1 : 0);
  Serial.print(" AGC=");
  Serial.print(agc);
  Serial.print(" MAG=");
  Serial.print(magnitude);
  Serial.print(" health=");
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
// PA8/PA9 are put into hardware timer (PWM) mode by analogWrite() in
// driveMotorA(). A plain digitalWrite() afterward does not reliably tear
// that timer channel back down on this core, so the pin can keep outputting
// the last PWM duty even after "braking" (motor spins on with no move in
// progress and no error printed, since nothing in software is wrong).
// Forcing pinMode(OUTPUT) here guarantees a full GPIO re-init out of
// alternate-function/timer mode before the brake level is applied.
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
// This is the block that would have shown the 161 deg target/actual gap
// immediately instead of leaving it to be reconstructed from STATUS lines.
void printMoveAbortDetail(float startAngle, float target, float current,
                           unsigned long start, unsigned long lastMoveMs,
                           int duty, bool forward) {
  Serial.print("  start=");
  Serial.print(startAngle, 2);
  Serial.print(" target=");
  Serial.print(target, 2);
  Serial.print(" current=");
  Serial.print(current, 2);
  Serial.print(" err=");
  Serial.print(angleErrorDeg(target, current), 2);
  Serial.print(" deg | elapsed=");
  Serial.print(millis() - start);
  Serial.print("ms sinceMotion=");
  Serial.print(millis() - lastMoveMs);
  Serial.print("ms | duty=");
  Serial.print(duty);
  Serial.print(" dir=");
  Serial.print(forward ? "FWD" : "REV");
  Serial.print(" | ");
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
    Serial.print("[move ");
    Serial.print(moveId);
    Serial.print("] start=");
    Serial.print(startAngle, 2);
    Serial.print(" target=");
    Serial.print(target, 2);
    Serial.print(" err=");
    Serial.print(angleErrorDeg(target, startAngle), 2);
    Serial.println(" deg");
  }

  while (true) {
    if (digitalRead(PIN_nFAULT) == LOW) {
      brakeMotorA();
      Serial.print("[move ");
      Serial.print(moveId);
      Serial.println("] ERROR: DRV8833 fault asserted, move aborted");
      return false;
    }
    if (!magnetDetected()) {
      brakeMotorA();
      Serial.print("[move ");
      Serial.print(moveId);
      Serial.println("] ERROR: magnet lost during move (MD=0), move aborted");
      return false;
    }

    const float current = readAngleDeg();
    const float err = angleErrorDeg(target, current);

    if (fabsf(err) <= ANGLE_TOLERANCE_DEG) {
      brakeMotorA();
      Serial.print("[move ");
      Serial.print(moveId);
      Serial.print("] Reached target: ");
      Serial.print(current, 2);
      Serial.println(" deg");
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
      Serial.print("[move ");
      Serial.print(moveId);
      Serial.print("] t=");
      Serial.print(millis() - start);
      Serial.print("ms angle=");
      Serial.print(current, 2);
      Serial.print(" err=");
      Serial.print(err, 2);
      Serial.print(" duty=");
      Serial.print(duty);
      Serial.print(" dir=");
      Serial.print(forward ? "FWD" : "REV");
      Serial.print(" sinceMotion=");
      Serial.print(millis() - lastMoveMs);
      Serial.println("ms");
    }

    if (millis() - lastMoveMs > STALL_TIMEOUT_MS) {
      brakeMotorA();
      Serial.print("[move ");
      Serial.print(moveId);
      Serial.println("] ERROR: stall detected (no encoder motion), move aborted");
      printMoveAbortDetail(startAngle, target, current, start, lastMoveMs, duty, forward);
      return false;
    }
    if (millis() - start > timeoutMs) {
      brakeMotorA();
      Serial.print("[move ");
      Serial.print(moveId);
      Serial.println("] ERROR: move timeout, aborted");
      printMoveAbortDetail(startAngle, target, current, start, lastMoveMs, duty, forward);
      return false;
    }

    delay(5);
  }
}

// Wraps moveToAngle() for every user-facing command (GO/STEP/T/A). If the
// move fails partway, targetAngleDeg previously stayed at the unreached
// value, so the *next* relative jog (STEP/GO, which add onto targetAngleDeg)
// would add on top of a position the wheel never actually reached. Repeated
// failures compound this silently until a jog target ends up on the other
// side of the wheel (which is exactly what a large err on a "+1 tooth" jog
// means: it isn't 9 deg away, target had already drifted). Resyncing to the
// real angle here, and logging when it happens, fixes the drift and makes it
// visible instead of silent.
bool commandMoveTo(float target, unsigned long timeoutMs) {
  const bool ok = moveToAngle(target, timeoutMs);
  if (!ok) {
    const float actual = readAngleDeg();
    const float drift = angleErrorDeg(target, actual);
    if (fabsf(drift) > ANGLE_TOLERANCE_DEG) {
      Serial.print("NOTE: resyncing target to actual angle ");
      Serial.print(actual, 2);
      Serial.print(" deg (was ");
      Serial.print(fabsf(drift), 2);
      Serial.println(" deg off) so the next jog/step tracks real position instead of compounding the miss");
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
  Serial.println("Calibrating zero point (still-duty) ...");
  brakeMotorA();
  delay(200);

  if (!magnetDetected()) {
    Serial.println("WARN: AS5600 magnet not detected, skipping calibration, using defaults");
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
    Serial.print("Calibration OK. still<=duty fwd=");
    Serial.print(stillFwd);
    Serial.print(" breakaway fwd=");
    Serial.print(breakFwd);
    Serial.print(" | still<=duty rev=");
    Serial.print(stillRev);
    Serial.print(" breakaway rev=");
    Serial.println(breakRev);
  } else {
    Serial.println("WARN: calibration incomplete (no breakaway found up to CAL_MAX_DUTY), using defaults");
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

  Serial.print("angle=");
  Serial.print(angle, 2);
  Serial.print(" deg | tooth=");
  Serial.print(toothPos, 2);
  Serial.print(" | target=");
  Serial.print(targetAngleDeg, 2);
  Serial.print(" deg | ");
  printMagnetLine(false);
  Serial.print(" | stillDutyMax=");
  Serial.print(stillDutyMax);
  Serial.print(" minMoveFwd=");
  Serial.print(minMoveDutyFwd);
  Serial.print(" minMoveRev=");
  Serial.print(minMoveDutyRev);
  Serial.print(" | i2cErrors=");
  Serial.print(i2cErrorCount);
  Serial.print(" trace=");
  Serial.println(traceMoveEnabled ? "ON" : "OFF");
}

void printHelp() {
  Serial.println("Commands (newline terminated):");
  Serial.println("  A<deg>      move to absolute angle, e.g. A90.0");
  Serial.println("  T<index>    move to tooth index 0..39, e.g. T5");
  Serial.println("  STEP+1      move +1 tooth   (9.0 deg)");
  Serial.println("  STEP-1      move -1 tooth");
  Serial.println("  STEP+0.5    move +0.5 tooth (4.5 deg)");
  Serial.println("  STEP-0.5    move -0.5 tooth");
  Serial.println("  ZERO        re-run still-duty (zero point) calibration");
  Serial.println("  STOP        brake motor A immediately");
  Serial.println("  STATUS      print angle/target/calibration/I2C error state");
  Serial.println("  TRACE ON    print live progress (angle/err/duty) during moves");
  Serial.println("  TRACE OFF   silence live move progress, keep abort details");
  Serial.println("  HELP / ?    show this text");
  Serial.println("Hardware: one button jogs +1 tooth (fwd), the other jogs -1 tooth (rev).");
  Serial.println("No hardware abort button: a move ends via target/stall/timeout/fault only.");
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
  if (upper == "STOP") { brakeMotorA(); Serial.println("Stopped."); return; }
  if (upper == "HELP" || upper == "?") { printHelp(); return; }
  if (upper == "TRACE ON") { traceMoveEnabled = true; Serial.println("Move trace: ON"); return; }
  if (upper == "TRACE OFF") { traceMoveEnabled = false; Serial.println("Move trace: OFF"); return; }

  if (upper.startsWith("STEP")) {
    const float steps = upper.substring(4).toFloat(); // tooth units, supports 0.5
    const float deltaDeg = steps * DEG_PER_TOOTH;
    targetAngleDeg = normalizeDeg(targetAngleDeg + deltaDeg);
    Serial.print("Target -> ");
    Serial.print(targetAngleDeg, 2);
    Serial.println(" deg");
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
    return;
  }

  if (upper.startsWith("T")) {
    int idx = upper.substring(1).toInt();
    idx = ((idx % TOOTH_COUNT) + TOOTH_COUNT) % TOOTH_COUNT;
    targetAngleDeg = idx * DEG_PER_TOOTH;
    Serial.print("Target tooth ");
    Serial.print(idx);
    Serial.print(" -> ");
    Serial.print(targetAngleDeg, 2);
    Serial.println(" deg");
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
    return;
  }

  if (upper.startsWith("A")) {
    const float deg = upper.substring(1).toFloat();
    targetAngleDeg = normalizeDeg(deg);
    Serial.print("Target -> ");
    Serial.print(targetAngleDeg, 2);
    Serial.println(" deg");
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
    return;
  }

  Serial.println("Unknown command. Type HELP.");
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

  Wire.setSDA(PIN_I2C_SDA);
  Wire.setSCL(PIN_I2C_SCL);
  Wire.begin();
  Wire.setClock(400000);

  Serial.begin(115200);
  const unsigned long waitStart = millis();
  while (!Serial && (millis() - waitStart < 1500)) {
    delay(10);
  }

  Serial.println("DRV8833 + AS5600 wheel position test bench ready");
  Serial.print("Wheel: ");
  Serial.print(TOOTH_COUNT);
  Serial.print(" teeth, ");
  Serial.print(DEG_PER_TOOTH, 2);
  Serial.println(" deg/tooth");

  if (!magnetDetected()) {
    Serial.println("WARN: AS5600 magnet not detected at startup. Check magnet gap/alignment.");
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
    // by something outside the closed-loop logic (e.g. a stuck PWM output
    // that survived a brake) rather than a real bug in the move logic.
    if (lastHeartbeatAngleValid) {
      const float drift = fabsf(angleErrorDeg(angleNow, lastHeartbeatAngle));
      if (drift > 1.0f) {
        Serial.print("WARN: wheel moved ");
        Serial.print(drift, 2);
        Serial.println(" deg between heartbeats with no move in progress - motor may be driving unexpectedly (check brake / PWM pin state)");
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
      Serial.println("FAULT asserted on DRV8833 -> motor A braked");
    }
    return;
  }

  if (buttonPressed(PIN_GO_FWD_INPUT, lastGoFwdEdgeMs)) {
    targetAngleDeg = normalizeDeg(targetAngleDeg + DEG_PER_TOOTH);
    Serial.print("[GO+] jog +1 tooth -> target ");
    Serial.print(targetAngleDeg, 2);
    Serial.println(" deg");
    commandMoveTo(targetAngleDeg, MOVE_TIMEOUT_MS);
  }

  if (buttonPressed(PIN_GO_REV_INPUT, lastGoRevEdgeMs)) {
    targetAngleDeg = normalizeDeg(targetAngleDeg - DEG_PER_TOOTH);
    Serial.print("[GO-] jog -1 tooth -> target ");
    Serial.print(targetAngleDeg, 2);
    Serial.println(" deg");
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
