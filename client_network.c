#include "client_network.h"
#include <arpa/inet.h>
#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

// Helper: Set socket options for high-speed LAN transfer
static void optimize_socket_for_lan(int socket_fd) {
    // Disable Nagle's algorithm for immediate sending
    int flag = 1;
    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
        TraceLog(LOG_WARNING, "Failed to set TCP_NODELAY");
    }
    
    // Increase send buffer to 2MB for bulk transfers
    int sendbuf = 2 * 1024 * 1024;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf)) < 0) {
        TraceLog(LOG_WARNING, "Failed to set SO_SNDBUF");
    }
    
    // Increase receive buffer to 2MB
    int recvbuf = 2 * 1024 * 1024;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &recvbuf, sizeof(recvbuf)) < 0) {
        TraceLog(LOG_WARNING, "Failed to set SO_RCVBUF");
    }
    
    TraceLog(LOG_INFO, "Socket optimized for LAN: TCP_NODELAY, 2MB buffers");
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

    // Optimize socket for LAN speed
    optimize_socket_for_lan(conn->socket_fd);

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

// Helper function: Send all data, handling partial sends and EAGAIN
static ssize_t send_all(int socket_fd, const char* data, size_t len)
{
    size_t total_sent = 0;
    int retry_count = 0;
    const int MAX_RETRIES = 50;
    
    while (total_sent < len) {
        ssize_t bytes_sent = send(socket_fd, data + total_sent, len - total_sent, 0);
        
        if (bytes_sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full, wait and retry
                retry_count++;
                if (retry_count > MAX_RETRIES) {
                    TraceLog(LOG_ERROR, "Max retries reached, sent %zu/%zu bytes", total_sent, len);
                    return total_sent > 0 ? (ssize_t)total_sent : -1;
                }
                
                // Exponential backoff: wait longer each retry
                usleep(1000 * (1 << (retry_count / 10))); // 1ms, 2ms, 4ms, 8ms...
                continue;
            } else {
                // Real error
                TraceLog(LOG_ERROR, "Send error after %zu bytes: %s", total_sent, strerror(errno));
                return total_sent > 0 ? (ssize_t)total_sent : -1;
            }
        }
        
        total_sent += bytes_sent;
        retry_count = 0; // Reset retry counter on successful send
    }
    
    return (ssize_t)total_sent;
}

int send_msg(ClientConnection* conn, const char* msg)
{
    if (!conn->connected || conn->socket_fd == -1) {
        TraceLog(LOG_ERROR, "No connection");
        return -1;
    }

    size_t len = strlen(msg);
    ssize_t bytes_sent = send_all(conn->socket_fd, msg, len);

    if (bytes_sent < 0) {
        TraceLog(LOG_ERROR, "Failed to send message");
        conn->connected = false;
        return -1;
    } else if ((size_t)bytes_sent < len) {
        TraceLog(LOG_WARNING, "Only sent %zd of %zu bytes", bytes_sent, len);
        return (int)bytes_sent;
    } else {
        TraceLog(LOG_INFO, "Sent: %zd bytes", bytes_sent);
    }

    return (int)bytes_sent;
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
