#pragma once

/*
  TestBench04 pin map (ATmega328PB-AU).

  Edit this file when your board's pin routing changes. Nothing else in the
  firmware should need touching to move a signal to a different pin.

  Pin numbers are Arduino-style digital/analog numbers as exposed by
  MiniCore's ATmega328PB variant (classic Uno-compatible numbering:
  D0-D13, A0-A5). SDA/SCL and the UART are fixed in silicon (hardware
  TWI0 and USART0) and are not reassignable here - they're listed below
  for reference only.
*/

#include <Arduino.h>

// ---------------------------------------------------------------
// Control inputs (buttons to GND, INPUT_PULLUP)
// ---------------------------------------------------------------
constexpr uint8_t PIN_GO_FWD_INPUT = A0; // jog +1 tooth (forward)
constexpr uint8_t PIN_GO_REV_INPUT = A1; // jog -1 tooth (reverse)

// ---------------------------------------------------------------
// DRV8833 channel A -> sprocket wheel motor (the only motor ever driven)
// ---------------------------------------------------------------
constexpr uint8_t PIN_AIN1 = 9;  // PWM (Timer1/OC1A), forward duty
constexpr uint8_t PIN_AIN2 = 10; // PWM (Timer1/OC1B), reverse duty

// ---------------------------------------------------------------
// DRV8833 channel B -> NOT USED. Wired for board compatibility only,
// held braked once in setup() and never touched again.
// ---------------------------------------------------------------
constexpr uint8_t PIN_BIN1 = 7;
constexpr uint8_t PIN_BIN2 = 8;

// ---------------------------------------------------------------
// DRV8833 control/status pins
// ---------------------------------------------------------------
constexpr uint8_t PIN_nSLEEP = 4; // set HIGH to enable driver
constexpr uint8_t PIN_nFAULT = 2; // input, active LOW

// ---------------------------------------------------------------
// AS5600 magnetic encoder - hardware TWI0, fixed pins, not reassignable:
//   SDA -> A4
//   SCL -> A5
// ---------------------------------------------------------------

// ---------------------------------------------------------------
// USART0 - hardware UART, fixed pins, not reassignable:
//   RX -> D0
//   TX -> D1
// No native USB on this MCU: talk to it through an external USB-serial
// adapter today, or through an RS485 transceiver on the same UART later.
// ---------------------------------------------------------------
