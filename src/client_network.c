#include "platform.h"
#include "client_network.h"
#include "message.h"
#include <errno.h>
// #include <fcntl.h>
// #include <netdb.h>
// #include <netinet/in.h>
// #include <netinet/tcp.h>
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
    #include <io.h>
#else
    #include <unistd.h>
#endif

#include "packet_queue.h"



// Helper function: Send all data, handling partial sends and EAGAIN
static ssize_t send_all(int socket_fd, const char* data, size_t len);

// Sender thread function
static void* sender_thread_func(void* arg) {
    ClientConnection* conn = (ClientConnection*)arg;
    
    while (conn->connected || conn->queue.count > 0) {
        Packet* pkt = pq_pop(&conn->queue);
        if (!pkt) continue;

        if (conn->connected && conn->socket_fd != -1) {
            PacketHeader header;
            header.type = pkt->type;
            header.length = htonl(pkt->length);

            // Send header
            if (send_all(conn->socket_fd, (const char*)&header, sizeof(header)) != sizeof(header)) {
                TraceLog(LOG_ERROR, "Failed to send packet header in thread");
                conn->connected = false;
            } else if (pkt->length > 0 && pkt->data != NULL) {
                // Send body
                if (send_all(conn->socket_fd, (const char*)pkt->data, pkt->length) != (ssize_t)pkt->length) {
                    TraceLog(LOG_ERROR, "Failed to send packet body in thread");
                    conn->connected = false;
                }
            }
        }
        
        pq_free_packet(pkt);
    }
    return NULL;
}

void init_client_connection(ClientConnection* conn)
{
    conn->socket_fd = -1;
    conn->connected = false;
    memset(conn->username, 0, sizeof(conn->username));
    pq_init(&conn->queue);
}

// Helper: Set socket options for high-speed LAN transfer
static void optimize_socket_for_lan(int socket_fd) {
    // Disable Nagle's algorithm for immediate sending
#ifdef _WIN32
    char flag = 1;  // Windows uses char* instead of int*
    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
#else
    int flag = 1;
    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
#endif
        TraceLog(LOG_WARNING, "Failed to set TCP_NODELAY");
    }

    // Increase send buffer to 2MB for bulk transfers
#ifdef _WIN32
    char sendbuf[4];
    *(int*)sendbuf = 2 * 1024 * 1024;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, sendbuf, sizeof(sendbuf)) < 0) {
#else
    int sendbuf = 2 * 1024 * 1024;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf)) < 0) {
#endif
        TraceLog(LOG_WARNING, "Failed to set SO_SNDBUF");
    }

    // Increase receive buffer to 2MB
#ifdef _WIN32
    char recvbuf[4];
    *(int*)recvbuf = 2 * 1024 * 1024;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, recvbuf, sizeof(recvbuf)) < 0) {
#else
    int recvbuf = 2 * 1024 * 1024;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &recvbuf, sizeof(recvbuf)) < 0) {
#endif
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
#ifdef _WIN32
    u_long iMode = 1;
    ioctlsocket(conn->socket_fd, FIONBIO, &iMode);
#else
    int flags = fcntl(conn->socket_fd, F_GETFL, 0);
    fcntl(conn->socket_fd, F_SETFL, flags | O_NONBLOCK);
#endif

    // Send username handshake to server
    if (conn->username[0] != '\0') {
        char handshake[300];
        snprintf(handshake, sizeof(handshake), "USERNAME:%s", conn->username);
        if (send_msg(conn, handshake) < 0) {
            TraceLog(LOG_WARNING, "Failed to send username handshake: %s", strerror(errno));
        } else {
            TraceLog(LOG_INFO, "Sent username handshake for '%s'", conn->username);
        }
    }

    TraceLog(LOG_INFO, "Connected to %s:%s", host, port);

    // Start sender thread
    if (pthread_create(&conn->sender_thread, NULL, sender_thread_func, conn) != 0) {
        TraceLog(LOG_ERROR, "Failed to create sender thread");
        conn->connected = false;
        close(conn->socket_fd);
        return -1;
    }

    return 0;
}

// Helper function: Send all data, handling partial sends and EAGAIN
static ssize_t send_all(int socket_fd, const char* data, size_t len)
{
    size_t total_sent = 0;
    int retry_count = 0;
    const int MAX_RETRIES = (len > 10000) ? 2000 : 200;

    int base_wait_ms = 2;
    if (len > 1000000) {
        base_wait_ms = 50;
    } else if (len > 100000) {
        base_wait_ms = 20;
    } else if (len > 10000) {
        base_wait_ms = 10;
    }

    while (total_sent < len) {
#ifdef _WIN32
        ssize_t bytes_sent = send(socket_fd, data + total_sent, len - total_sent, 0);
#else
        ssize_t bytes_sent = send(socket_fd, data + total_sent, len - total_sent, MSG_NOSIGNAL);
#endif

        if (bytes_sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                retry_count++;
                if (retry_count > MAX_RETRIES) {
                    TraceLog(LOG_ERROR, "Max retries reached while sending %zu/%zu bytes", total_sent, len);
                    return -1;
                }

                int wait_ms = base_wait_ms;
                if (retry_count > 100)
                    wait_ms *= 2;
                if (retry_count > 500)
                    wait_ms *= 5;
                if (retry_count > 1000)
                    wait_ms *= 10;
                if (wait_ms > 1000)
                    wait_ms = 1000;

                usleep((useconds_t)wait_ms * 1000);
                continue;
            }

            if (errno == EPIPE || errno == ECONNRESET) {
                TraceLog(LOG_WARNING, "Connection closed while sending after %zu/%zu bytes", total_sent, len);
                return -1;
            }

            TraceLog(LOG_ERROR, "Send error after %zu/%zu bytes: %s", total_sent, len, strerror(errno));
            return -1;
        } else if (bytes_sent == 0) {
            retry_count++;
            if (retry_count > MAX_RETRIES) {
                TraceLog(LOG_WARNING, "Send stalled after %d retries (%zu/%zu bytes)", retry_count, total_sent, len);
                return total_sent > 0 ? (ssize_t)total_sent : -1;
            }
            usleep(1000);
            continue;
        }

        total_sent += (size_t)bytes_sent;

        if (len > 65536 && (total_sent % 65536) == 0) {
            TraceLog(LOG_DEBUG, "send_all progress: %zu/%zu bytes", total_sent, len);
        }

        retry_count = 0;
    }

    return (ssize_t)total_sent;
}

int send_packet(ClientConnection* conn, uint8_t type, const void* data, uint32_t length)
{
    if (!conn->connected) {
        TraceLog(LOG_ERROR, "No connection");
        return -1;
    }

    pq_push(&conn->queue, type, data, length);
    return 0;
}

// Send a message, use PACKET_TYPE_TEXT
int send_msg(ClientConnection* conn, const char* msg)
{
    return send_packet(conn, PACKET_TYPE_TEXT, msg, (uint32_t)strlen(msg));
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
        closesocket(conn->socket_fd);
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
