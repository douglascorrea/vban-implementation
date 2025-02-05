#ifndef VBAN4MAC_TYPES_H
#define VBAN4MAC_TYPES_H

#include <stdint.h>

// VBAN Protocol Constants
#define VBAN_HEADER_SIZE 28
#define VBAN_MAX_PACKET_SIZE 1436
#define VBAN_PROTOCOL_AUDIO 0x00
#define VBAN_DATATYPE_INT16 0x01
#define VBAN_DEFAULT_PORT 6980
#define VBAN_SAMPLE_RATE 48000
#define VBAN_SAMPLE_RATE_INDEX 3  // Index for 48kHz (corrected according to VBAN protocol spec)
#define VBAN_PROTOCOL_MAXNBS 256  // Maximum number of samples per packet

// VBAN Packet Header Structure
typedef struct __attribute__((packed)) {
    uint32_t vban;           // Contains 'VBAN' fourcc
    uint8_t format_SR;       // Sample rate index and sub protocol
    uint8_t format_nbs;      // Number of samples per frame (1-256)
    uint8_t format_nbc;      // Number of channels (1-256)
    uint8_t format_bit;      // Bit resolution and codec
    char streamname[16];     // Stream identifier
    uint32_t nuFrame;        // Frame counter
} vban_header_t;

#endif /* VBAN4MAC_TYPES_H */ 