#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>

#define MAX_CLIENTS 32
#define PORT 8898

typedef void (*server_msg_cb)(const char* message, const char* display_name);

void server_set_msg_cb(server_msg_cb callback);
bool init_server(void);
void cleanup_server(void);
bool is_server_running(void);
int server_accept_client(void);
void server_recv_msgs(void);
int get_client_count(void);

#endif
