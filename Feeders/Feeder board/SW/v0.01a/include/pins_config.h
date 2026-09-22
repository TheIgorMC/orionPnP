#pragma once

/*
  v0.01a pin map (ATmega328PB-AU) - forked from beta1, unchanged pin map
  (see beta1/pins_config.h history for how these were arrived at, incl.
  the PIN_5V_READY/PIN_485_RELAY correction). Base is the real Feeder
  board schematic SCH_Feeder_V02.pdf (V0.2a/V0.2b), inherited from
  alpha01-03, PLUS the additions below (I_MON, 485_RELAY, 5V_READY) and
  the ext LED move (already anticipated in alpha03, see PIN_EXT_LED).
  Most of these pins haven't been on real hardware yet - this firmware
  is written for a schematic revision that doesn't exist as a built
  board yet, same situation alpha03/beta1 were already in for their own
  additions.

  Otherwise, pins are taken directly from the actual board net names, so
  this file should track the schematic 1:1 - if a revision moves a signal,
  update it here first.

  Pin numbers are Arduino-style digital/analog numbers as exposed by
  MiniCore's ATmega328PB variant (classic Uno-compatible numbering:
  D0-D13, A0-A5, plus A6/A7 for the 328PB's extra PE2/PE3 pins - see
  PIN_I_MON/PIN_485_RELAY below for why these two specifically are used;
  PIN_5V_READY is on A2, one of the classic ADC0-5 pins, not A6/A7).
  SDA/SCL/USART0/USART1 are fixed in silicon and not reassignable -
  listed below for reference only.
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
// DRV8833 channel <-> motor mapping - SWAPPED from alpha01-03 on the
// real beta1 board, confirmed by the first bench test: commanding
// "motor A" (the closed-loop feed/sprocket motor logic in main.cpp,
// driveMotorA()/moveToAngle() etc.) actually spun the peel motor
// instead. That's a channel-level swap (which DRV8833 H-bridge drives
// which physical motor connector), not the within-channel direction
// swap invertMotorA=true was added for - the two are different bugs.
// Fixed here rather than in main.cpp, same philosophy as invertMotorA:
// main.cpp's "motor A" stays defined as "the closed-loop feed motor
// with the AS5600 encoder" and "motor B" as "the open-loop peel motor,"
// only the pin numbers backing those roles change. Both pin pairs are
// driven with plain analogWrite() (see driveMotorA()/driveMotorB() in
// main.cpp), so which AVR timer backs which pair doesn't matter -
// swapping the constants is a complete, safe fix.
//
// invertMotorA=true (below) was set based on the OLD, since-corrected
// channel assignment - it may or may not still be the right polarity
// now that "motor A" drives a different physical channel. NOT
// re-verified against this fix yet; re-test direction on real hardware
// before trusting it.
// ---------------------------------------------------------------
constexpr uint8_t PIN_AIN1 = 5;  // PWM (Timer0/OC0A), forward duty - closed-loop feed/sprocket motor
constexpr uint8_t PIN_AIN2 = 6;  // PWM (Timer0/OC0B), reverse duty

constexpr uint8_t PIN_BIN1 = 9;  // PWM (Timer1/OC1A), forward duty - open-loop peel motor, no encoder
constexpr uint8_t PIN_BIN2 = 10; // PWM (Timer1/OC1B), reverse duty

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

// ---------------------------------------------------------------
// Power sequencing / rail monitoring - new in beta1, not on any V0.2a
// board built so far.
//
// PIN_I_MON is on A6 (PE2/ADC6), one of the ATmega328PB's two extra
// ADC-capable pins that don't exist on the classic 328P - confirmed
// correct against the actual installed MiniCore toolchain
// (pins_arduino.h: PIN_A6=25, PIN_PE2=25, analogPinToChannel(25)=6/ADC6).
//
// PIN_5V_READY moved to A2 (PC2/ADC2, one of the classic ADC0-5 pins) -
// NOT A7/PE3 as originally speced here. PE3 (A7) is the relay drive
// (PIN_485_RELAY, below) instead. Two separate, real signals - the
// earlier draft of this file had them backwards.
// ---------------------------------------------------------------
// TPS26600 eFuse IMON output (RIMON=309k, 1%) - voltage proportional to
// 12V rail load current, linear through the origin: ~4.07V at the 200mA
// design max load. Read against the default AVCC reference (unlike
// PIN_5V_READY below) - see readIMonMilliamps() in main.cpp for the
// counts-to-mA conversion. EN/FLT# are not wired to the MCU: the MCU only
// runs once the eFuse is already on, so there's no fault state where
// firmware could still be reading a pin to report it.
constexpr uint8_t PIN_I_MON = A6; // PE2/ADC6 - analog, TPS26600 IMON (12V rail current sense)

// 4.7k (rail side) / 1k (GND side) resistor divider off the 5V rail ->
// nominally ~0.88V at the pin when the rail is healthy (~816/1023 against
// the 1.1V reference below - 80% of full scale, clips only above ~6.27V
// rail, comfortably clear of a 5V rail's normal tolerance). Picked over a
// 10k/1k divider (~0.45V, ~422/1023, clips above ~12.1V) for roughly 2x
// the resolution - there's no realistic scenario on a "5V" rail that
// needs headroom all the way to 12V. See main.cpp's power-up sequencing
// section for why this MUST be read against the ATmega's internal 1.1V
// bandgap reference, not the default AVCC/VCC reference - using AVCC
// would be measuring the divided 5V rail against a reference that IS the
// same 5V rail, which reads the same fixed ratio regardless of the
// rail's actual absolute value and so cannot detect "still ramping up"
// at all - true regardless of which pin senses it, not specific to A7.
constexpr uint8_t PIN_5V_READY = A2; // PC2/ADC2 - analog, 5V rail via 4.7k/1k divider

// Digital output -> small N-channel MOSFET gate -> relay coil that
// physically connects/disconnects this feeder's RS485 A/B lines to the
// shared rail. HIGH = MOSFET on = relay energized = bus connected
// (low-side switch: GPIO -> gate, MOSFET source -> GND with a pull-down
// resistor, relay coil between the MOSFET drain and the coil supply rail
// - flip RELAY_ACTIVE_HIGH in main.cpp if the actual board inverts this).
// On A7/PE3, NOT D13/PB5 as originally speced here - freeing D13/PB5
// back up (unused since PIN_EXT_LED moved off it in alpha03; nothing
// reuses it now). See main.cpp for the power-up sequencing this gates
// (does not engage until PIN_5V_READY has read stable for
// RELAY_READY_STABLE_MS).
constexpr uint8_t PIN_485_RELAY = A7; // PE3 - RS485 bus-connect relay coil (via MOSFET), OUTPUT
