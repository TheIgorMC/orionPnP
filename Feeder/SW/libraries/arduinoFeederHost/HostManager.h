#ifndef HOST_MANAGER_H
#define HOST_MANAGER_H

#include "FeederProtocol.h"
#include <SoftwareSerial.h>

class HostManager {
public:
    HostManager(uint8_t rxPin1, uint8_t txPin1, uint8_t rxPin2, uint8_t txPin2);
    void begin(uint32_t baudRate1, uint32_t baudRate2);
    
    // Protocol methods
    void startupPolling(uint16_t address);
    void assignAddress(uint16_t newAddress);
    void hotplugPolling();
    void keepAlive(uint16_t address);
    void feedOperation(uint16_t address);

private:
    SoftwareSerial bus1;
    SoftwareSerial bus2;

    void sendPacket(SoftwareSerial &bus, const FeederPacket &packet);
    bool receivePacket(SoftwareSerial &bus, FeederPacket &packet);
};

#endif
