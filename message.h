#ifndef MESSAGE_H
#define MESSAGE_H

#include <pthread.h>
#include <stdbool.h>

#define MSG_BUFFER 2048
#define MAX_MESSAGES 200

typedef struct {
    char text[MSG_BUFFER];
    char sender[256];
    double timestamp;
} Message;

typedef struct {
    Message messages[MAX_MESSAGES];
    int count;
    pthread_mutex_t mutex;
} MessageQueue;

void init_message_queue(MessageQueue* queue);
void add_message(MessageQueue* queue, const char* sender, const char* text);
Message get_message(MessageQueue* queue, int index);
int get_message_count(MessageQueue* queue);

#endif
