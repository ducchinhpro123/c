#ifndef SERVER_H
#define SERVER_H

#include "platform.h"
#include "protocol.h"
#include <errno.h>
#include <raylib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_CLIENTS 32
#define PORT 8898
#define CLIENT_STREAM_BUFFER (PROTOCOL_MAX_PAYLOAD + (64u * 1024u) + sizeof(PacketHeader))

typedef void (*server_msg_cb)(const char* msg, const char* username);

typedef struct {
    int sock_fd;
    char username[PROTOCOL_USERNAME_MAX_LEN + 1];
    char ip_addr[16];
    char* recv_buffer;
    size_t recv_capacity;
    size_t recv_len;
    bool authenticated;
} Client;

typedef struct {
    char username[PROTOCOL_USERNAME_MAX_LEN + 1];
    char message[PROTOCOL_TEXT_MAX_LEN + 1];
    bool is_system;
} ChatMessage;

void server_set_msg_cb(server_msg_cb cb);
bool init_server(void);
void cleanup_server(void);
bool is_server_running(void);
int server_accept_client(void);
void server_recv_msgs(void);
void server_broadcast_msg(const char* msg, int sender_fd);
int get_client_count(void);
Client* get_clients(void);

#endif
