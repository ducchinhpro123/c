/*
 * Sliding Window Protocol Implementation
 */

#include "sliding_window.h"
#include <raylib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

// Get current time in seconds
static double get_time_seconds() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

SlidingWindow* create_sliding_window(uint32_t total_chunks) {
    SlidingWindow* window = malloc(sizeof(SlidingWindow));
    if (!window) {
        TraceLog(LOG_ERROR, "Failed to allocate SlidingWindow");
        return NULL;
    }

    window->chunks = calloc(total_chunks, sizeof(ChunkInfo));
    if (!window->chunks) {
        TraceLog(LOG_ERROR, "Failed to allocate chunk array");
        free(window);
        return NULL;
    }

    // Initialize chunk states
    for (uint32_t i = 0; i < total_chunks; i++) {
        window->chunks[i].chunk_index = i;
        window->chunks[i].state = CHUNK_STATE_PENDING;
        window->chunks[i].retry_count = 0;
        window->chunks[i].last_send_time = 0.0;
        window->chunks[i].packet_data = NULL;
        window->chunks[i].packet_size = 0;
    }

    window->total_chunks = total_chunks;
    window->window_start = 0;
    window->next_to_send = 0;
    window->chunks_acked = 0;
    window->complete = false;

    TraceLog(LOG_INFO, "Created sliding window: %u chunks, window size %d",
             total_chunks, WINDOW_SIZE);

    return window;
}

void free_sliding_window(SlidingWindow* window) {
    if (window) {
        if (window->chunks) {
            // Free stored packets
            for (uint32_t i = 0; i < window->total_chunks; i++) {
                if (window->chunks[i].packet_data) {
                    free(window->chunks[i].packet_data);
                }
            }
            free(window->chunks);
        }
        free(window);
    }
}

bool window_can_send(SlidingWindow* window) {
    if (!window || window->complete) {
        return false;
    }

    // Can send if:
    // 1. There are chunks left to send
    // 2. Window is not full (unacked < WINDOW_SIZE)
    if (window->next_to_send >= window->total_chunks) {
        return false;
    }

    uint32_t unacked = window_get_unacked_count(window);
    return unacked < WINDOW_SIZE;
}

uint32_t window_get_next_chunk(SlidingWindow* window) {
    if (!window || window->next_to_send >= window->total_chunks) {
        return (uint32_t)-1;
    }
    return window->next_to_send;
}

void window_mark_sent(SlidingWindow* window, uint32_t chunk_index,
                      const unsigned char* packet, size_t packet_size) {
    if (!window || chunk_index >= window->total_chunks) {
        return;
    }

    ChunkInfo* chunk = &window->chunks[chunk_index];
    chunk->state = CHUNK_STATE_SENT;
    chunk->last_send_time = get_time_seconds();

    // Store packet for potential retransmission
    if (chunk->packet_data) {
        free(chunk->packet_data);
    }
    chunk->packet_data = malloc(packet_size);
    if (chunk->packet_data) {
        memcpy(chunk->packet_data, packet, packet_size);
        chunk->packet_size = packet_size;
    }

    // Advance next_to_send if this was the next chunk
    if (chunk_index == window->next_to_send) {
        window->next_to_send++;
    }

    TraceLog(LOG_DEBUG, "Chunk %u marked as sent (attempt %u)",
             chunk_index, chunk->retry_count + 1);
}

void window_mark_acked(SlidingWindow* window, uint32_t chunk_index) {
    if (!window || chunk_index >= window->total_chunks) {
        return;
    }

    ChunkInfo* chunk = &window->chunks[chunk_index];
    
    if (chunk->state != CHUNK_STATE_ACKED) {
        chunk->state = CHUNK_STATE_ACKED;
        window->chunks_acked++;

        // Free packet data (no longer needed)
        if (chunk->packet_data) {
            free(chunk->packet_data);
            chunk->packet_data = NULL;
            chunk->packet_size = 0;
        }

        TraceLog(LOG_INFO, "Chunk %u ACKed (%u/%u complete)",
                 chunk_index, window->chunks_acked, window->total_chunks);

        // Slide window forward if this was the window start
        if (chunk_index == window->window_start) {
            while (window->window_start < window->total_chunks &&
                   window->chunks[window->window_start].state == CHUNK_STATE_ACKED) {
                window->window_start++;
            }
        }

        // Check completion
        if (window->chunks_acked == window->total_chunks) {
            window->complete = true;
            TraceLog(LOG_INFO, "All chunks acknowledged - transfer complete!");
        }
    }
}

bool window_needs_retransmit(SlidingWindow* window, uint32_t chunk_index) {
    if (!window || chunk_index >= window->total_chunks) {
        return false;
    }

    ChunkInfo* chunk = &window->chunks[chunk_index];
    
    if (chunk->state != CHUNK_STATE_SENT) {
        return false;
    }

    double current_time = get_time_seconds();
    double elapsed = current_time - chunk->last_send_time;

    return elapsed >= RETRANSMIT_TIMEOUT;
}

uint32_t window_get_unacked_count(SlidingWindow* window) {
    if (!window) {
        return 0;
    }

    uint32_t count = 0;
    for (uint32_t i = window->window_start; i < window->next_to_send; i++) {
        if (window->chunks[i].state == CHUNK_STATE_SENT) {
            count++;
        }
    }
    return count;
}

bool window_is_complete(SlidingWindow* window) {
    return window && window->complete;
}

float window_get_progress(SlidingWindow* window) {
    if (!window || window->total_chunks == 0) {
        return 0.0f;
    }
    return (float)window->chunks_acked / (float)window->total_chunks;
}

bool window_check_timeouts(SlidingWindow* window, RetransmitInfo* retransmit) {
    if (!window || !retransmit) {
        return false;
    }

    double current_time = get_time_seconds();

    // Check all sent chunks for timeout
    for (uint32_t i = window->window_start; i < window->next_to_send; i++) {
        ChunkInfo* chunk = &window->chunks[i];
        
        if (chunk->state == CHUNK_STATE_SENT) {
            double elapsed = current_time - chunk->last_send_time;
            
            if (elapsed >= RETRANSMIT_TIMEOUT) {
                // Check retry limit
                if (chunk->retry_count >= MAX_RETRIES) {
                    TraceLog(LOG_ERROR, "Chunk %u failed after %u retries",
                             i, chunk->retry_count);
                    chunk->state = CHUNK_STATE_FAILED;
                    continue;
                }

                // Need retransmission
                if (chunk->packet_data) {
                    retransmit->chunk_index = i;
                    retransmit->packet_data = chunk->packet_data;
                    retransmit->packet_size = chunk->packet_size;
                    
                    chunk->retry_count++;
                    chunk->last_send_time = current_time;
                    
                    TraceLog(LOG_WARNING, "Retransmitting chunk %u (attempt %u/%u)",
                             i, chunk->retry_count + 1, MAX_RETRIES + 1);
                    
                    return true;
                }
            }
        }
    }

    return false;
}
