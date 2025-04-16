#ifndef CONFIG_H
#define CONFIG_H


// ==== General configuration flags ====

#define ENABLE_SELFTEST 1 // Enable self-test mode for debugging
#define ENABLE_DEBUG 1 // Enable debug mode for verbose output
// #define ORION_IS_HOST // Uncomment this line if the device is a host board instead of a feeder

// ==== Inverted logic flags ====

#define INVERT_DIR_A 1 // Invert direction logic for motor A
#define INVERT_DIR_B 1 // Invert direction logic for motor B
#define INVERT_FAULT_LED 0 // Invert fault logic for fault LED
#define INVERT_OPTO 1 // Invert opto logic for opto sensors
#define SW_ACTIVE_LOW 1 // Invert logic for switches

// ==== EEPROM configuration ====
#define EEPROM_ADDR_LOCATION 0x00

// ==== Pin definitions ====

// ==== Motor A (DRV8833 H-Bridge) ====
#define MOTOR_A_IN1 PB1  // PWM capable
#define MOTOR_A_IN2 PB2  // Direction logic or PWM

// ==== Motor B ====
#define MOTOR_B_IN1 PD5  // PWM
#define MOTOR_B_IN2 PD6  // Direction logic or PWM

// ==== RS485 ====
#define RS485_RE_DE PD2 // RE+DE control pin

// ==== Status RGB LEDs (Neopixels) ====
#define LED_RGB_PIN PD3 // Data line for SK6812/WS2812 chain

// ==== Fault LED ====
#define FAULT_LED_PIN PD7 // Generic board fault LED

// ==== Opto Sensors ====
#define OPTO1_PIN PC2 // Digital or analog read
#define OPTO2_PIN PC3
#define DEBOUNCE_TIME_MS 10
#define OPTO_TRIGGER_DELAY_MS 2
#define OPTO_TIMEOUT_MS 2000 // Max time to wait for sensor


// ==== User Switches ====
#define SW1_PIN PC0
#define SW2_PIN PC1

// ==== Fault Pin (DRV8833 FAULT) ====
#define DRV_FAULT_PIN PB0 // Input with pull-up
#define DRV_SLEEP_PIN PD4 // Sleep pin for DRV8833

#endif
