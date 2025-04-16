#include "OrionProtocol.h"

// ==== CRC16-CCITT ====
static uint16_t crc16_ccitt(const uint8_t* data, uint8_t len) {
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

// ==== CRC Functions ====
uint16_t OrionGetCRC(const OrionPacket& packet) {
    return crc16_ccitt((const uint8_t*)&packet, ORION_PACKET_SIZE - sizeof(packet.crc));
}

bool OrionCheckCRC(const OrionPacket& packet) {
    return OrionGetCRC(packet) == packet.crc;
}

// ==== Receive Packet ====
bool OrionReceivePacket(Stream& serial, OrionPacket& packet) {
    if (serial.available() < ORION_PACKET_SIZE) return false;

    serial.readBytes((uint8_t*)&packet, ORION_PACKET_SIZE);

    if (packet.startByte != ORION_START_BYTE) return false;
    if (packet.version != ORION_PROTOCOL_VERSION) return false;
    if (!OrionCheckCRC(packet)) return false;

    return true;
}

// ==== Send Packet ====
void OrionSendPacket(Stream& serial, const OrionPacket& packetIn) {
    OrionPacket packet = packetIn;
    packet.crc = OrionGetCRC(packet);
    serial.write((uint8_t*)&packet, ORION_PACKET_SIZE);
}

// ==== Make Packet ====
OrionPacket OrionMakePacket(uint16_t address, uint8_t commandId, OrionCommand cmd, uint16_t payload) {
    OrionPacket packet;
    packet.startByte = ORION_START_BYTE;
    packet.address   = address;
    packet.version   = ORION_PROTOCOL_VERSION;
    packet.commandId = commandId;
    packet.command   = cmd;
    packet.payload   = payload;
    packet.crc       = OrionGetCRC(packet);
    return packet;
}



// ==== Queue API ====
void OrionQueueInit(OrionQueue& q) {
    q.head = 0;
    q.tail = 0;
}

bool OrionQueueIsFull(const OrionQueue& q) {
    return ((q.tail + 1) % ORION_CMD_QUEUE_SIZE) == q.head;
}

bool OrionQueueIsEmpty(const OrionQueue& q) {
    return q.head == q.tail;
}

bool OrionQueueEnqueue(OrionQueue& q, const OrionPacket& pkt) {
    if (OrionQueueIsFull(q)) return false;
    q.queue[q.tail] = pkt;
    q.tail = (q.tail + 1) % ORION_CMD_QUEUE_SIZE;
    return true;
}

bool OrionQueueDequeue(OrionQueue& q, OrionPacket& pkt) {
    if (OrionQueueIsEmpty(q)) return false;
    pkt = q.queue[q.head];
    q.head = (q.head + 1) % ORION_CMD_QUEUE_SIZE;
    return true;
}
