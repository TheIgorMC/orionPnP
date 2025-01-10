#include "HostUtilities.h"
#include "PinoutConfig.h"

void enableRS485(uint8_t enablePin) {
    digitalWrite(enablePin, HIGH); // Set enable pin to HIGH to transmit
}

void disableRS485(uint8_t enablePin) {
    digitalWrite(enablePin, LOW); // Set enable pin to LOW to receive
}

void debugMessage(const char *msg) {
#ifdef DEBUG
    Serial.println(msg); // Print debug messages to the Serial monitor
#endif
}

void setFaultLED(bool state) {
    digitalWrite(FAULT_LED_PIN, state ? HIGH : LOW); // Set Fault LED based on state
}

void handleGCode(const String &gcode, HostManager &host) {
    if (gcode.startsWith("M115")) {
        // Respond with custom firmware string
        Serial.println("Custom Feeder Firmware V1.0");
    } else if (gcode.startsWith("M100")) {
        // Parse `F` parameter to extract feeder bank and address
        int fIndex = gcode.indexOf('F');
        if (fIndex > -1) {
            String fParam = gcode.substring(fIndex + 1);
            long feederParam = fParam.toInt();
            uint8_t bank = (feederParam / 10000) % 10;
            uint16_t address = feederParam % 10000;

            // Determine which RS485 bus to use
            uint8_t reDePin = (bank == 1) ? BUS1_RE_DE_PIN : BUS2_RE_DE_PIN;
            
            // Perform feed operation
            enableRS485(reDePin);
            host.feedOperation(address);
            disableRS485(reDePin);

            Serial.println("Feed operation initiated");
        } else {
            Serial.println("Invalid M100 command. Use M100 F<bank><address>.");
        }
    } else {
        Serial.println("Unknown G-code command.");
    }
}
