/*
  DRV8833 simple motor test bench (STM32F411CC Blackpill)

  - Two digital inputs:
      GO   -> run both motors forward at low speed (sync)
      STOP -> hard brake both motors
  - Uses DRV8833 channel A and B, one motor per channel.
*/

// ---------------------------
// User-configurable pin map
// ---------------------------

// Control inputs (buttons to GND, use INPUT_PULLUP)
const int PIN_GO_INPUT = PB12;
const int PIN_STOP_INPUT = PB13;

// DRV8833 channel A motor
const int PIN_AIN1 = PA8;   // PWM-capable pin preferred
const int PIN_AIN2 = PA9;

// DRV8833 channel B motor
const int PIN_BIN1 = PA10;
const int PIN_BIN2 = PB9;

// DRV8833 control/status pins
const int PIN_nSLEEP = PB14;
const int PIN_nFAULT = PB15;

// Low-speed setting for precision (0..255 for analogWrite)
const int LOW_SPEED_DUTY = 40;  // ~16% duty
const int START_KICK_DUTY = 110;
const unsigned long START_KICK_MS = 300;

// Basic debounce timing
const unsigned long DEBOUNCE_MS = 20;
const bool BUTTONS_ACTIVE_LOW = true;
const bool SWAP_GO_STOP_ACTIONS = true;

bool motorRunning = false;
unsigned long lastGoEdgeMs = 0;
unsigned long lastStopEdgeMs = 0;
unsigned long lastFaultLogMs = 0;
unsigned long lastHeartbeatMs = 0;
unsigned long runStartedMs = 0;

void logState(const char *event) {
  Serial.print("[");
  Serial.print(millis());
  Serial.print(" ms] ");
  Serial.print(event);
  Serial.print(" | running=");
  Serial.print(motorRunning ? "1" : "0");
  Serial.print(" go=");
  Serial.print(digitalRead(PIN_GO_INPUT) == LOW ? "P" : "R");
  Serial.print(" stop=");
  Serial.print(digitalRead(PIN_STOP_INPUT) == LOW ? "P" : "R");
  Serial.print(" fault=");
  Serial.println(digitalRead(PIN_nFAULT) == LOW ? "1" : "0");
}

void hardBrakeAll() {
  // Hard brake: both inputs high on each channel.
  digitalWrite(PIN_AIN1, HIGH);
  digitalWrite(PIN_AIN2, HIGH);
  digitalWrite(PIN_BIN1, HIGH);
  digitalWrite(PIN_BIN2, HIGH);
}

void runForwardAll(int duty) {
  // Forward drive on both channels to keep motors synchronized.
  analogWrite(PIN_AIN1, duty);
  digitalWrite(PIN_AIN2, LOW);
  analogWrite(PIN_BIN1, duty);
  digitalWrite(PIN_BIN2, LOW);
}

bool buttonPressed(int pin, unsigned long &lastEdgeMs) {
  const int activeLevel = BUTTONS_ACTIVE_LOW ? LOW : HIGH;
  if (digitalRead(pin) == activeLevel) {
    unsigned long now = millis();
    if (now - lastEdgeMs >= DEBOUNCE_MS) {
      lastEdgeMs = now;
      // Wait for release to avoid repeated triggers on hold.
      while (digitalRead(pin) == activeLevel) {
        delay(1);
      }
      return true;
    }
  }
  return false;
}

void setup() {
  pinMode(PIN_GO_INPUT, INPUT_PULLUP);
  pinMode(PIN_STOP_INPUT, INPUT_PULLUP);

  pinMode(PIN_AIN1, OUTPUT);
  pinMode(PIN_AIN2, OUTPUT);
  pinMode(PIN_BIN1, OUTPUT);
  pinMode(PIN_BIN2, OUTPUT);

  pinMode(PIN_nSLEEP, OUTPUT);
  pinMode(PIN_nFAULT, INPUT_PULLUP);

  // Wake DRV8833
  digitalWrite(PIN_nSLEEP, HIGH);

  // Start safe: hard brake requested by user.
  hardBrakeAll();

  // USB CDC debug. If host is not ready, continue after a short timeout.
  Serial.begin(115200);
  const unsigned long waitStart = millis();
  while (!Serial && (millis() - waitStart < 1500)) {
    delay(10);
  }

  Serial.println("DRV8833 test bench ready");
  Serial.println("NOTE: BIN2 moved off PA11 to PB9 to avoid USB conflict.");
  Serial.print("Buttons active level: ");
  Serial.println(BUTTONS_ACTIVE_LOW ? "LOW (pull-up)" : "HIGH (pull-down)");
  Serial.print("GO/STOP mapping: ");
  Serial.println(SWAP_GO_STOP_ACTIONS ? "SWAPPED" : "NORMAL");
  logState("startup");
}

void loop() {
  // Heartbeat to confirm firmware activity even without button traffic.
  const unsigned long now = millis();
  if (now - lastHeartbeatMs >= 2000) {
    lastHeartbeatMs = now;
    logState("heartbeat");
  }

  // Fault line is active low on many DRV8833 breakout boards.
  // If fault occurs, force hard brake on both motors.
  if (digitalRead(PIN_nFAULT) == LOW) {
    if (motorRunning) {
      motorRunning = false;
      logState("fault -> stop");
    } else if (now - lastFaultLogMs >= 500) {
      lastFaultLogMs = now;
      logState("fault active");
    }
    hardBrakeAll();
    return;
  }

  const bool goPressed = SWAP_GO_STOP_ACTIONS
      ? buttonPressed(PIN_STOP_INPUT, lastStopEdgeMs)
      : buttonPressed(PIN_GO_INPUT, lastGoEdgeMs);

  const bool stopPressed = SWAP_GO_STOP_ACTIONS
      ? buttonPressed(PIN_GO_INPUT, lastGoEdgeMs)
      : buttonPressed(PIN_STOP_INPUT, lastStopEdgeMs);

  if (goPressed) {
    if (!motorRunning) {
      motorRunning = true;
      runStartedMs = now;
      logState("GO pressed -> run");
    }
  }

  if (stopPressed) {
    if (motorRunning) {
      motorRunning = false;
      logState("STOP pressed -> brake");
    }
  }

  if (motorRunning) {
    const int duty = (now - runStartedMs < START_KICK_MS) ? START_KICK_DUTY : LOW_SPEED_DUTY;
    runForwardAll(duty);
  } else {
    hardBrakeAll();
  }
}
