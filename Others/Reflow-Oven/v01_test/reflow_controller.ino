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

enum ReflowPhase {
  PHASE_PREHEAT,
  PHASE_SOAK,
  PHASE_REFLOW,
  PHASE_COOLDOWN,
};

// ─── SIMPLE DEAD-TIME COMPENSATED PI ────────────────────────────────────────
// This uses a 25s ahead prediction: T_pred = T + slope_filtered * 25s
const float Kp = 3.2f;               // % power per C of predicted error
const float Ki = 0.020f;             // % power per (C*s)
const float THERMAL_DELAY_S = 25.0f; // Measured process dead time
const float SLOPE_FILTER_ALPHA = 0.20f;
const float RISE_LOOKAHEAD_S = 22.0f;
const unsigned long COOLDOWN_GUARD_SEC = 8; // Treat last seconds before cooldown as no-boost zone

float integral_error = 0.0f;
float prev_temp = 0.0f;
float filtered_slope = 0.0f;
bool predictor_initialized = false;

// ─── OBJETOS ─────────────────────────────────────────────────────────────────
Adafruit_MAX31855 thermocouple(PIN_CS);

// ─── ESTADO ───────────────────────────────────────────────────────────────────
unsigned long bootTime = 0;
unsigned long profileStartMs = 0;
unsigned long pausedProfileElapsedMs = 0;
unsigned long lastRead = 0;
unsigned long lastSSRUpdate = 0;
const unsigned long READ_INTERVAL = 250;       // Read temp every 250ms
const unsigned long SSR_UPDATE_INTERVAL = 100; // 0.1s software PWM update

bool profileMode = false;
bool profilePaused = false;
bool preheatMode = false;
float preheatTargetC = 100.0f;
bool ssrOn = false;
uint8_t dutyPercent = 0;
uint8_t ssrPwmSlot = 0;
unsigned long lastOpenDoorWarn = 0;
const unsigned long OPEN_DOOR_WARN_INTERVAL_MS = 3000;
unsigned long lastSafetyCutoffWarn = 0;
const unsigned long SAFETY_WARN_INTERVAL_MS = 2000;

// ─── SETUP ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.setTimeout(40);
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

  bootTime = millis();
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

ReflowPhase getPhase(unsigned long elapsed_s) {
  if (elapsed_s < PROFILE[1].time_s) {
    return PHASE_PREHEAT;
  }
  if (elapsed_s < PROFILE[2].time_s) {
    return PHASE_SOAK;
  }
  if (elapsed_s < PROFILE[3].time_s) {
    return PHASE_REFLOW;
  }
  return PHASE_COOLDOWN;
}

// ─── PREDICTIVE PI CONTROLLER ────────────────────────────────────────────────
uint8_t computePredictivePI(float current_temp, float target_temp) {
  const float dt_s = READ_INTERVAL / 1000.0f;

  if (!predictor_initialized) {
    predictor_initialized = true;
    prev_temp = current_temp;
    filtered_slope = 0.0f;
  }

  float raw_slope = (current_temp - prev_temp) / dt_s;
  filtered_slope += SLOPE_FILTER_ALPHA * (raw_slope - filtered_slope);
  prev_temp = current_temp;

  float predicted_temp = current_temp + filtered_slope * THERMAL_DELAY_S;
  float error = target_temp - predicted_temp;

  // Integrate only when close enough to target to remove small offsets.
  if (error > -15.0f && error < 15.0f) {
    integral_error += error * dt_s;
  }
  if (integral_error > 300.0f) integral_error = 300.0f;
  if (integral_error < -300.0f) integral_error = -300.0f;

  float output = Kp * error + Ki * integral_error;

  if (output > 100.0f) output = 100.0f;
  if (output < 0.0f) output = 0.0f;

  return (uint8_t)(output + 0.5f);
}

