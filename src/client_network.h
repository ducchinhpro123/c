#ifndef CLIENT_NETWORK_H
#define CLIENT_NETWORK_H

#include "platform.h"
#include "protocol.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#ifndef _WIN32
#include <sys/types.h>
#endif

#include "packet_queue.h"
#include <pthread.h>

typedef struct ClientConnection {
    int socket_fd;
    atomic_bool connected;
    bool sender_thread_started;
    char username[PROTOCOL_USERNAME_MAX_LEN + 1];
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
