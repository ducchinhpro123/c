#ifndef SERVER_H
#define SERVER_H

#include <fcntl.h>
#include <arpa/inet.h>  // inet_addr()
#include <netinet/in.h> // sockaddr_in
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <raylib.h>
#include <sys/socket.h> // socket()
#include <unistd.h>     // close()

#define MAX_CLIENTS 100
#define PORT 8898
#define BUFFER_SIZE 1024

typedef struct {
    int sock_fd;
    char username[256];
    char ip_addr[16];
} Client;

typedef struct {
    char username[256];
    char message[BUFFER_SIZE];
    bool is_system;
} ChatMessage;

bool init_server();
void cleanup_server();
bool is_server_running();
int server_accept_client();
void server_recv_msgs();
void server_broadcast_msg(const char *msg, int sender_fd);
int get_client_count();
Client* get_clients();

#endif
