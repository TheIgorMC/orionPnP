// OrionProtocol.h
// enum and struct definitions for the Orion protocol
// This file is part of the OrionPnP project


// OrionCommand enum for command codes
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
