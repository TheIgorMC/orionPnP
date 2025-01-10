#include "FeederProtocol.h"

uint8_t calculateChecksum(const FeederPacket &packet) {
    return packet.header ^ (packet.address & 0xFF) ^ (packet.address >> 8);
}

bool validatePacket(const FeederPacket &packet) {
    return packet.checksum == calculateChecksum(packet);
}

void buildPacket(FeederPacket &packet, uint8_t header, uint16_t address) {
    packet.header = header;
    packet.address = address;
    packet.checksum = calculateChecksum(packet);
}

void parsePacket(const uint8_t *buffer, FeederPacket &packet) {
    packet.header = buffer[0];
    packet.address = (buffer[1] | (buffer[2] << 8));
    packet.checksum = buffer[3];
}
