#include "server.h"
#include <errno.h>
#include <fcntl.h>
#include <raylib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static int server_fd = -1;
static Client clients[MAX_CLIENTS];
static int client_count = 0;
static bool server_running = false;
static server_msg_cb g_on_msg_cb = NULL;

void server_set_msg_cb(server_msg_cb cb)
{
    g_on_msg_cb = cb;
}

int get_client_count()
{
    return client_count;
}
Client* get_clients()
{
    return clients;
}

int server_accept_client()
{
    if (!server_running || server_fd == -1) {
        return -1;
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

    if (client_fd == -1) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return -1;
        }
        TraceLog(LOG_ERROR, "Accept failed. %s", strerror(errno));
        return -1;
    }

    // Non-block mode
    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags == -1) {
        TraceLog(LOG_ERROR, "fcntl failed: (%s)", strerror(errno));
    } else {
        if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            TraceLog(LOG_WARNING, "fcntl(F_SETFL) O_NONBLOCK failed: %s", strerror(errno));
        }
    }

    if (client_count >= MAX_CLIENTS) {
        TraceLog(LOG_WARNING, "Max clients reached, rejecting");
        close(client_fd);
        return -1;
    }

    clients[client_count].sock_fd = client_fd;
    strcpy(clients[client_count].username, "Unknown");
    strcpy(clients[client_count].ip_addr, inet_ntoa(client_addr.sin_addr));

    client_count++;
    char join_msg[512];
    snprintf(join_msg, sizeof(join_msg), "SYSTEM: A new user has joined with ip: %s", clients[client_count - 1].ip_addr);

    server_broadcast_msg(join_msg, client_fd);

    // TraceLog(LOG_WARNING, "Client connected from: %s (%d/%d)", clients[client_count - 1].ip_addr, client_count, MAX_CLIENTS);

    return client_fd;
}

bool is_server_running()
{
    return server_running;
}

void cleanup_server()
{
    if (server_fd != -1) {
        TraceLog(LOG_INFO, "Cleaning the server");
        close(server_fd);
        server_fd = -1;
    }

    TraceLog(LOG_INFO, "Finished clean up the server");
    server_running = false;
}

bool init_server()
{
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("socket");
        TraceLog(LOG_ERROR, "Socket creation failed.");
        return false;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // Accept connection on any interface
    addr.sin_port = htons(PORT); // Convert to network byte order, listen on port PORT

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        TraceLog(LOG_ERROR, "Bind failed: %s", strerror(errno));
        close(server_fd);
        return false;
    }
    if (listen(server_fd, 10) == -1) {
        perror("listen");
        TraceLog(LOG_ERROR, "Listen failed: %s", strerror(errno));
        close(server_fd);
        return false;
    }

    int flags = fcntl(server_fd, F_GETFL, 0); // Current status of the socket
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK); // Modifty it

    server_running = true;
    TraceLog(LOG_INFO, "Server is running on port %d, ready to accept connections", PORT);
    return true;
}

void server_recv_msgs()
{
    for (int i = 0; i < client_count; ++i) {
        char buffer[BUFFER_SIZE];
        int fd = clients[i].sock_fd;
        int bytes_recv = recv(fd, buffer, sizeof(buffer) - 1, 0);

        if (bytes_recv > 0) {
            buffer[bytes_recv] = '\0';
            // Handle simple username handshake: "USERNAME:<name>"
            const char* prefix = "USERNAME:";
            size_t prefix_len = strlen(prefix);
            if (bytes_recv >= (int)prefix_len && strncmp(buffer, prefix, prefix_len) == 0) {
                const char* name = buffer + prefix_len;
                if (*name) {
                    snprintf(clients[i].username, sizeof(clients[i].username), "%s", name);
                    TraceLog(LOG_INFO, "Client %s set username to '%s'", clients[i].ip_addr, clients[i].username);
                }
                continue; // do not broadcast handshake
            }

            TraceLog(LOG_INFO, "Received a message from %s: %s", clients[i].username, buffer);
            char* colon_pos = strchr(buffer, ':');
            if (colon_pos != NULL) {
                char* msg_part = colon_pos + 1;
                TraceLog(LOG_INFO, "Sending a message %s", buffer);
                server_broadcast_msg(buffer, fd);

                if (g_on_msg_cb) {
                    g_on_msg_cb(buffer, clients[i].username);
                }
            } else {
                server_broadcast_msg(buffer, fd);
                if (g_on_msg_cb) {
                    g_on_msg_cb(buffer, clients[i].username);
                }
            }
        } else if (bytes_recv == 0) {
            TraceLog(LOG_INFO, "Client disconnected (%s)", clients[i].ip_addr);

            char leave_msg[512];
            snprintf(leave_msg, sizeof(leave_msg), "SYSTEM: User %s (%s) has left", clients[i].username, clients[i].ip_addr);
            server_broadcast_msg(leave_msg, -1); // -1 means broadcast to all clients

            close(fd);
            for (int j = i + 1; j < client_count; ++j)
                clients[j - 1] = clients[j];
            client_count--;
            i--;
        } else {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                continue;
            TraceLog(LOG_INFO, "recv failed from: %s (%s)", clients[i].ip_addr, strerror(errno));
            close(fd);
            for (int j = i + 1; j < client_count; j++)
                clients[j - 1] = clients[j];
            client_count--;
            i--;
        }
    }
}

void server_broadcast_msg(const char* msg, int sender_fd)
{
    size_t len = strlen(msg);
    for (int i = 0; i < client_count; ++i) {
        int fd = clients[i].sock_fd;
        if (fd == sender_fd)
            continue;

        ssize_t bytes_sent = send(clients[i].sock_fd, msg, len, 0);
        if (bytes_sent < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
            TraceLog(LOG_WARNING, "send failed to %s: %s", clients[i].ip_addr, strerror(errno));
            close(fd);
            for (int j = i + 1; j < client_count; ++j)
                clients[j - 1] = clients[j];
            client_count--;
            i--;
        }
    }
}
