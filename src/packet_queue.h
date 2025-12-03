#ifndef PACKET_QUEUE_H
#define PACKET_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

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
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PacketQueue;

void pq_init(PacketQueue* pq);
int pq_push(PacketQueue* pq, uint8_t type, const void* data, uint32_t length);
Packet* pq_pop(PacketQueue* pq); // Blocking pop
void pq_free_packet(Packet* pkt);
size_t pq_get_data_size(PacketQueue* pq);

#endif // PACKET_QUEUE_H
