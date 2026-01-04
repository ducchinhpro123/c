#include "packet_queue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Initializes mutex and condition variable
void pq_init(PacketQueue* pq) {
    pq->head = NULL;
    pq->tail = NULL;
    pq->count = 0;
    pq->total_data_size = 0;
    pthread_mutex_init(&pq->mutex, NULL);
    pthread_cond_init(&pq->cond, NULL);
}

// Adds packet to queue, signals waiting thread
int pq_push(PacketQueue* pq, uint8_t type, const void* data, uint32_t length) {
    Packet* pkt = (Packet*)malloc(sizeof(Packet));
    if (!pkt) return -1;

    pkt->type = type;
    pkt->length = length;
    pkt->next = NULL;

    if (length > 0 && data != NULL) {
        pkt->data = malloc(length);
        if (!pkt->data) {
            free(pkt);
            return -1;
        }
        memcpy(pkt->data, data, length);
    } else {
        pkt->data = NULL;
    }

    pthread_mutex_lock(&pq->mutex);

    if (pq->tail) {
        pq->tail->next = pkt;
        pq->tail = pkt;
    } else {
        pq->head = pkt;
        pq->tail = pkt;
    }

    pq->count++;
    pq->total_data_size += length;

    pthread_cond_signal(&pq->cond);
    pthread_mutex_unlock(&pq->mutex);
    return 0;
}

Packet* pq_pop(PacketQueue* pq) {
    pthread_mutex_lock(&pq->mutex);

    while (pq->head == NULL) {
        pthread_cond_wait(&pq->cond, &pq->mutex);
    }

    Packet* pkt = pq->head;
    pq->head = pkt->next;

    if (pq->head == NULL) {
        pq->tail = NULL;
    }

    pq->count--;
    pq->total_data_size -= pkt->length;

    pthread_mutex_unlock(&pq->mutex);
    return pkt;
}

void pq_free_packet(Packet* pkt) {
    if (pkt) {
        if (pkt->data) {
            free(pkt->data);
        }
        free(pkt);
    }
}

size_t pq_get_data_size(PacketQueue* pq) {
    size_t size;
    pthread_mutex_lock(&pq->mutex);
    size = pq->total_data_size;
    pthread_mutex_unlock(&pq->mutex);
    return size;
}

size_t pq_get_data_size_unlocked(PacketQueue* pq) {
    return pq->total_data_size;
}

// Zero-copy push: takes ownership of data pointer (caller must have malloc'd it)
int pq_push_zero_copy(PacketQueue* pq, uint8_t type, void* data, uint32_t length) {
    Packet* pkt = (Packet*)malloc(sizeof(Packet));
    if (!pkt) return -1;

    pkt->type = type;
    pkt->length = length;
    pkt->next = NULL;
    pkt->data = data;  // Take ownership, no copy

    pthread_mutex_lock(&pq->mutex);

    if (pq->tail) {
        pq->tail->next = pkt;
        pq->tail = pkt;
    } else {
        pq->head = pkt;
        pq->tail = pkt;
    }

    pq->count++;
    pq->total_data_size += length;

    pthread_cond_signal(&pq->cond);
    pthread_mutex_unlock(&pq->mutex);
    return 0;
}
