#include <Arduino.h>
#include "config.h"

void setup() {
  pinMode(MOTOR_A_PWM, OUTPUT);
  pinMode(MOTOR_A_DIR, OUTPUT);
  pinMode(MOTOR_B_PWM, OUTPUT);
  pinMode(MOTOR_B_DIR, OUTPUT);
  pinMode(SLEEP_DRV, OUTPUT);

  digitalWrite(SLEEP_DRV, HIGH); // Wake up driver

  Serial.begin(115200);
  delay(1000); // Give serial time to start
  Serial.println(F("Starting motor test..."));
}

void loop() {
  // --- Motor A (Feed) Forward ---
  Serial.println(F("Motor A: FORWARD"));
  digitalWrite(MOTOR_A_DIR, LOW);
  analogWrite(MOTOR_A_PWM, DEFAULT_PWM);
  delay(1000);

  // Reverse
  Serial.println(F("Motor A: REVERSE"));
  digitalWrite(MOTOR_A_DIR, HIGH);
  analogWrite(MOTOR_A_PWM, DEFAULT_PWM);
  delay(1000);

  analogWrite(MOTOR_A_PWM, 0);
  Serial.println(F("Motor A: STOPPED"));

  // --- Motor B (Peel) Forward ---
  Serial.println(F("Motor B: FORWARD"));
  digitalWrite(MOTOR_B_DIR, LOW);
  analogWrite(MOTOR_B_PWM, DEFAULT_PWM);
  delay(1000);

  // Reverse
  Serial.println(F("Motor B: REVERSE"));
  digitalWrite(MOTOR_B_DIR, HIGH);
  analogWrite(MOTOR_B_PWM, DEFAULT_PWM);
  delay(1000);

  analogWrite(MOTOR_B_PWM, 0);
  Serial.println(F("Motor B: STOPPED"));

  delay(1000); // Pause between cycles
}
