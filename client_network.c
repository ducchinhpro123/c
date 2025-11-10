#include "client_network.h"
#include <arpa/inet.h>
#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <raylib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* typedef struct { */
/*     int socket_fd; */
/*     bool connected; */
/*     char username[256]; */
/* } ClientConnection; */

void init_client_connection(ClientConnection* conn)
{
    conn->socket_fd = -1;
    conn->connected = false;
    memset(conn->username, 0, sizeof(conn->username));
}

int connect_to_server(ClientConnection* conn, const char* host, const char* port, const char* username)
{
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(host, port, &hints, &res);
    if (status != 0) {
        TraceLog(LOG_ERROR, "getaddrinfo failed: %s", gai_strerror(status));
        return -1;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        conn->socket_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (conn->socket_fd == -1) {
            TraceLog(LOG_INFO, "socket failed. Continue searching: %s", strerror(errno));
            continue;
        }
        if (connect(conn->socket_fd, p->ai_addr, p->ai_addrlen) == -1) {
            TraceLog(LOG_INFO, "connect failed. Continue searching: %s", strerror(errno));
            close(conn->socket_fd);
            continue;
        }

        break;
    }

    if (p == NULL) {
        TraceLog(LOG_ERROR, "Failed to connect to any address!");
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    conn->connected = true;
    // set username
    snprintf(conn->username, sizeof(conn->username), "%s", username);

    // set non-blocking mode
    int flags = fcntl(conn->socket_fd, F_GETFL, 0);
    fcntl(conn->socket_fd, F_SETFL, flags | O_NONBLOCK);

    // Send username handshake to server
    if (conn->username[0] != '\0') {
        char handshake[300];
        snprintf(handshake, sizeof(handshake), "USERNAME:%s", conn->username);
        ssize_t sent = send(conn->socket_fd, handshake, strlen(handshake), 0);
        if (sent < 0) {
            TraceLog(LOG_WARNING, "Failed to send username handshake: %s", strerror(errno));
        } else {
            TraceLog(LOG_INFO, "Sent username handshake for '%s'", conn->username);
        }
    }

    TraceLog(LOG_INFO, "Connected to %s:%s", host, port);

    return 0;
    // connect(int fd, const struct sockaddr *addr, socklen_t len);
}

int send_msg(ClientConnection* conn, const char* msg)
{
    if (!conn->connected || conn->socket_fd == -1) {
        TraceLog(LOG_ERROR, "No connection");
        return -1;
    }

    size_t len = strlen(msg);

    int bytes_sent = send(conn->socket_fd, msg, len, 0);

    if (bytes_sent < 0) {
        TraceLog(LOG_ERROR, "Failed to send message: %s", strerror(errno));
        conn->connected = false;
        return -1;
    } else if ((size_t)bytes_sent < len) {
        TraceLog(LOG_WARNING, "Only sent %d of %d bytes", bytes_sent, len);
    } else {
        TraceLog(LOG_INFO, "Sent: %d bytes", bytes_sent);
    }

    return bytes_sent;
}

int recv_msg(ClientConnection* conn, char* buffer, int size)
{
    ssize_t bytes_recv = recv(conn->socket_fd, buffer, size, 0);
    if (bytes_recv < 0) {
        // No data yet, keep the connection open
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return 0;
        }

        TraceLog(LOG_ERROR, "Recv failed: %s", strerror(errno));
        conn->connected = false;
        return -1;
    }

    if (bytes_recv == 0) {
        TraceLog(LOG_ERROR, "Server closed connection");
        conn->connected = false;
        close(conn->socket_fd);
        return -1;
    }

    buffer[bytes_recv] = '\0';
    return bytes_recv;
}

void disconnect_from_server(ClientConnection* conn)
{
    if (conn->connected || conn->socket_fd != -1) {
        conn->connected = false;
        conn->socket_fd = -1;
        TraceLog(LOG_INFO, "Disconnected");
    }
}
