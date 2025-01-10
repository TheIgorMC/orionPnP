#ifndef HOST_UTILITIES_H
#define HOST_UTILITIES_H

#include <Arduino.h>

// Function declarations
void enableRS485(uint8_t enablePin);
void disableRS485(uint8_t enablePin);
void debugMessage(const char *msg);
void setFaultLED(bool state);
void handleGCode(const String &gcode, HostManager &host);

#endif
