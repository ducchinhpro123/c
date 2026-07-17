#ifndef PACKET_QUEUE_H
#define PACKET_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct Packet {
    uint8_t type;
    uint32_t length;
    void* data;
    struct Packet* next;
} Packet;

typedef struct {
    Packet* head;
    Packet* tail;
    size_t count;
    size_t total_data_size; // Total bytes of data in queue (for throttling)
    bool closed;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PacketQueue;

void pq_init(PacketQueue* pq);
int pq_push(PacketQueue* pq, uint8_t type, const void* data, uint32_t length);
int pq_push_zero_copy(PacketQueue* pq, uint8_t type, void* data, uint32_t length); // Takes ownership of data
Packet* pq_pop(PacketQueue* pq); // Blocking pop
void pq_free_packet(Packet* pkt);
void pq_close(PacketQueue* pq);
void pq_reopen(PacketQueue* pq);
void pq_destroy(PacketQueue* pq);
size_t pq_get_data_size(PacketQueue* pq);
size_t pq_get_data_size_unlocked(PacketQueue* pq); // Call only when holding external lock

#endif // PACKET_QUEUE_H
