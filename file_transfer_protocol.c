/*
 * New Binary Protocol Implementation
 * Length-prefixed, binary-safe file transfer protocol
 */

#include "file_transfer.h"
#include <arpa/inet.h>
#include <string.h>

// ============================================================================
// UTILITY FUNCTIONS - Byte Order Conversion
// ============================================================================

uint32_t htonl_custom(uint32_t hostlong) {
    return htonl(hostlong);
}

uint32_t ntohl_custom(uint32_t netlong) {
    return ntohl(netlong);
}

uint16_t htons_custom(uint16_t hostshort) {
    return htons(hostshort);
}

uint16_t ntohs_custom(uint16_t netshort) {
    return ntohs(netshort);
}

// ============================================================================
// PACKET CREATION
// ============================================================================

unsigned char* create_file_chunk_packet(
    const char* filename,
    uint64_t total_size,
    uint32_t chunk_index,
    uint32_t total_chunks,
    const unsigned char* chunk_data,
    uint32_t chunk_size,
    size_t* packet_size)
{
    if (!filename || !chunk_data || !packet_size) {
        TraceLog(LOG_ERROR, "Invalid parameters to create_file_chunk_packet");
        return NULL;
    }

    uint16_t filename_len = strlen(filename);
    if (filename_len >= MAX_FILENAME) {
        TraceLog(LOG_ERROR, "Filename too long: %s", filename);
        return NULL;
    }

    // Calculate sizes
    // Header: magic(4) + pkt_len(4) + type(1) + payload_len(4) = 13 bytes
    // Payload: fname_len(2) + fname(N) + total_size(8) + chunk_idx(4) + 
    //          total_chunks(4) + chunk_len(4) + chunk_data(M)
    
    size_t payload_size = 2 + filename_len + 8 + 4 + 4 + 4 + chunk_size;
    size_t total_packet_size = PROTOCOL_HEADER_SIZE + payload_size;
    
    if (total_packet_size > MAX_PACKET_SIZE + PROTOCOL_HEADER_SIZE) {
        TraceLog(LOG_ERROR, "Packet too large: %zu bytes", total_packet_size);
        return NULL;
    }

    // Allocate packet buffer
    unsigned char* packet = malloc(total_packet_size);
    if (!packet) {
        TraceLog(LOG_ERROR, "Failed to allocate packet buffer");
        return NULL;
    }

    unsigned char* ptr = packet;

    // Write header
    uint32_t magic = htonl_custom(PROTOCOL_MAGIC);
    memcpy(ptr, &magic, 4);
    ptr += 4;

    uint32_t pkt_len = htonl_custom((uint32_t)total_packet_size);
    memcpy(ptr, &pkt_len, 4);
    ptr += 4;

    *ptr++ = PKT_TYPE_FILE_CHUNK;

    uint32_t payload_len = htonl_custom((uint32_t)payload_size);
    memcpy(ptr, &payload_len, 4);
    ptr += 4;

    // Write payload
    uint16_t fname_len_net = htons_custom(filename_len);
    memcpy(ptr, &fname_len_net, 2);
    ptr += 2;

    memcpy(ptr, filename, filename_len);
    ptr += filename_len;

    uint64_t total_size_net = htonl_custom((uint32_t)(total_size >> 32));
    memcpy(ptr, &total_size_net, 4);
    ptr += 4;
    total_size_net = htonl_custom((uint32_t)(total_size & 0xFFFFFFFF));
    memcpy(ptr, &total_size_net, 4);
    ptr += 4;

    uint32_t chunk_idx_net = htonl_custom(chunk_index);
    memcpy(ptr, &chunk_idx_net, 4);
    ptr += 4;

    uint32_t total_chunks_net = htonl_custom(total_chunks);
    memcpy(ptr, &total_chunks_net, 4);
    ptr += 4;

    uint32_t chunk_size_net = htonl_custom(chunk_size);
    memcpy(ptr, &chunk_size_net, 4);
    ptr += 4;

    memcpy(ptr, chunk_data, chunk_size);
    ptr += chunk_size;

    *packet_size = total_packet_size;
    
    TraceLog(LOG_DEBUG, "Created packet: %s chunk %u/%u (%u bytes, total packet: %zu)",
             filename, chunk_index, total_chunks, chunk_size, total_packet_size);
    
    return packet;
}

// ============================================================================
// PACKET RECEIVER MANAGEMENT
// ============================================================================

