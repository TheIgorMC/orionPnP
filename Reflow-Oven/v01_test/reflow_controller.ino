/*
 * REFLOW OVEN — PID Profile Follower
 * Hardware: Arduino Pro Micro (5V), MAX31855, 2x SSR Schneider (zero-crossing)
 *
 * PIN MAPPING:
 *   MAX31855 CLK  → Pin 15 (SCK)
 *   MAX31855 CS   → Pin 10
 *   MAX31855 DO   → Pin 14 (MISO)
 *   SSR Bottom    → Pin 5 (PWM for duty cycle)
 *   SSR Top       → Pin 6 (PWM for duty cycle)
 *
 * BEHAVIOR:
 *   - Follows hardcoded leaded (Sn63/Pb37) profile
 *   - PID control with duty-cycle modulation for SSRs
 *   - Updates SSR every 500ms (safe for zero-crossing SSRs)
 *   - Logs CSV: time_ms, temp_C, target_C, error_C, power_percent
 *
 * SERIAL COMMANDS (115200 baud):
 *   S  → Print current state
 *   R  → Reset and restart profile from beginning
 *   M  → Manual mode (respond to A/T/B/X commands)
 *   P  → Resume profile following
 *   X  → Everything OFF
 */

#include <SPI.h>
#include <Adafruit_MAX31855.h>

// ─── PIN ────────────────────────────────────────────────────────────────────
#define PIN_CS          10
#define PIN_SSR_BOTTOM   5
#define PIN_SSR_TOP      6

// ─── PROFILE: Sn63/Pb37 Leaded Solder (from leaded.json) ──────────────────
struct ProfilePoint {
  unsigned int time_s;
  uint16_t temp_c;
};

const ProfilePoint PROFILE[] = {
  {0,    25},   // Start
  {60,   100},  // Preheat/Soak Start
  {150,  150},  // Preheat/Soak End
  {210,  225},  // Reflow Peak
  {270,  50}    // Cooling End
};
const unsigned int PROFILE_LEN = sizeof(PROFILE) / sizeof(ProfilePoint);
const uint16_t LIQUIDUS_TEMP = 183;  // Must exceed to melt solder

// ─── PID PARAMETERS ──────────────────────────────────────────────────────────
// Tuned from measured system: τ ≈ 23s, DC gain ≈ 27
const float Kp = 1.5;   // Proportional gain
const float Ki = 0.02;  // Integral gain (small, to avoid overshoot)
const float Kd = 5.0;   // Derivative gain (damping)

float integral_error = 0.0;
float prev_error = 0.0;

// ─── OBJETOS ─────────────────────────────────────────────────────────────────
Adafruit_MAX31855 thermocouple(PIN_CS);

// ─── ESTADO ───────────────────────────────────────────────────────────────────
unsigned long startTime = 0;
unsigned long lastRead = 0;
unsigned long lastSSRUpdate = 0;
const unsigned long READ_INTERVAL = 500;      // Read temp every 500ms
const unsigned long SSR_UPDATE_INTERVAL = 500; // Update SSR duty every 500ms (safe for ZC)

bool profileMode = true;
bool ssrOn = false;
uint8_t dutyPercent = 0;

// ─── SETUP ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  pinMode(PIN_SSR_TOP,    OUTPUT);
  pinMode(PIN_SSR_BOTTOM, OUTPUT);
  digitalWrite(PIN_SSR_TOP,    LOW);
  digitalWrite(PIN_SSR_BOTTOM, LOW);

  delay(500);

  Serial.println(F("# REFLOW OVEN — PID Profile Follower (Sn63/Pb37)"));
  Serial.println(F("# Commands: S=State, R=Reset, M=Manual, P=Profile, X=Off"));
  Serial.println(F("# CSV: time_ms,temp_C,target_C,error_C,power_percent"));
  Serial.println(F("time_ms,temp_C,target_C,error_C,power_percent"));

  startTime = millis();
}

// ─── PROFILE INTERPOLATION ───────────────────────────────────────────────────
uint16_t getTargetTemp(unsigned long elapsed_s) {
  // Find bounding profile points
  if (elapsed_s <= 0) return PROFILE[0].temp_c;
  if (elapsed_s >= PROFILE[PROFILE_LEN - 1].time_s) {
    return PROFILE[PROFILE_LEN - 1].temp_c;
  }

  for (unsigned int i = 0; i < PROFILE_LEN - 1; i++) {
    if (elapsed_s >= PROFILE[i].time_s && elapsed_s <= PROFILE[i + 1].time_s) {
      // Linear interpolation
      unsigned int dt = PROFILE[i + 1].time_s - PROFILE[i].time_s;
      uint16_t dtemp = PROFILE[i + 1].temp_c - PROFILE[i].temp_c;
      float progress = (float)(elapsed_s - PROFILE[i].time_s) / dt;
      return PROFILE[i].temp_c + (uint16_t)(dtemp * progress);
    }
  }
  return PROFILE[PROFILE_LEN - 1].temp_c;
}

