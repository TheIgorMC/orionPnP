/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */
#pragma once

#include "env_validate.h"

#if HOTENDS > 0
  #error "Core board for OrionPnP supports no hotends. It is intended to be used on a PnP machine."
#endif

#define BOARD_INFO_NAME "OrionPnP Core V02"

#define DIRECT_PIN_CONTROL true // Enable direct pin control for M226

#define USES_DIAG_PINS

// Add support for pressure sensors on board
#ifndef HAS_PRESSURE_SENSOR
  #define HAS_PRESSURE_SENSOR true
#endif

#if NO_EEPROM_SELECTED
  //#define SRAM_EEPROM_EMULATION                 // Use BackSRAM-based EEPROM emulation
  #define FLASH_EEPROM_EMULATION                  // Use Flash-based EEPROM emulation
#endif

#ifdef FLASH_EEPROM_EMULATION
  // Decrease delays and flash wear by spreading writes across the
  // 128 kB sector allocated for EEPROM emulation.
  #define FLASH_EEPROM_LEVELING
#endif


// PIN SECTION

// Pins for camera LEDs
#define PIN_LED1           PC_8
#define PIN_LED2           PE_5

// Pin for pump control
#define PIN_PUMP           PB_1

// Pins for solenoid control
#define PIN_SOL1           PD_14
#define PIN_SOL2           PB_0
#define PIN_SOL3           PD_12
#define PIN_SOL4           PD_11

// I2C for interface 1, to pressure sensor 1
#define PIN_SDA1           PB_7
#define PIN_SCL1           PB_6

// I2C for interface 2, to pressure sensor 2
#define PIN_SDA2           PB_10
#define PIN_SCL2           PB_11

// Pins for stallguard/sensorless homing
#define X_DIAG_PIN                          PB10  // X-
#define Y_DIAG_PIN                          PE12  // Y-
#define Z_DIAG_PIN                          PG8   // Z-







//
// Limit Switches
//
#ifdef X_STALL_SENSITIVITY
  #define X_STOP_PIN                  X_DIAG_PIN
  #if X_HOME_TO_MIN
    #define X_MAX_PIN                       PE15  // E0
  #else
    #define X_MIN_PIN                       PE15  // E0
  #endif
#endif

#ifdef Y_STALL_SENSITIVITY
  #define Y_STOP_PIN                  Y_DIAG_PIN
  #if Y_HOME_TO_MIN
    #define Y_MAX_PIN                       PE10  // E1
  #else
    #define Y_MIN_PIN                       PE10  // E1
  #endif
#endif

#ifdef Z_STALL_SENSITIVITY
  #define Z_STOP_PIN                  Z_DIAG_PIN
  #if Z_HOME_TO_MIN
    #define Z_MAX_PIN                       PG5   // E2
  #else
    #define Z_MIN_PIN                       PG5   // E2
  #endif
#endif

//
// Steppers
//
#define X_STEP_PIN                          PE9
#define X_DIR_PIN                           PF1
#define X_ENABLE_PIN                        PF2
#ifndef X_CS_PIN
  #define X_CS_PIN                          PA15
#endif

#define Y_STEP_PIN                          PE11
#define Y_DIR_PIN                           PE8
#define Y_ENABLE_PIN                        PD7
#ifndef Y_CS_PIN
  #define Y_CS_PIN                          PB8
#endif

#define Y2_STEP_PIN                          PE13
#define Y2_DIR_PIN                           PC2
#define Y2_ENABLE_PIN                        PC0
#ifndef Y2_CS_PIN
  #define Y2_CS_PIN                          PC10
#endif

#define Z_STEP_PIN                          PE14
#define Z_DIR_PIN                           PA0
#define Z_ENABLE_PIN                        PC3
#ifndef Z_CS_PIN
  #define Z_CS_PIN                          PB9
#endif

#define I_STEP_PIN                          PD15
#define I_DIR_PIN                           PE7
#define I_ENABLE_PIN                        PA3
#ifndef I_CS_PIN
  #define I_CS_PIN                          PB3
#endif

#define J_STEP_PIN                         PD13
#define J_DIR_PIN                          PG9
#define J_ENABLE_PIN                       PF0
#ifndef J_CS_PIN
  #define J_CS_PIN                         PG15
#endif


#if HAS_TMC_UART
  
//TMC2208/TMC2209 stepper drivers

  #define X_SERIAL_TX_PIN                   PC13
  #define Y_SERIAL_TX_PIN                   PE3
  #define Y2_SERIAL_TX_PIN                   PE1
  #define Z_SERIAL_TX_PIN                  PD4
  #define I_SERIAL_TX_PIN                  PD1
  #define J_SERIAL_TX_PIN                  PD6

  // Reduce baud rate to improve software serial reliability
  #ifndef TMC_BAUD_RATE
    #define TMC_BAUD_RATE                  19200
  #endif

#endif // HAS_TMC_UART
