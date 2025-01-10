#ifndef HOST_MANAGER_H
#define HOST_MANAGER_H

#include "FeederProtocol.h"
#include <SoftwareSerial.h>

class HostManager {
public:
    HostManager(uint8_t rxPin1, uint8_t txPin1, uint8_t rxPin2, uint8_t txPin2);
    void begin(uint32_t baudRate1, uint32_t baudRate2);
    void sendCommand(uint8_t bus, uint8_t command, const uint8_t *data, uint8_t length);
    bool receivePacket(uint8_t bus, FeederPacket &packet);

private:
    SoftwareSerial bus1;
    SoftwareSerial bus2;
};

#endif
