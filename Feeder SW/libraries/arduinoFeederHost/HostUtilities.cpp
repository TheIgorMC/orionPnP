#include "HostUtilities.h"
#include "PinoutConfig.h"

void debugPrint(const char *msg) {
#ifdef DEBUG
    Serial.println(msg); // Print message only in debug mode
#endif
}

void enableRS485(uint8_t enablePin) {
    digitalWrite(enablePin, HIGH); // Set enable pin to HIGH to transmit
    debugPrint("RS485 Transmit Enabled");
}

void disableRS485(uint8_t enablePin) {
    digitalWrite(enablePin, LOW); // Set enable pin to LOW to receive
    debugPrint("RS485 Receive Enabled");
}

void setFaultLED(bool state) {
    digitalWrite(FAULT_LED_PIN, state ? HIGH : LOW);
    debugPrint(state ? "Fault LED ON" : "Fault LED OFF");
}

void handleGCode(const String &gcode, HostManager &host) {
    if (gcode.startsWith("M115")) {
        Serial.println("FIRMWARE_NAME: Orion_Feeder_Host FIRMWARE_VERSION: v01a");
        debugPrint("M115: Sent firmware version.");
    } else if (gcode.startsWith("M100")) {
        int fIndex = gcode.indexOf('F');
        if (fIndex > -1) {
            String fParam = gcode.substring(fIndex + 1);
            long feederParam = fParam.toInt();
            uint8_t bank = (feederParam / 10000) % 10;
            uint16_t address = feederParam % 10000;

            uint8_t reDePin = (bank == 1) ? BUS1_RE_DE_PIN : BUS2_RE_DE_PIN;

            debugPrint("M100: Initiating feed operation...");
            enableRS485(reDePin);
            host.feedOperation(address); // Execute feed operation
            disableRS485(reDePin);

            // Simulate a response
            uint8_t responseStatus = FEEDER_STATUS_OK; // Example response status
            switch (responseStatus) {
                case FEEDER_STATUS_OK:
                    Serial.println("OK");
                    debugPrint("M100: Feed operation completed successfully.");
                    break;
                case FEEDER_STATUS_FAULT:
                    Serial.println("FAULT");
                    debugPrint("M100: Feed operation encountered a fault.");
                    break;
                default:
                    Serial.println("UNKNOWN");
                    debugPrint("M100: Unknown feed operation status.");
                    break;
            }
        } else {
            Serial.println("Invalid M100 command. Use M100 F<bank><address>.");
            debugPrint("M100: Invalid command syntax.");
        }
    } else {
        Serial.println("Unknown G-code command.");
        debugPrint("Unknown G-code command received.");
    }
}