PacketReceiver* create_packet_receiver() {
    PacketReceiver* receiver = malloc(sizeof(PacketReceiver));
    if (!receiver) {
        TraceLog(LOG_ERROR, "Failed to allocate PacketReceiver");
        return NULL;
    }

    receiver->state = RECV_STATE_HEADER;
    receiver->bytes_received = 0;
    receiver->buffer = malloc(MAX_PACKET_SIZE + PROTOCOL_HEADER_SIZE);
    receiver->buffer_capacity = MAX_PACKET_SIZE + PROTOCOL_HEADER_SIZE;
    
    if (!receiver->buffer) {
        TraceLog(LOG_ERROR, "Failed to allocate receiver buffer");
        free(receiver);
        return NULL;
    }

    memset(&receiver->header, 0, sizeof(PacketHeader));
    
    TraceLog(LOG_INFO, "Created packet receiver with %zu byte buffer", 
             receiver->buffer_capacity);
    
    return receiver;
}

void free_packet_receiver(PacketReceiver* receiver) {
    if (receiver) {
        if (receiver->buffer) {
            free(receiver->buffer);
        }
        free(receiver);
    }
}

void reset_packet_receiver(PacketReceiver* receiver) {
    if (receiver) {
        receiver->state = RECV_STATE_HEADER;
        receiver->bytes_received = 0;
        memset(&receiver->header, 0, sizeof(PacketHeader));
    }
}

// ============================================================================
// PACKET RECEPTION STATE MACHINE
// ============================================================================

ParseResult feed_packet_receiver(PacketReceiver* receiver, 
                                 const unsigned char* data, 
                                 size_t data_len) 
{
    if (!receiver || !data || data_len == 0) {
        return PARSE_RESULT_ERROR;
    }

    size_t bytes_consumed = 0;

    while (bytes_consumed < data_len) {
        if (receiver->state == RECV_STATE_HEADER) {
            // Receiving header (13 bytes)
            size_t needed = PROTOCOL_HEADER_SIZE - receiver->bytes_received;
            size_t available = data_len - bytes_consumed;
            size_t to_copy = (needed < available) ? needed : available;

            memcpy(receiver->buffer + receiver->bytes_received, 
                   data + bytes_consumed, to_copy);
            receiver->bytes_received += to_copy;
            bytes_consumed += to_copy;

            if (receiver->bytes_received == PROTOCOL_HEADER_SIZE) {
                // Parse header
                unsigned char* ptr = receiver->buffer;
                
                uint32_t magic;
                memcpy(&magic, ptr, 4);
                magic = ntohl_custom(magic);
                ptr += 4;

                if (magic != PROTOCOL_MAGIC) {
                    TraceLog(LOG_ERROR, "Invalid magic number: 0x%08X", magic);
                    reset_packet_receiver(receiver);
                    return PARSE_RESULT_ERROR;
                }

                receiver->header.magic = magic;

                memcpy(&receiver->header.packet_length, ptr, 4);
                receiver->header.packet_length = ntohl_custom(receiver->header.packet_length);
                ptr += 4;

                receiver->header.packet_type = *ptr++;

                memcpy(&receiver->header.payload_length, ptr, 4);
                receiver->header.payload_length = ntohl_custom(receiver->header.payload_length);

                // Validate packet size
                if (receiver->header.packet_length > receiver->buffer_capacity) {
                    TraceLog(LOG_ERROR, "Packet too large: %u bytes", 
                             receiver->header.packet_length);
                    reset_packet_receiver(receiver);
                    return PARSE_RESULT_ERROR;
                }

                // Transition to payload state
                receiver->state = RECV_STATE_PAYLOAD;
                receiver->bytes_received = 0;

                TraceLog(LOG_DEBUG, "Header received: type=%u, payload=%u bytes",
                         receiver->header.packet_type, receiver->header.payload_length);
            }
        }
        else if (receiver->state == RECV_STATE_PAYLOAD) {
            // Receiving payload
            size_t needed = receiver->header.payload_length - receiver->bytes_received;
            size_t available = data_len - bytes_consumed;
            size_t to_copy = (needed < available) ? needed : available;

            memcpy(receiver->buffer + PROTOCOL_HEADER_SIZE + receiver->bytes_received,
                   data + bytes_consumed, to_copy);
            receiver->bytes_received += to_copy;
            bytes_consumed += to_copy;

            if (receiver->bytes_received == receiver->header.payload_length) {
                // Complete packet received!
                TraceLog(LOG_DEBUG, "Complete packet received: %u bytes",
                         receiver->header.packet_length);
                return PARSE_RESULT_COMPLETE_PACKET;
            }
        }
    }

    return PARSE_RESULT_NEED_MORE_DATA;
}

// ============================================================================
// PACKET PARSING
// ============================================================================

