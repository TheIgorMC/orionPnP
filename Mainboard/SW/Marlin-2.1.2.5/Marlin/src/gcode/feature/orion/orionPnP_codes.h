/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2021 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
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

#include "../../../inc/MarlinConfig.h"

#if ENABLED(HAS_PRESSURE_SENSOR)

#include "../../gcode.h"

#include "../../../feature/pressure/XGZP6897D.h"


// M10001: Read Pressure Sensor 1
void GcodeSuite::M10001() {
  
    float temperature, pressure;
    XGZP6897D XGZP6897D(2048, &TwoWire(PIN_SDA1, PIN_SCL1)); // K factor for the sensor, see datasheet
    XGZP6897D.readSensor(temperature, pressure);

    double result = pressure;
    SERIAL_ECHOLNPGM("P:", result);
}

// M10002: Read Pressure Sensor 2
void GcodeSuite::M10002() {
  
    float temperature, pressure;
    XGZP6897D XGZP6897D(2048, &TwoWire(PIN_SDA2, PIN_SCL2)); // K factor for the sensor, see datasheet
    XGZP6897D.readSensor(temperature, pressure);

    double result = pressure;
    SERIAL_ECHOLNPGM("P:", result);
}

// M10010: Bottom cam light ON
void GcodeSuite::M10010() {
    pinMode(PIN_LED1, OUTPUT);
    digitalWrite(PIN_LED1, HIGH);
    SERIAL_ECHOLNPGM("Bottom cam light ON");
}

// M10011: Bottom cam light OFF
void GcodeSuite::M10011() {
    pinMode(PIN_LED1, OUTPUT);
    digitalWrite(PIN_LED1, LOW);
    SERIAL_ECHOLNPGM("Bottom cam light OFF");
}

// M10012: Top cam light ON
void GcodeSuite::M10012() {
    pinMode(PIN_LED2, OUTPUT);
    digitalWrite(PIN_LED2, HIGH);
    SERIAL_ECHOLNPGM("Top cam light ON");
}

// M10013: Top cam light OFF
void GcodeSuite::M10013() {
    pinMode(PIN_LED2, OUTPUT);
    digitalWrite(PIN_LED2, LOW);
    SERIAL_ECHOLNPGM("Top cam light OFF");
}

// M10020: Turn pump ON
void GcodeSuite::M10020() {
    pinMode(PIN_PUMP, OUTPUT);
    digitalWrite(PIN_PUMP, HIGH);
    SERIAL_ECHOLNPGM("Pump ON");
}

// M10021: Turn pump OFF
void GcodeSuite::M10021() {
    pinMode(PIN_PUMP, OUTPUT);
    digitalWrite(PIN_PUMP, LOW);
    SERIAL_ECHOLNPGM("Pump OFF");
}

// M10030: Edit solenoid H1/2 S1/0
void GcodeSuite::M10030() {
    int solenoid = parser.intval('H', 1); // Default to solenoid 1
    int state = parser.intval('S', 1); // Default to ON

    if (solenoid < 1 || solenoid > 4) {
        SERIAL_ERRORLNPGM("Invalid solenoid number. Use H1-4.");
        return;
    }

    PinName pin;
    switch (solenoid) {
        case 1: pin = PIN_SOL1; break;
        case 2: pin = PIN_SOL2; break;
        case 3: pin = PIN_SOL3; break;
        case 4: pin = PIN_SOL4; break;
    }

    pinMode(pin, OUTPUT);
    digitalWrite(pin, state ? HIGH : LOW);
    
    SERIAL_ECHOLNPGM("Solenoid ", solenoid, " set to ", state ? "ON" : "OFF");
}

#endif
