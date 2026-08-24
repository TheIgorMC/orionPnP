/*
 * REFLOW OVEN — Step Response Test
 * Hardware: Arduino Pro Micro (5V), MAX31855, 2x SSR Schneider
 *
 * PIN MAPPING:
 *   MAX31855 CLK  → Pin 15 (SCK)
 *   MAX31855 CS   → Pin 10
 *   MAX31855 DO   → Pin 14 (MISO)
 *   SSR Bottom    → Pin 5
 *   SSR Top       → Pin 6
 *
 * COMANDI SERIALE (115200 baud):
 *   A  → Entrambi SSR ON  (step response)
 *   T  → Solo Top ON
 *   B  → Solo Bottom ON
 *   X  → Tutto OFF
 *   S  → Stampa stato attuale
 *
 * OUTPUT CSV: timestamp_ms, temperatura_C, top_state, bottom_state
 */

#include <SPI.h>
#include <Adafruit_MAX31855.h>

// ─── PIN ────────────────────────────────────────────────────────────────────
#define PIN_CS          10
#define PIN_SSR_BOTTOM   5
#define PIN_SSR_TOP      6

// ─── OGGETTI ─────────────────────────────────────────────────────────────────
Adafruit_MAX31855 thermocouple(PIN_CS);

// ─── STATO ───────────────────────────────────────────────────────────────────
bool ssrTop    = false;
bool ssrBottom = false;

unsigned long lastRead   = 0;
unsigned long startTime  = 0;
const unsigned long READ_INTERVAL = 500; // ms

// ─── SETUP ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  pinMode(PIN_SSR_TOP,    OUTPUT);
  pinMode(PIN_SSR_BOTTOM, OUTPUT);
  digitalWrite(PIN_SSR_TOP,    LOW);
  digitalWrite(PIN_SSR_BOTTOM, LOW);

  // MAX31855 ha bisogno di 500ms per stabilizzarsi
  delay(500);

  Serial.println(F("# REFLOW OVEN — Step Response Test"));
  Serial.println(F("# Comandi: A=All ON | T=Top | B=Bottom | X=OFF | S=Stato"));
  Serial.println(F("# CSV: time_ms,temp_C,top,bottom"));
  Serial.println(F("time_ms,temp_C,top,bottom"));

  startTime = millis();
}

// ─── LOOP ────────────────────────────────────────────────────────────────────
void loop() {

  // ── Lettura seriale ──────────────────────────────────────────────────────
  if (Serial.available()) {
    char cmd = toupper(Serial.read());

    switch (cmd) {
      case 'A':
        ssrTop = ssrBottom = true;
        Serial.println(F("# CMD: ENTRAMBI ON"));
        break;
      case 'T':
        ssrTop    = true;
        ssrBottom = false;
        Serial.println(F("# CMD: SOLO TOP ON"));
        break;
      case 'B':
        ssrTop    = false;
        ssrBottom = true;
        Serial.println(F("# CMD: SOLO BOTTOM ON"));
        break;
      case 'X':
        ssrTop = ssrBottom = false;
        Serial.println(F("# CMD: TUTTO OFF"));
        break;
      case 'S':
        Serial.print(F("# STATO → Top: "));
        Serial.print(ssrTop    ? "ON" : "OFF");
        Serial.print(F(" | Bottom: "));
        Serial.println(ssrBottom ? "ON" : "OFF");
        break;
    }

    // Applica subito
    digitalWrite(PIN_SSR_TOP,    ssrTop    ? HIGH : LOW);
    digitalWrite(PIN_SSR_BOTTOM, ssrBottom ? HIGH : LOW);
  }

  // ── Lettura temperatura ──────────────────────────────────────────────────
  unsigned long now = millis();
  if (now - lastRead >= READ_INTERVAL) {
    lastRead = now;

    double temp = thermocouple.readCelsius();

    if (isnan(temp)) {
      Serial.print(now - startTime);
      Serial.println(F(",ERR,"));
      return;
    }

    // Output CSV
    Serial.print(now - startTime);
    Serial.print(',');
    Serial.print(temp, 1);
    Serial.print(',');
    Serial.print(ssrTop    ? 1 : 0);
    Serial.print(',');
    Serial.println(ssrBottom ? 1 : 0);

    // ── Safety cutoff a 230°C ─────────────────────────────────────────────
    if (temp >= 230.0) {
      ssrTop = ssrBottom = false;
      digitalWrite(PIN_SSR_TOP,    LOW);
      digitalWrite(PIN_SSR_BOTTOM, LOW);
      Serial.println(F("# ⚠️  SAFETY CUTOFF 230°C — SSR spenti!"));
    }
  }
}