// ─── SET SSR POWER (duty cycle 0-100%) ──────────────────────────────────────
// For zero-crossing SSRs, we modulate by switching ON/OFF at fixed intervals.
// E.g., 50% duty with 100ms update -> 1s full window with 10 slots.
void setSSRPower(uint8_t percent) {
  dutyPercent = percent;

  // Fast-paths for hard OFF / hard ON.
  if (percent == 0) {
    digitalWrite(PIN_SSR_TOP, LOW);
    digitalWrite(PIN_SSR_BOTTOM, LOW);
    ssrOn = false;
  } else if (percent >= 100) {
    digitalWrite(PIN_SSR_TOP, HIGH);
    digitalWrite(PIN_SSR_BOTTOM, HIGH);
    ssrOn = true;
  } else {
    // Time-proportioning over 10 slots. With 100ms updates, full window = 1s.
    const uint8_t slots = 10;
    // Bias toward higher effective power for underpowered heaters.
    const uint8_t onSlots = (uint8_t)((percent * slots + 80) / 100);
    const bool onNow = (ssrPwmSlot < onSlots);

    digitalWrite(PIN_SSR_TOP, onNow ? HIGH : LOW);
    digitalWrite(PIN_SSR_BOTTOM, onNow ? HIGH : LOW);
    ssrOn = onNow;

    ssrPwmSlot = (uint8_t)((ssrPwmSlot + 1) % slots);
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
        preheatMode = false;
        profileMode = true;
        profilePaused = false;
        profileStartMs = now;
        pausedProfileElapsedMs = 0;
        integral_error = 0.0f;
        predictor_initialized = false;
        lastOpenDoorWarn = 0;
        Serial.println(F("# Profile started/restarted"));
        break;

      case 'M':
        if (profileMode) {
          pausedProfileElapsedMs = now - profileStartMs;
          profilePaused = true;
        }
        preheatMode = false;
        profileMode = false;
        Serial.println(F("# Manual mode enabled (profile paused). Use A/T/B/X."));
        break;

      case 'P':
        preheatMode = false;
        profileMode = true;
        if (profilePaused) {
          profileStartMs = now - pausedProfileElapsedMs;
        } else {
          profileStartMs = now;
        }
        profilePaused = false;
        integral_error = 0.0f;
        predictor_initialized = false;
        lastOpenDoorWarn = 0;
        Serial.println(F("# Profile mode running"));
        break;

      case 'X':
        preheatMode = false;
        profileMode = false;
        profilePaused = false;
        pausedProfileElapsedMs = 0;
        predictor_initialized = false;
        integral_error = 0.0f;
        setSSRPower(0);
        Serial.println(F("# All SSR OFF (Profile stopped)"));
        break;

      case 'H': {
        float t = Serial.parseFloat();
        if (t < 30.0f || t > 240.0f) {
          Serial.println(F("# Invalid preheat target (30..240C)"));
          break;
        }
        preheatTargetC = t;
        preheatMode = true;
        profileMode = false;
        profilePaused = false;
        pausedProfileElapsedMs = 0;
        integral_error = 0.0f;
        predictor_initialized = false;
        Serial.print(F("# Preheat target set: "));
        Serial.print(preheatTargetC, 1);
        Serial.println(F("C"));
        break;
      }

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
          dutyPercent = 100;
          ssrOn = true;
          Serial.println(F("# Manual: Top SSR ON"));
        }
        break;

      case 'B':
        if (!profileMode) {
          digitalWrite(PIN_SSR_TOP, LOW);
          digitalWrite(PIN_SSR_BOTTOM, HIGH);
          dutyPercent = 100;
          ssrOn = true;
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
      Serial.print(now - bootTime);
      Serial.println(F(",ERR,,,"));
      return;
    }

    float current_temp = (float)temp_raw;
    const unsigned long elapsed_log_ms = now - bootTime;
    float profile_target = 0.0;
    float effective_target = 0.0;
    float error = 0.0;
    bool has_target = false;
    ReflowPhase phase = PHASE_PREHEAT;
    unsigned long profile_elapsed_s = 0;
    bool near_or_in_cooldown = false;
    
    if (profileMode) {
      profile_elapsed_s = (now - profileStartMs) / 1000;
      profile_target = (float)getTargetTemp(profile_elapsed_s);
      phase = getPhase(profile_elapsed_s);
      near_or_in_cooldown = (phase == PHASE_COOLDOWN) || (profile_elapsed_s + COOLDOWN_GUARD_SEC >= PROFILE[3].time_s);

      // If current is above profile target, hold target at current to avoid extra heating.
      effective_target = profile_target;
      if (current_temp > profile_target) {
        effective_target = current_temp;
      }
      error = effective_target - current_temp;
      has_target = true;
    } else if (preheatMode) {
      effective_target = preheatTargetC;
      error = effective_target - current_temp;
      has_target = true;
    }

    // Compute PID output
    uint8_t power_percent = 0;
    if (profileMode || preheatMode) {
      power_percent = computePredictivePI(current_temp, effective_target);

      // If a major rise is needed in the next ~20-25s, push full power now.
      if (profileMode && !near_or_in_cooldown) {
        unsigned long lookahead_s = profile_elapsed_s + (unsigned long)RISE_LOOKAHEAD_S;
        float future_target = (float)getTargetTemp(lookahead_s);
        float rise_needed = future_target - effective_target;
        if (rise_needed >= 8.0f && current_temp < (future_target - 6.0f)) {
          power_percent = 100;
        }
      }

      // Maintain higher heating effort when clearly below target.
      if (!near_or_in_cooldown) {
        if (error > 20.0f && power_percent < 85) {
          power_percent = 85;
        } else if (error > 10.0f && power_percent < 65) {
          power_percent = 65;
        }
      }

      // On cooldown edge/in cooldown, if we are already on target band or hotter, force heaters off.
      if (profileMode && near_or_in_cooldown && current_temp >= (profile_target - 1.0f)) {
        power_percent = 0;
      }

      if (now - lastSSRUpdate >= SSR_UPDATE_INTERVAL) {
        setSSRPower(power_percent);
        lastSSRUpdate = now;
      }
    } else {
      power_percent = dutyPercent;
    }

    if (profileMode && phase == PHASE_COOLDOWN && current_temp > (profile_target + 5.0)) {
      if (now - lastOpenDoorWarn >= OPEN_DOOR_WARN_INTERVAL_MS) {
        Serial.println(F("# WARNING: COOLDOWN too slow — OPEN DOOR"));
        lastOpenDoorWarn = now;
      }
    }

    // CSV output
    Serial.print(elapsed_log_ms);
    Serial.print(',');
    Serial.print(current_temp, 1);
    Serial.print(',');
    if (has_target) {
      Serial.print(effective_target, 1);
    }
    Serial.print(',');
    if (has_target) {
      Serial.print(error, 1);
    }
    Serial.print(',');
    Serial.println(power_percent);
  }

  // ── Safety cutoff ────────────────────────────────────────────────────────
  double temp = thermocouple.readCelsius();
  if (!isnan(temp) && temp >= 230.0) {
    setSSRPower(0);
    if (now - lastSafetyCutoffWarn >= SAFETY_WARN_INTERVAL_MS) {
      Serial.println(F("# ⚠️  SAFETY CUTOFF 230°C — SSR spenti!"));
      lastSafetyCutoffWarn = now;
    }
  }
}

// ─── PRINT CURRENT STATE ─────────────────────────────────────────────────────
void printState(unsigned long now) {
  Serial.println(F("\n# ── STATE ──"));
  Serial.print(F("# Time: "));
  Serial.print((now - bootTime) / 1000);
  Serial.println(F("s"));
  Serial.print(F("# Mode: "));
  if (profileMode) {
    Serial.println(F("Profile"));
  } else if (preheatMode) {
    Serial.println(F("Preheat"));
  } else {
    Serial.println(F("Manual"));
  }
  if (profileMode || profilePaused) {
    unsigned long profileElapsedMs = profileMode ? (now - profileStartMs) : pausedProfileElapsedMs;
    Serial.print(F("# Profile Elapsed: "));
    Serial.print(profileElapsedMs / 1000);
    Serial.println(F("s"));
  }
  if (preheatMode) {
    Serial.print(F("# Preheat Target: "));
    Serial.print(preheatTargetC, 1);
    Serial.println(F("C"));
  }
  Serial.print(F("# SSR Power: "));
  Serial.print(dutyPercent);
  Serial.println(F("%"));
  Serial.println();
}
