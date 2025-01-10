#include "FeederProtocol.h"

bool validatePacket(const FeederPacket &packet) {
    return packet.checksum == calculateChecksum(packet.data, packet.length);
}

uint8_t calculateChecksum(const uint8_t *data, uint8_t length) {
    uint8_t checksum = 0;
    for (uint8_t i = 0; i < length; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

bool parsePacket(uint8_t *buffer, uint8_t bufferSize, FeederPacket &packet) {
    if (bufferSize < 4) return false; // Minimum packet size: header + command + length + checksum
    if (buffer[0] != FEEDER_HEADER) return false;

    packet.header = buffer[0];
    packet.command = buffer[1];
    packet.length = buffer[2];

    if (packet.length + 4 > bufferSize) return false; // Validate length
    for (uint8_t i = 0; i < packet.length; i++) {
        packet.data[i] = buffer[3 + i];
    }
    packet.checksum = buffer[3 + packet.length];

    return validatePacket(packet);
}

void buildPacket(FeederPacket &packet, uint8_t command, const uint8_t *data, uint8_t dataLength) {
    packet.header = FEEDER_HEADER;
    packet.command = command;
    packet.length = dataLength;

    for (uint8_t i = 0; i < dataLength; i++) {
        packet.data[i] = data[i];
    }

    packet.checksum = calculateChecksum(data, dataLength);
}
