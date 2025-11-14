/*
 * Sliding Window Protocol for Reliable File Transfer
 * Implements ACK/NACK mechanism with retransmission
 */

#ifndef SLIDING_WINDOW_H
#define SLIDING_WINDOW_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define WINDOW_SIZE 10          // Number of unacknowledged chunks allowed
#define RETRANSMIT_TIMEOUT 2.0  // Seconds before retransmission
#define MAX_RETRIES 5           // Maximum retransmission attempts

// Chunk state tracking
typedef enum {
    CHUNK_STATE_PENDING,     // Not sent yet
    CHUNK_STATE_SENT,        // Sent, waiting for ACK
    CHUNK_STATE_ACKED,       // Acknowledged
    CHUNK_STATE_FAILED       // Failed after max retries
} ChunkState;

// Individual chunk tracking
typedef struct {
    uint32_t chunk_index;
    ChunkState state;
    uint32_t retry_count;
    double last_send_time;
    unsigned char* packet_data;  // Pre-built packet
    size_t packet_size;
} ChunkInfo;

// Sliding window manager
typedef struct {
    ChunkInfo* chunks;
    uint32_t total_chunks;
    uint32_t window_start;       // First unacknowledged chunk
    uint32_t next_to_send;       // Next chunk to send
    uint32_t chunks_acked;       // Total acknowledged
    bool complete;
} SlidingWindow;

// Window management
SlidingWindow* create_sliding_window(uint32_t total_chunks);
void free_sliding_window(SlidingWindow* window);

// Chunk operations
bool window_can_send(SlidingWindow* window);
uint32_t window_get_next_chunk(SlidingWindow* window);
void window_mark_sent(SlidingWindow* window, uint32_t chunk_index, 
                      const unsigned char* packet, size_t packet_size);
void window_mark_acked(SlidingWindow* window, uint32_t chunk_index);
bool window_needs_retransmit(SlidingWindow* window, uint32_t chunk_index);

// Window state
uint32_t window_get_unacked_count(SlidingWindow* window);
bool window_is_complete(SlidingWindow* window);
float window_get_progress(SlidingWindow* window);

// Retransmission logic
typedef struct {
    uint32_t chunk_index;
    unsigned char* packet_data;
    size_t packet_size;
} RetransmitInfo;

bool window_check_timeouts(SlidingWindow* window, RetransmitInfo* retransmit);

#endif // SLIDING_WINDOW_H
