#include "HostManager.h"
#include "PinoutConfig.h"
#include "HostUtilities.h"

HostManager host(BUS1_RO_PIN, BUS1_DI_PIN, BUS2_RO_PIN, BUS2_DI_PIN);

void setup() {
    Serial.begin(115200); // Serial for G-code communication
#ifdef DEBUG
    Serial.println("RS485 Feeder Host - Debug Mode Enabled");
#endif

    pinMode(BUS1_RE_DE_PIN, OUTPUT);
    pinMode(BUS2_RE_DE_PIN, OUTPUT);
    pinMode(FAULT_LED_PIN, OUTPUT);

    host.begin(38400, 38400); // Set RS485 buses to higher baud rate
    debugPrint("RS485 buses initialized.");
}

void loop() {
    if (Serial.available() > 0) {
        String gcode = Serial.readStringUntil('\n');
        gcode.trim();
        debugPrint("G-code received:");
        debugPrint(gcode.c_str());
        handleGCode(gcode, host);
    }
}
