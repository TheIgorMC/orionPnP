#ifndef FEEDER_PROTOCOL_H
#define FEEDER_PROTOCOL_H

#include <stdint.h>

// Protocol constants
#define FEEDER_HEADER_STARTUP 0x01
#define FEEDER_HEADER_ASSIGN  0x02
#define FEEDER_HEADER_HOTPLUG 0x03
#define FEEDER_HEADER_KEEPALIVE 0x04
#define FEEDER_HEADER_FEED    0x05

#define FEEDER_STATUS_OK      0x00
#define FEEDER_STATUS_ACK     0x01
#define FEEDER_STATUS_FEEDING 0x02
#define FEEDER_STATUS_FAULT   0xFF

#define FEEDER_MAX_PACKET_SIZE 8 // Header (1) + Address (2) + Checksum (1)

// Packet structure
typedef struct {
    uint8_t header;
    uint16_t address;
    uint8_t checksum;
} FeederPacket;

// Function declarations
bool validatePacket(const FeederPacket &packet);
uint8_t calculateChecksum(const FeederPacket &packet);
void buildPacket(FeederPacket &packet, uint8_t header, uint16_t address);
void parsePacket(const uint8_t *buffer, FeederPacket &packet);

#endif
