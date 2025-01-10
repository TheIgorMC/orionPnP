#ifndef HOST_UTILITIES_H
#define HOST_UTILITIES_H

#include <Arduino.h>
#include "HostManager.h"

// Enable or disable debugging by uncommenting or commenting the following line:
#define DEBUG

// Debugging helper function
void debugPrint(const char *msg);
void enableRS485(uint8_t enablePin);
void disableRS485(uint8_t enablePin);
void setFaultLED(bool state);
void handleGCode(const String &gcode, HostManager &host);

#endif
