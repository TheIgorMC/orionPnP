#include "HostManager.h"

HostManager::HostManager(uint8_t rxPin1, uint8_t txPin1, uint8_t rxPin2, uint8_t txPin2)
    : bus1(rxPin1, txPin1), bus2(rxPin2, txPin2) {}

void HostManager::begin(uint32_t baudRate1, uint32_t baudRate2) {
    bus1.begin(baudRate1);
    bus2.begin(baudRate2);
}

void HostManager::sendCommand(uint8_t bus, uint8_t command, const uint8_t *data, uint8_t length) {
    FeederPacket packet;
    buildPacket(packet, command, data, length);

    SoftwareSerial *selectedBus = (bus == 1) ? &bus1 : &bus2;
    selectedBus->write((uint8_t *)&packet, sizeof(FeederPacket));
}

bool HostManager::receivePacket(uint8_t bus, FeederPacket &packet) {
    SoftwareSerial *selectedBus = (bus == 1) ? &bus1 : &bus2;
    if (selectedBus->available() > 0) {
        uint8_t buffer[FEEDER_MAX_PACKET_SIZE];
        size_t bytesRead = selectedBus->readBytes(buffer, FEEDER_MAX_PACKET_SIZE);
        return parsePacket(buffer, bytesRead, packet);
    }
    return false;
}
