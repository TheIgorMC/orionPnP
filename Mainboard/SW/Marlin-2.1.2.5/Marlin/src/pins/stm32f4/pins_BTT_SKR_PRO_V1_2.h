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

#if HOTENDS > 3 || E_STEPPERS > 3
  #error "BIGTREE SKR Pro V1.2 supports up to 3 hotends / E steppers."
#endif

#define BOARD_INFO_NAME "BTT SKR Pro V1.2"

#define HAS_PRESSURE_SENSOR
#define DIRECT_PIN_CONTROL

// Pin mapping: 0 = PA0, 1 = PA1, ..., 15 = PA15, 16 = PB0, ..., 31 = PB15, ..., 191 = PL15

#define PIN_LED1           (2 * 16 + 8)   // PC8  = 40
#define PIN_LED2           (4 * 16 + 5)   // PE5  = 69

#define PIN_PUMP           (1 * 16 + 1)   // PB1  = 17

#define PIN_SOL1           (3 * 16 + 14)  // PD14 = 62
#define PIN_SOL2           (1 * 16 + 0)   // PB0  = 16
#define PIN_SOL3           (3 * 16 + 12)  // PD12 = 60
#define PIN_SOL4           (3 * 16 + 11)  // PD11 = 59

#include "pins_BTT_SKR_PRO_common.h"

#include "C:\Users\Mattia\Documents\GitHub\orionPnP\Mainboard\SW\Marlin-2.1.2.5\Marlin\src\gcode\feature\orion\orionPnP_codes.h"