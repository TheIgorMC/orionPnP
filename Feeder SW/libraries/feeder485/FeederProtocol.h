#ifndef FEEDER_PROTOCOL_H
#define FEEDER_PROTOCOL_H

#include <stdint.h>

// Define constants
#define FEEDER_HEADER 0xA5 // Example header byte
#define FEEDER_MAX_PACKET_SIZE 32

// Packet structure
typedef struct {
    uint8_t header;
    uint8_t command;
    uint8_t length;
    uint8_t data[FEEDER_MAX_PACKET_SIZE];
    uint8_t checksum;
} FeederPacket;

// Function declarations
bool validatePacket(const FeederPacket &packet);
uint8_t calculateChecksum(const uint8_t *data, uint8_t length);
bool parsePacket(uint8_t *buffer, uint8_t bufferSize, FeederPacket &packet);
void buildPacket(FeederPacket &packet, uint8_t command, const uint8_t *data, uint8_t dataLength);

#endif
