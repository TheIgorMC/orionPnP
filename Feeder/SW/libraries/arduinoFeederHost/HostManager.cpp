#include "HostManager.h"

HostManager::HostManager(uint8_t rxPin1, uint8_t txPin1, uint8_t rxPin2, uint8_t txPin2)
    : bus1(rxPin1, txPin1), bus2(rxPin2, txPin2) {}

void HostManager::begin(uint32_t baudRate1, uint32_t baudRate2) {
    bus1.begin(baudRate1);
    bus2.begin(baudRate2);
}

void HostManager::sendPacket(SoftwareSerial &bus, const FeederPacket &packet) {
    bus.write(&packet.header, sizeof(FeederPacket));
}

bool HostManager::receivePacket(SoftwareSerial &bus, FeederPacket &packet) {
    if (bus.available() >= sizeof(FeederPacket)) {
        uint8_t buffer[sizeof(FeederPacket)];
        bus.readBytes(buffer, sizeof(FeederPacket));
        parsePacket(buffer, packet);
        return validatePacket(packet);
    }
    return false;
}

void HostManager::startupPolling(uint16_t address) {
    FeederPacket packet;
    buildPacket(packet, FEEDER_HEADER_STARTUP, address);
    sendPacket(bus1, packet);
}

void HostManager::assignAddress(uint16_t newAddress) {
    FeederPacket packet;
    buildPacket(packet, FEEDER_HEADER_ASSIGN, newAddress);
    sendPacket(bus1, packet);
}

void HostManager::hotplugPolling() {
    FeederPacket packet;
    buildPacket(packet, FEEDER_HEADER_HOTPLUG, 0x0000);
    sendPacket(bus1, packet);
}

void HostManager::keepAlive(uint16_t address) {
    FeederPacket packet;
    buildPacket(packet, FEEDER_HEADER_KEEPALIVE, address);
    sendPacket(bus1, packet);
}

void HostManager::feedOperation(uint16_t address) {
    FeederPacket packet;
    buildPacket(packet, FEEDER_HEADER_FEED, address);
    sendPacket(bus1, packet);

    // Handle feed status responses
    FeederPacket response;
    while (receivePacket(bus1, response)) {
        if (response.header == FEEDER_STATUS_OK || response.header == FEEDER_STATUS_FAULT) {
            break; // Operation complete
        }
    }
}
