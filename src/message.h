#ifndef MESSAGE_H
#define MESSAGE_H

#include <stdbool.h>

#ifdef _WIN32
    #include <windows.h>
    // Define a type that's large enough to hold a CRITICAL_SECTION
    typedef struct {
        char data[64];  // Size should accommodate CRITICAL_SECTION (platform-dependent)
    } pthread_mutex_t;
#else
    #include <pthread.h>
#endif

#define MSG_BUFFER 1500000  // Must match BUFFER_SIZE in server.h for file packets
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
