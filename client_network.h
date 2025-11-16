#ifndef CLIENT_NETWORK_H
#define CLIENT_NETWORK_H

// #include "platform.h"
#include <stdbool.h>
#include <sys/types.h>

typedef struct {
    int socket_fd;
    bool connected;
    char username[256];
} ClientConnection;

// Initialize connection
void init_client_connection(ClientConnection* conn);

int connect_to_server(ClientConnection* conn, const char* host, const char* port, const char* username);

int send_msg(ClientConnection* conn, const char* msg);

int recv_msg(ClientConnection* conn, char* buffer, int size);

void disconnect_from_server(ClientConnection* conn);

#endif
