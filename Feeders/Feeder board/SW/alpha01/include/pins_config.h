#pragma once

/*
  alpha01 pin map (ATmega328PB-AU) - matches the real Feeder board schematic
  SCH_Feeder_V02.pdf, version V0.2a (MCU page) / V0.2b (Serial page).

  Unlike TestBench04 (an ad-hoc ISP dev-rig wiring), these pins are taken
  directly from the actual board net names, so this file should track the
  schematic 1:1 - if a revision moves a signal, update it here first.

  Pin numbers are Arduino-style digital/analog numbers as exposed by
  MiniCore's ATmega328PB variant (classic Uno-compatible numbering:
  D0-D13, A0-A5). SDA/SCL/USART0/USART1 are fixed in silicon and not
  reassignable - listed below for reference only.
*/

#include <Arduino.h>

// ---------------------------------------------------------------
// RS485 bus - hardware USART0, fixed pins, not reassignable:
//   RO (transceiver -> MCU RX) -> D0 (RXD0)
//   DI (MCU TX -> transceiver) -> D1 (TXD0)
// This is the live feeder bus. Nothing should ever Serial.print() to it -
// use Serial1 (below) for debug/local output instead.
// ---------------------------------------------------------------
constexpr uint8_t PIN_RS485_RE = 2; // combined RE#/DE direction control (MAX1487): HIGH = transmit, LOW = receive

// ---------------------------------------------------------------
// Local debug/programming UART - hardware USART1, fixed pins, shared with
// the ISP programming header (MOSI0/TXD1 and MISO0/RXD1 are the same
// silicon pins - see Feeder-Design wiki page, "Programming Header"):
//   TX1 -> D11 (MOSI0/TXD1)
//   RX1 -> D12 (MISO0/RXD1)
// Use Serial1 for all human-readable output. Only reachable through the
// programming header, and mutually exclusive with ISP flashing on that
// same header at any given instant.
// ---------------------------------------------------------------

// ---------------------------------------------------------------
// DRV8833 channel A -> sprocket wheel motor (the only motor ever driven)
// ---------------------------------------------------------------
constexpr uint8_t PIN_AIN1 = 9;  // PWM (Timer1/OC1A), forward duty
constexpr uint8_t PIN_AIN2 = 10; // PWM (Timer1/OC1B), reverse duty

// ---------------------------------------------------------------
// DRV8833 channel B -> NOT USED. Wired for board compatibility only,
// held braked once in setup() and never touched again.
// ---------------------------------------------------------------
constexpr uint8_t PIN_BIN1 = 5;
constexpr uint8_t PIN_BIN2 = 6;

// ---------------------------------------------------------------
// DRV8833 control/status pins
// ---------------------------------------------------------------
constexpr uint8_t PIN_nSLEEP = 4; // set HIGH to enable driver
constexpr uint8_t PIN_nFAULT = 8; // input, active LOW (DRV_FLT net, PB0)

// ---------------------------------------------------------------
// AS5600 magnetic encoder - hardware TWI0, fixed pins, not reassignable:
//   SDA -> A4
//   SCL -> A5
// ---------------------------------------------------------------

// ---------------------------------------------------------------
// User switches (SW1/SW2 on the board; SW3 is a physical RESET button,
// wired straight to the MCU RESET pin, not GPIO)
// ---------------------------------------------------------------
constexpr uint8_t PIN_SW1 = A0;
constexpr uint8_t PIN_SW2 = A1;

// ---------------------------------------------------------------
// Status indicators
// ---------------------------------------------------------------
constexpr uint8_t PIN_RGB_DATA = 3; // single SK6812 LED, WS2812-compatible timing
constexpr uint8_t PIN_FAULT_LED = 7; // separate simple board-fault LED, not part of the RGB chain