bool parse_file_chunk_packet(const unsigned char* packet_data,
                             size_t packet_len,
                             FileChunkPayload* payload)
{
    if (!packet_data || !payload || packet_len < PROTOCOL_HEADER_SIZE) {
        TraceLog(LOG_ERROR, "Invalid parameters to parse_file_chunk_packet");
        return false;
    }

    // Skip header, parse payload
    const unsigned char* ptr = packet_data + PROTOCOL_HEADER_SIZE;
    size_t remaining = packet_len - PROTOCOL_HEADER_SIZE;

    if (remaining < 2) {
        TraceLog(LOG_ERROR, "Packet too small for filename length");
        return false;
    }

    // Read filename length
    memcpy(&payload->filename_length, ptr, 2);
    payload->filename_length = ntohs_custom(payload->filename_length);
    ptr += 2;
    remaining -= 2;

    if (payload->filename_length >= MAX_FILENAME || payload->filename_length > remaining) {
        TraceLog(LOG_ERROR, "Invalid filename length: %u", payload->filename_length);
        return false;
    }

    // Read filename
    memcpy(payload->filename, ptr, payload->filename_length);
    payload->filename[payload->filename_length] = '\0';
    ptr += payload->filename_length;
    remaining -= payload->filename_length;

    if (remaining < 8 + 4 + 4 + 4) {
        TraceLog(LOG_ERROR, "Packet too small for metadata");
        return false;
    }

    // Read total file size (64-bit)
    uint32_t high, low;
    memcpy(&high, ptr, 4);
    high = ntohl_custom(high);
    ptr += 4;
    memcpy(&low, ptr, 4);
    low = ntohl_custom(low);
    ptr += 4;
    payload->total_file_size = ((uint64_t)high << 32) | low;
    remaining -= 8;

    // Read chunk index
    memcpy(&payload->chunk_index, ptr, 4);
    payload->chunk_index = ntohl_custom(payload->chunk_index);
    ptr += 4;
    remaining -= 4;

    // Read total chunks
    memcpy(&payload->total_chunks, ptr, 4);
    payload->total_chunks = ntohl_custom(payload->total_chunks);
    ptr += 4;
    remaining -= 4;

    // Read chunk data length
    memcpy(&payload->chunk_data_length, ptr, 4);
    payload->chunk_data_length = ntohl_custom(payload->chunk_data_length);
    ptr += 4;
    remaining -= 4;

    if (payload->chunk_data_length != remaining) {
        TraceLog(LOG_ERROR, "Chunk data length mismatch: expected %u, got %zu",
                 payload->chunk_data_length, remaining);
        return false;
    }

    // Point to chunk data (don't copy, just reference)
    payload->chunk_data = (unsigned char*)ptr;

    TraceLog(LOG_INFO, "Parsed chunk: %s [%u/%u] %u bytes",
             payload->filename, payload->chunk_index, payload->total_chunks,
             payload->chunk_data_length);

    return true;
}

// ============================================================================
// ACK/NACK CONTROL PACKETS
// ============================================================================

unsigned char* create_ack_packet(uint32_t chunk_index, size_t* packet_size) {
    // ACK packet: header + 4-byte chunk_index
    size_t total_size = PROTOCOL_HEADER_SIZE + 4;
    unsigned char* packet = malloc(total_size);
    if (!packet) return NULL;

    unsigned char* ptr = packet;

    // Header
    uint32_t magic = htonl_custom(PROTOCOL_MAGIC);
    memcpy(ptr, &magic, 4);
    ptr += 4;

    uint32_t pkt_len = htonl_custom((uint32_t)total_size);
    memcpy(ptr, &pkt_len, 4);
    ptr += 4;

    *ptr++ = PKT_TYPE_CONTROL;

    uint32_t payload_len = htonl_custom(4);
    memcpy(ptr, &payload_len, 4);
    ptr += 4;

    // Payload: chunk_index
    uint32_t chunk_idx_net = htonl_custom(chunk_index);
    memcpy(ptr, &chunk_idx_net, 4);

    *packet_size = total_size;
    return packet;
}

bool parse_ack_packet(const unsigned char* packet_data, size_t packet_len, 
                     uint32_t* chunk_index) {
    if (!packet_data || !chunk_index || packet_len != PROTOCOL_HEADER_SIZE + 4) {
        return false;
    }

    const unsigned char* ptr = packet_data + PROTOCOL_HEADER_SIZE;
    memcpy(chunk_index, ptr, 4);
    *chunk_index = ntohl_custom(*chunk_index);
    return true;
}
