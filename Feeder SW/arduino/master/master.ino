#include "HostManager.h"
#include "PinoutConfig.h"
#include "HostUtilities.h"

// Create an instance of HostManager
HostManager host(BUS1_RO_PIN, BUS1_DI_PIN, BUS2_RO_PIN, BUS2_DI_PIN);

void setup() {
    // Initialize Serial Monitor for G-code
    Serial.begin(115200); // G-code communication
    #ifdef DEBUG
    Serial.println("RS485 Feeder Host - Starting...");
    #endif

    // Set RS485 enable pins and Fault LED pin as outputs
    pinMode(BUS1_RE_DE_PIN, OUTPUT);
    pinMode(BUS2_RE_DE_PIN, OUTPUT);
    pinMode(FAULT_LED_PIN, OUTPUT);

    // Disable RS485 buses initially
    disableRS485(BUS1_RE_DE_PIN);
    disableRS485(BUS2_RE_DE_PIN);

    // Turn off Fault LED initially
    setFaultLED(false);

    // Initialize RS485 buses
    host.begin(38400, 38400); // Set RS485 buses to a higher baud rate
}

void loop() {
    // Listen for G-code commands from the serial port
    if (Serial.available() > 0) {
        String gcode = Serial.readStringUntil('\n'); // Read until newline
        gcode.trim(); // Remove any extra whitespace
        handleGCode(gcode, host);
    }

    // Add delays or additional tasks as needed
}
