#ifndef SERVER_H
#define SERVER_H

#include "platform.h"
#include <errno.h>
#include <raylib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// Packet Types
#define PACKET_TYPE_TEXT        0
#define PACKET_TYPE_FILE_START  1
#define PACKET_TYPE_FILE_CHUNK  2
#define PACKET_TYPE_FILE_END    3
#define PACKET_TYPE_FILE_ABORT  4
#define PACKET_TYPE_FILE_ACCEPT 5

#pragma pack(push, 1)
typedef struct {
    uint8_t type;
    uint32_t length; // Network Byte Order
} PacketHeader;
#pragma pack(pop)

#define MAX_CLIENTS 100
#define PORT 8898
// #define BUFFER_SIZE 1024
#define BUFFER_SIZE (5 * 1024 * 1024) // 5MB to support 4MB chunks
#define CLIENT_STREAM_BUFFER (BUFFER_SIZE * 2)

typedef void (*server_msg_cb)(const char* msg, const char* username);

typedef struct {
    int sock_fd;
    char username[256];
    char ip_addr[16];
    char recv_buffer[CLIENT_STREAM_BUFFER];
    size_t recv_len;
} Client;

typedef struct {
    char username[256];
    char message[BUFFER_SIZE];
    bool is_system;
} ChatMessage;

void server_set_msg_cb(server_msg_cb cb);
bool init_server();
void cleanup_server();
bool is_server_running();
int server_accept_client();
void server_recv_msgs();
void server_broadcast_msg(const char* msg, int sender_fd);
int get_client_count();
Client* get_clients();

#endif
