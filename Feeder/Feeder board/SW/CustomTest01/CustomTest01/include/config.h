#ifndef CONFIG_H
#define CONFIG_H

// ==== Motor A (DRV8833 H-Bridge) ====
#define MOTOR_A_IN1 5  // PWM capable
#define MOTOR_A_IN2 4  // Direction logic or PWM

// ==== Motor B ====
#define MOTOR_B_IN1 6  // PWM
#define MOTOR_B_IN2 7  // Direction logic or PWM

// ==== RS485 ====
#define RS485_RE_DE 2 // RE+DE control pin

// ==== Status RGB LEDs (Neopixels) ====
#define LED_RGB_PIN 3 // Data line for SK6812/WS2812 chain

// ==== Opto Sensors ====
#define OPTO1_PIN 8 // Digital or analog read
#define OPTO2_PIN 9

// ==== User Switches ====
#define SW1_PIN 10
#define SW2_PIN 11

// ==== Fault Pin (DRV8833 FAULT) ====
#define DRV_FAULT_PIN 12 // Input with pull-up

#endif
