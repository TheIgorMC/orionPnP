#pragma once

/*
  alpha03 pin map (ATmega328PB-AU) - base is the real Feeder board schematic
  SCH_Feeder_V02.pdf, version V0.2a (MCU page) / V0.2b (Serial page), PLUS
  one deliberate deviation from that schematic - see PIN_EXT_LED below.
  alpha03 is the beta1 codebase: it gets ahead of a hardware change that
  doesn't exist on any built V0.2a board yet.

  Otherwise, pins are taken directly from the actual board net names, so
  this file should track the schematic 1:1 - if a revision moves a signal,
  update it here first.

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
// I2C bus (hardware TWI0, fixed pins, not reassignable):
//   SDA -> A4
//   SCL -> A5
// Shared by two devices, distinguished by I2C address (no new pins
// needed for the second one):
//   - AS5600 magnetic encoder (0x36)
//   - AT24CS02 EEPROM + factory serial number (0x50 EEPROM page,
//     0x58 read-only identification page) - see AT24CS02 section in
//     main.cpp. Not present on any V0.2a board built so far; alpha03
//     is the first firmware to expect it, ahead of the beta1 schematic
//     actually adding it. I2C reads degrade gracefully (same
//     checked-endTransmission/requestFrom pattern as the AS5600 code)
//     if it isn't actually populated.
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
constexpr uint8_t PIN_RGB_DATA = 3; // single SK6812 LED, WS2812-compatible timing - live magnet-detect status
constexpr uint8_t PIN_FAULT_LED = 7; // separate simple board-fault LED, not part of the RGB chain

// SCHEMATIC DEVIATION from V0.2a/V0.2b, pending the beta1 revision: the
// old PIN_EXT_LED (D13/PB5, a plain digital LED) shared the ISP header's
// SCK line - mutually exclusive with ISP flashing at any given instant.
// Moved to its own pin, A3/PC3, which is unused/unpopulated on every
// V0.2a board built so far (it carried the old optical-interrupter
// signals before the AS5600 switch - see Feeder-Design wiki page) and
// isn't shared with anything else. No V0.2a board has this LED actually
// wired up yet; this pin assignment is what beta1's schematic should
// route it to.
//
// Standard red LED + series resistor sized for ~20mA, direct GPIO drive -
// no transistor needed. 20mA is comfortably inside the ATmega328PB's
// 40mA absolute-maximum DC current per I/O pin (Microchip datasheet,
// "Absolute Maximum Ratings"), and is the conventional design point for
// driving an LED straight off an AVR pin (same current class as, e.g.,
// the Arduino Uno's own onboard LED) - not something that needed a
// dedicated addressable LED or an external switch to handle safely.
constexpr uint8_t PIN_EXT_LED = A3; // PC3 - standard LED, simple on/off, not the ISP header
