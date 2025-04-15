#include <Arduino.h>
#include "config.h"

uint8_t pwmVal = DEFAULT_PWM;
uint16_t onTime = DEFAULT_ON_TIME;
uint16_t offTime = DEFAULT_OFF_TIME;

void setupMotorPins() {
  pinMode(MOTOR_A_PWM, OUTPUT);
  pinMode(MOTOR_A_DIR, OUTPUT);
}

void motorForward(uint8_t pwm) {
  digitalWrite(MOTOR_A_DIR, LOW);
  analogWrite(MOTOR_A_PWM, pwm);
}

void motorStop() {
  analogWrite(MOTOR_A_PWM, 0);
}

void parseSerial() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.startsWith("PWM ")) {
      pwmVal = cmd.substring(4).toInt();
      Serial.print(F("Set PWM to "));
      Serial.println(pwmVal);
    } else if (cmd.startsWith("ON ")) {
      onTime = cmd.substring(3).toInt();
      Serial.print(F("Set ON time to "));
      Serial.println(onTime);
    } else if (cmd.startsWith("OFF ")) {
      offTime = cmd.substring(4).toInt();
      Serial.print(F("Set OFF time to "));
      Serial.println(offTime);
    } else if (cmd.startsWith("GO")) {
      Serial.println(F("Running motor..."));
      motorForward(pwmVal);
      delay(onTime);
      motorStop();
      delay(offTime);
    }
  }
}

void setup() {
  Serial.begin(115200);
  setupMotorPins();
  Serial.println(F("Feeder Test Ready. Send 'GO' to run."));
}

void loop() {
  parseSerial();
}
