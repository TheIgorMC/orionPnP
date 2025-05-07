#ifndef ORION_PROTOCOL_H
#define ORION_PROTOCOL_H

#include <Arduino.h>

// ==== Constants ====
#define ORION_START_BYTE 0xAA
#define ORION_HOST_ADDR 0xFFFF
#define ORION_PROTOCOL_VERSION 1
#define ORION_PACKET_SIZE 10
#define ORION_ACK_TIMEOUT_MS 40 
#define RE_POLL_INTERVAL 10 // every 10 main loops (≈5 seconds)

#ifdef ORION_IS_HOST
  #define ORION_CMD_QUEUE_SIZE 32
#else
  #define ORION_CMD_QUEUE_SIZE 8
#endif


// ==== Command Enum ====
typedef enum {
    CMD_ADDR_ASSIGN      = 0x01,
    CMD_ADDR_CONFIRM     = 0x02,
    CMD_POLL             = 0x03,
    
    CMD_FEED_FORWARD     = 0x10,
    CMD_FEED_BACKWARD    = 0x11,
    CMD_LOAD_TAPE        = 0x12,
    CMD_UNLOAD_TAPE      = 0x13,

    CMD_REQ_STATUS       = 0x20,
    CMD_ERROR_REPORT     = 0x21,

    CMD_ACK              = 0xA0,
    CMD_NACK             = 0xA1
} OrionCommand;

// ==== Acknowledgment Enum ====
typedef enum {
    ACK_GENERIC        = 0x0001,
    ACK_BUSY           = 0x0002,
    ACK_DONE           = 0x0003,
    ACK_QUEUED         = 0x0004,
    ACK_BUFFER_FULL    = 0x0005
} OrionAckPayload;

// ==== Packet Struct ====
typedef struct __attribute__((packed)) {
    uint8_t  startByte;
    uint16_t address;
    uint8_t  version;
    uint8_t  commandId;
    uint8_t  command;
    uint16_t payload;
    uint16_t crc;
} OrionPacket;


// ==== Queue Constants ====
typedef struct {
    OrionPacket queue[ORION_CMD_QUEUE_SIZE];
    uint8_t head;
    uint8_t tail;
} OrionQueue;

// ==== API ====
uint16_t OrionGetCRC(const OrionPacket& packet);
bool OrionCheckCRC(const OrionPacket& packet);

// ==== Packet Functions ====
bool OrionReceivePacket(Stream& serial, OrionPacket& packet);
void OrionSendPacket(Stream& serial, const OrionPacket& packet);
OrionPacket OrionMakePacket(uint16_t address, uint8_t commandId, OrionCommand cmd, uint16_t payload);

// ==== Queue Functions ====
void OrionQueueInit(OrionQueue& q);
bool OrionQueueIsFull(const OrionQueue& q);
bool OrionQueueIsEmpty(const OrionQueue& q);
bool OrionQueueEnqueue(OrionQueue& q, const OrionPacket& pkt);
bool OrionQueueDequeue(OrionQueue& q, OrionPacket& pkt);



#endif