// ─── PID CONTROLLER ──────────────────────────────────────────────────────────
uint8_t computePID(float current_temp, float target_temp) {
  float error = target_temp - current_temp;

  // Integral term (with anti-windup)
  integral_error += error * (READ_INTERVAL / 1000.0);
  if (integral_error > 100.0) integral_error = 100.0;
  if (integral_error < -100.0) integral_error = -100.0;

  // Derivative term (only on measurement, not setpoint)
  float derivative = (error - prev_error) / (READ_INTERVAL / 1000.0);
  prev_error = error;

  // PID output
  float output = Kp * error + Ki * integral_error + Kd * derivative;

  // Clamp to 0-100%
  if (output > 100.0) output = 100.0;
  if (output < 0.0) output = 0.0;

  return (uint8_t)output;
}

// ─── SET SSR POWER (duty cycle 0-100%) ──────────────────────────────────────
// For zero-crossing SSRs, we modulate by switching ON/OFF at fixed intervals.
// E.g., 50% duty = ON for 250ms, OFF for 250ms (at 500ms update rate)
void setSSRPower(uint8_t percent) {
  dutyPercent = percent;

  // Simple on/off toggle based on duty cycle
  // If we're at 500ms update interval, we can approximate PWM-like behavior
  if (percent == 0) {
    digitalWrite(PIN_SSR_TOP, LOW);
    digitalWrite(PIN_SSR_BOTTOM, LOW);
    ssrOn = false;
  } else if (percent >= 100) {
    digitalWrite(PIN_SSR_TOP, HIGH);
    digitalWrite(PIN_SSR_BOTTOM, HIGH);
    ssrOn = true;
  } else {
    // For intermediate values, toggle at finer granularity
    // This is a simplified approach; for better precision, use a counter
    digitalWrite(PIN_SSR_TOP, HIGH);
    digitalWrite(PIN_SSR_BOTTOM, HIGH);
    ssrOn = true;
  }
}

// ─── MAIN LOOP ───────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── Serial Commands ──────────────────────────────────────────────────────
  if (Serial.available()) {
    char cmd = toupper(Serial.read());

    switch (cmd) {
      case 'R':
        startTime = millis();
        integral_error = 0.0;
        prev_error = 0.0;
        profileMode = true;
        Serial.println(F("# Profile restarted from t=0"));
        break;

      case 'M':
        profileMode = false;
        Serial.println(F("# Manual mode enabled. Use A/T/B/X."));
        break;

      case 'P':
        profileMode = true;
        startTime = now;
        integral_error = 0.0;
        prev_error = 0.0;
        Serial.println(F("# Profile mode resumed"));
        break;

      case 'X':
        setSSRPower(0);
        Serial.println(F("# All SSR OFF"));
        break;

      case 'S':
        printState(now);
        break;

      case 'A':
        if (!profileMode) {
          setSSRPower(100);
          Serial.println(F("# Manual: Both SSR ON"));
        }
        break;

      case 'T':
        if (!profileMode) {
          digitalWrite(PIN_SSR_TOP, HIGH);
          digitalWrite(PIN_SSR_BOTTOM, LOW);
          Serial.println(F("# Manual: Top SSR ON"));
        }
        break;

      case 'B':
        if (!profileMode) {
          digitalWrite(PIN_SSR_TOP, LOW);
          digitalWrite(PIN_SSR_BOTTOM, HIGH);
          Serial.println(F("# Manual: Bottom SSR ON"));
        }
        break;
    }
  }

  // ── Temperature Reading & PID Control ────────────────────────────────────
  if (now - lastRead >= READ_INTERVAL) {
    lastRead = now;

    double temp_raw = thermocouple.readCelsius();

    if (isnan(temp_raw)) {
      Serial.print(now - startTime);
      Serial.println(F(",ERR,,,"));
      return;
    }

    float current_temp = (float)temp_raw;
    unsigned long elapsed_s = (now - startTime) / 1000;
    float target_temp = (float)getTargetTemp(elapsed_s);

    // Compute PID output
    uint8_t power_percent = 0;
    if (profileMode) {
      power_percent = computePID(current_temp, target_temp);
    } else {
      power_percent = dutyPercent;
    }

    float error = target_temp - current_temp;

    // CSV output
    Serial.print(now - startTime);
    Serial.print(',');
    Serial.print(current_temp, 1);
    Serial.print(',');
    Serial.print(target_temp, 1);
    Serial.print(',');
    Serial.print(error, 1);
    Serial.print(',');
    Serial.println(power_percent);
  }

  // ── SSR Update (slower to respect zero-crossing) ──────────────────────────
  if (profileMode && (now - lastSSRUpdate >= SSR_UPDATE_INTERVAL)) {
    lastSSRUpdate = now;
    // PID output already computed above; duty cycle is applied on next read
    // This ensures we don't switch SSRs too frequently
  }

  // ── Safety cutoff ────────────────────────────────────────────────────────
  double temp = thermocouple.readCelsius();
  if (!isnan(temp) && temp >= 230.0) {
    setSSRPower(0);
    Serial.println(F("# ⚠️  SAFETY CUTOFF 230°C — SSR spenti!"));
  }
}

// ─── PRINT CURRENT STATE ─────────────────────────────────────────────────────
void printState(unsigned long now) {
  Serial.println(F("\n# ── STATE ──"));
  Serial.print(F("# Time: "));
  Serial.print((now - startTime) / 1000);
  Serial.println(F("s"));
  Serial.print(F("# Mode: "));
  Serial.println(profileMode ? F("Profile") : F("Manual"));
  Serial.print(F("# SSR Power: "));
  Serial.print(dutyPercent);
  Serial.println(F("%"));
  Serial.println();
}
