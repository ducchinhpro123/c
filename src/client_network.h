#ifndef CLIENT_NETWORK_H
#define CLIENT_NETWORK_H

// #include "platform.h"
#include <stdbool.h>
#include <sys/types.h>

#include <stdint.h>

// Packet Types
#define PACKET_TYPE_TEXT       0
#define PACKET_TYPE_FILE_START 1
#define PACKET_TYPE_FILE_CHUNK 2
#define PACKET_TYPE_FILE_END   3
#define PACKET_TYPE_FILE_ABORT 4

#pragma pack(push, 1)
typedef struct {
    uint8_t type;
    uint32_t length; // Network Byte Order
} PacketHeader;
#pragma pack(pop)

#include <pthread.h>
#include "packet_queue.h"

typedef struct {
    int socket_fd;
    bool connected;
    char username[256];
    PacketQueue queue;
    pthread_t sender_thread;
} ClientConnection;

// Initialize connection
void init_client_connection(ClientConnection* conn);

int connect_to_server(ClientConnection* conn, const char* host, const char* port, const char* username);

// Send a text message (wraps it in PACKET_TYPE_TEXT)
int send_msg(ClientConnection* conn, const char* msg);

// Send a generic packet
int send_packet(ClientConnection* conn, uint8_t type, const void* data, uint32_t length);

int recv_msg(ClientConnection* conn, char* buffer, int size);

void disconnect_from_server(ClientConnection* conn);

#endif
