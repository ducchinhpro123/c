#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <raylib.h>

#define MAX_FILENAME 256
#define CHUNK_SIZE 262144 // 256KB chunks for LAN speed (was 32KB)
#define MAX_FILE_SIZE (500 * 1024 * 1024) // 500MB limit

// New Protocol Constants
#define PROTOCOL_MAGIC 0x46494C45  // "FILE" in hex
#define PROTOCOL_HEADER_SIZE 13    // magic(4) + pkt_len(4) + type(1) + payload_len(4)
#define MAX_PACKET_SIZE (CHUNK_SIZE + 1024)  // Chunk + metadata overhead

// Packet Types
typedef enum {
    PKT_TYPE_FILE_CHUNK = 0x01,
    PKT_TYPE_TEXT_MSG = 0x02,
    PKT_TYPE_CONTROL = 0x03
} PacketType;

// Receive State Machine
typedef enum {
    RECV_STATE_HEADER,   // Reading header
    RECV_STATE_PAYLOAD   // Reading payload
} RecvState;

// Packet Header Structure (13 bytes)
typedef struct __attribute__((packed)) {
    uint32_t magic;         // 0x46494C45
    uint32_t packet_length; // Total packet size
    uint8_t packet_type;    // PacketType enum
    uint32_t payload_length;// Payload size
} PacketHeader;

// File Chunk Payload Structure
typedef struct {
    uint16_t filename_length;
    char filename[MAX_FILENAME];
    uint64_t total_file_size;
    uint32_t chunk_index;
    uint32_t total_chunks;
    uint32_t chunk_data_length;
    unsigned char* chunk_data;  // Points to actual data
} FileChunkPayload;

// Packet Receiver State
typedef struct {
    RecvState state;
    PacketHeader header;
    uint32_t bytes_received;
    unsigned char* buffer;
    size_t buffer_capacity;
} PacketReceiver;

typedef struct {
    char filename[MAX_FILENAME];
    size_t total_size;
    size_t bytes_received;
    int total_chunks;
    int chunks_received;
    FILE* file_handle; // File handle for streaming writes
    bool complete;
} FileTransfer;

// NEW PROTOCOL FUNCTIONS (Length-Prefixed Binary)
// ================================================

// Packet Creation
unsigned char* create_file_chunk_packet(const char* filename, uint64_t total_size,
    uint32_t chunk_index, uint32_t total_chunks,
    const unsigned char* chunk_data, uint32_t chunk_size,
    size_t* packet_size);

// Packet Receiver Management
PacketReceiver* create_packet_receiver();
void free_packet_receiver(PacketReceiver* receiver);
void reset_packet_receiver(PacketReceiver* receiver);

// Packet Reception (State Machine)
typedef enum {
    PARSE_RESULT_NEED_MORE_DATA,
    PARSE_RESULT_COMPLETE_PACKET,
    PARSE_RESULT_ERROR
} ParseResult;

ParseResult feed_packet_receiver(PacketReceiver* receiver, const unsigned char* data, size_t data_len);
bool parse_file_chunk_packet(const unsigned char* packet_data, size_t packet_len, FileChunkPayload* payload);

// Utility
uint32_t htonl_custom(uint32_t hostlong);
uint32_t ntohl_custom(uint32_t netlong);
uint16_t htons_custom(uint16_t hostshort);
uint16_t ntohs_custom(uint16_t netshort);

// ACK/NACK Control Packets
unsigned char* create_ack_packet(uint32_t chunk_index, size_t* packet_size);
bool parse_ack_packet(const unsigned char* packet_data, size_t packet_len, uint32_t* chunk_index);

// LEGACY PROTOCOL FUNCTIONS (Delimiter-Based - DEPRECATED)
// =========================================================

// Encoding/Decoding
char* base64_encode(const unsigned char* data, size_t input_length, size_t* output_length);
unsigned char* base64_decode(const char* data, size_t input_length, size_t* output_length);

// Protocol functions (DEPRECATED - use new protocol)
char* create_file_packet(const char* filename, size_t filesize,
    int chunk_index, int total_chunks,
    const unsigned char* chunk_data, size_t chunk_size);
bool parse_file_packet(const char* packet, FileTransfer* ft, unsigned char** chunk_data, size_t* chunk_size);

// COMMON FUNCTIONS (Used by both protocols)
// ==========================================

// File operations
bool read_file_to_buffer(const char* filepath, unsigned char** buffer, size_t* size);
bool write_buffer_to_file(const char* filepath, const unsigned char* buffer, size_t size);

// Streaming file operations
bool stream_file_chunk(const char* filepath, size_t offset, unsigned char* buffer, size_t* chunk_size);
bool write_chunk_to_file(FILE* file_handle, const unsigned char* chunk_data, size_t chunk_size);

// Transfer management
FileTransfer* create_file_transfer(const char* filename, size_t total_size, int total_chunks);
FileTransfer* create_streaming_transfer(const char* filepath, size_t total_size, int total_chunks);
bool add_chunk_to_streaming_transfer(FileTransfer* ft, int chunk_index, const unsigned char* data, size_t size);
void free_file_transfer(FileTransfer* ft);
void close_streaming_transfer(FileTransfer* ft, bool success);

bool ensure_dir_exist(const char* dirpath);
#endif
