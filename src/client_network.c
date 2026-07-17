#include "client_network.h"
#include "platform.h"
#include <errno.h>
// #include <fcntl.h>
// #include <netdb.h>
// #include <netinet/in.h>
// #include <netinet/tcp.h>
#include <pthread.h>
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

static bool set_socket_nonblocking(int socket_fd)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(socket_fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(socket_fd, F_GETFL, 0);
    return flags >= 0 && fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// Sender thread function
static void* sender_thread_func(void* arg)
{
    ClientConnection* conn = (ClientConnection*)arg;

    for (;;) {
        Packet* pkt = pq_pop(&conn->queue);
        if (!pkt)
            break;

        if (atomic_load(&conn->connected) && conn->socket_fd != -1) {
            PacketHeader header;
            header.type = pkt->type;
            header.length = htonl(pkt->length);

            // Send header
            if (send_all(conn->socket_fd, (const char*)&header, sizeof(header)) != sizeof(header)) {
                TraceLog(LOG_ERROR, "Failed to send packet header in thread");
                atomic_store(&conn->connected, false);
                pq_close(&conn->queue);
            } else if (pkt->length > 0 && pkt->data != NULL) {
                // Send body
                if (send_all(conn->socket_fd, (const char*)pkt->data, pkt->length) != (ssize_t)pkt->length) {
                    TraceLog(LOG_ERROR, "Failed to send packet body in thread");
                    atomic_store(&conn->connected, false);
                    pq_close(&conn->queue);
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
    atomic_init(&conn->connected, false);
    conn->sender_thread_started = false;
    memset(conn->username, 0, sizeof(conn->username));
    pq_init(&conn->queue);
}

// Helper: Set socket options for high-speed LAN transfer
static void optimize_socket_for_lan(int socket_fd)
{
    int pr = 8;
    // Disable Nagle's algorithm for immediate sending
#ifdef _WIN32
    char flag = 1; // Windows uses char* instead of int*
    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
#else
    int flag = 1;
    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
#endif
        TraceLog(LOG_WARNING, "Failed to set TCP_NODELAY");
    }

#ifdef _WIN32
    int sendbuf_size = pr * 1024 * 1024;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, (char*)&sendbuf_size, sizeof(sendbuf_size)) < 0) {
#else
    int sendbuf_size = pr * 1024 * 1024;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &sendbuf_size, sizeof(sendbuf_size)) < 0) {
#endif
        TraceLog(LOG_WARNING, "Failed to set SO_SNDBUF");
    }

#ifdef _WIN32
    int recvbuf_size = pr * 1024 * 1024;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, (char*)&recvbuf_size, sizeof(recvbuf_size)) < 0) {
#else
    int recvbuf_size = pr * 1024 * 1024;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &recvbuf_size, sizeof(recvbuf_size)) < 0) {
#endif
        TraceLog(LOG_WARNING, "Failed to set SO_RCVBUF");
    }

    TraceLog(LOG_INFO, "Socket optimized for LAN: TCP_NODELAY, %dMB buffers", pr);
}

int connect_to_server(ClientConnection* conn, const char* host, const char* port, const char* username)
{
    if (!conn || !host || host[0] == '\0' || !port || port[0] == '\0' || !protocol_username_is_valid(username) || atomic_load(&conn->connected) || conn->sender_thread_started || conn->socket_fd != -1)
        return -1;

    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(host, port, &hints, &res);
    if (status != 0) {
        TraceLog(LOG_ERROR, "getaddrinfo failed: %s", gai_strerror(status));
        return -1;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        conn->socket_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (conn->socket_fd == -1) {
#ifdef _WIN32
            TraceLog(LOG_INFO, "socket failed. Continue searching: WSA error %d", WSAGetLastError());
#else
            TraceLog(LOG_INFO, "socket failed. Continue searching: %s", strerror(errno));
#endif
            continue;
        }
        if (!set_socket_nonblocking(conn->socket_fd)) {
            closesocket(conn->socket_fd);
            conn->socket_fd = -1;
            continue;
        }

        int connect_result = connect(conn->socket_fd, p->ai_addr, p->ai_addrlen);
        bool connecting = false;
#ifdef _WIN32
        if (connect_result == -1) {
            int connect_error = WSAGetLastError();
            connecting = connect_error == WSAEWOULDBLOCK || connect_error == WSAEINPROGRESS;
        }
#else
        if (connect_result == -1)
            connecting = errno == EINPROGRESS;
#endif

        if (connect_result == -1 && connecting) {
            int socket_error = 0;
            socklen_t error_len = sizeof(socket_error);
            int ready = wait_socket_writable(conn->socket_fd, 3000);
            if (ready <= 0 || getsockopt(conn->socket_fd, SOL_SOCKET, SO_ERROR,
#ifdef _WIN32
                                  (char*)&socket_error,
#else
                                  &socket_error,
#endif
                                  &error_len)
                    != 0
                || socket_error != 0) {
                connect_result = -1;
            } else {
                connect_result = 0;
            }
        }

        if (connect_result == -1) {
#ifdef _WIN32
            TraceLog(LOG_INFO, "connect failed. Continue searching: WSA error %d", WSAGetLastError());
            closesocket(conn->socket_fd);
#else
            TraceLog(LOG_INFO, "connect failed. Continue searching: %s", strerror(errno));
            close(conn->socket_fd);
#endif
            conn->socket_fd = -1;
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
    pq_reopen(&conn->queue);
    atomic_store(&conn->connected, true);
    // set username
    snprintf(conn->username, sizeof(conn->username), "%s", username);

    // Optimize socket for LAN speed
    optimize_socket_for_lan(conn->socket_fd);

    // Start sender thread
    if (pthread_create(&conn->sender_thread, NULL, sender_thread_func, conn) != 0) {
        TraceLog(LOG_ERROR, "Failed to create sender thread");
        atomic_store(&conn->connected, false);
        closesocket(conn->socket_fd);
        conn->socket_fd = -1;
        return -1;
    }
    conn->sender_thread_started = true;

    // The first text packet is a bounded username handshake. The server never
    // trusts sender names embedded in later chat messages.
    char handshake[sizeof(conn->username) + 10];
    snprintf(handshake, sizeof(handshake), "USERNAME:%s", conn->username);
    if (send_msg(conn, handshake) < 0) {
        TraceLog(LOG_WARNING, "Failed to queue username handshake");
        disconnect_from_server(conn);
        return -1;
    }

    TraceLog(LOG_INFO, "Connected to %s:%s", host, port);

    return 0;
}

// Helper function: Send all data, handling partial sends and EAGAIN
static ssize_t send_all(int socket_fd, const char* data, size_t len)
{
    size_t total_sent = 0;
    int retry_count = 0;
    const int MAX_RETRIES = 20;
    const int POLL_TIMEOUT_MS = 100; // Wait up to 100ms for socket to be writable

    while (total_sent < len) {
#ifdef _WIN32
        ssize_t bytes_sent = send(socket_fd, data + total_sent, (int)(len - total_sent), 0);
#else
        ssize_t bytes_sent = send(socket_fd, data + total_sent, len - total_sent, MSG_NOSIGNAL);
#endif

        if (bytes_sent < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
#endif
                // Use poll/select to wait for socket to be writable (no busy-wait)
                int poll_result = wait_socket_writable(socket_fd, POLL_TIMEOUT_MS);
                if (poll_result > 0) {
                    continue; // Socket is writable, retry immediately
                } else if (poll_result == 0) {
                    // Timeout
                    retry_count++;
                    if (retry_count > MAX_RETRIES) {
                        TraceLog(LOG_ERROR, "Max retries reached while sending %zu/%zu bytes", total_sent, len);
                        return -1;
                    }
                    continue;
                } else {
                    // Poll error
                    TraceLog(LOG_ERROR, "Poll error while waiting for socket");
                    return -1;
                }
            }

#ifdef _WIN32
            if (err == WSAECONNRESET || err == WSAECONNABORTED) {
                TraceLog(LOG_WARNING, "Connection closed while sending after %zu/%zu bytes", total_sent, len);
                return -1;
            }
            TraceLog(LOG_ERROR, "Send error after %zu/%zu bytes: WSA error %d", total_sent, len, err);
#else
            if (errno == EPIPE || errno == ECONNRESET) {
                TraceLog(LOG_WARNING, "Connection closed while sending after %zu/%zu bytes", total_sent, len);
                return -1;
            }

            TraceLog(LOG_ERROR, "Send error after %zu/%zu bytes: %s", total_sent, len, strerror(errno));
#endif
            return -1;
        } else if (bytes_sent == 0) {
            retry_count++;
            if (retry_count > MAX_RETRIES) {
                TraceLog(LOG_WARNING, "Send stalled after %d retries (%zu/%zu bytes)", retry_count, total_sent, len);
                return total_sent > 0 ? (ssize_t)total_sent : -1;
            }
            // Use poll to wait instead of usleep
            wait_socket_writable(socket_fd, 10);
            continue;
        }

        total_sent += (size_t)bytes_sent;
        retry_count = 0;
    }

    return (ssize_t)total_sent;
}

int send_packet(ClientConnection* conn, uint8_t type, const void* data, uint32_t length)
{
    if (!conn || !atomic_load(&conn->connected)) {
        TraceLog(LOG_ERROR, "No connection");
        return -1;
    }

    if (!protocol_packet_is_valid(type, length) || (length > 0 && !data)) {
        TraceLog(LOG_ERROR, "Refusing invalid packet type=%u length=%u", type, length);
        return -1;
    }

    // Bound queued network data even if a caller forgets to throttle.
    if (pq_get_data_size(&conn->queue) + length > 64u * 1024u * 1024u) {
        TraceLog(LOG_WARNING, "Outgoing queue is full");
        return -1;
    }

    if (pq_push(&conn->queue, type, data, length) != 0) {
        TraceLog(LOG_ERROR, "Failed to push packet to queue (memory full?)");
        return -1;
    }
    return 0;
}

// Send a message, use PACKET_TYPE_TEXT
int send_msg(ClientConnection* conn, const char* msg)
{
    if (!msg)
        return -1;
    size_t len = strnlen(msg, PROTOCOL_TEXT_MAX_LEN + 1u);
    if (!protocol_text_is_valid(msg, len))
        return -1;
    return send_packet(conn, PACKET_TYPE_TEXT, msg, (uint32_t)len);
}

int recv_msg(ClientConnection* conn, char* buffer, int size)
{
    if (!conn || !buffer || size < 2 || conn->socket_fd == -1)
        return -1;

#ifdef _WIN32
    ssize_t bytes_recv = recv(conn->socket_fd, buffer, size - 1, 0);
#else
    ssize_t bytes_recv = recv(conn->socket_fd, buffer, (size_t)(size - 1), 0);
#endif
    if (bytes_recv < 0) {
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return 0;
        }
        TraceLog(LOG_ERROR, "Recv failed: WSA error %d", err);
#else
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return 0;
        }
        TraceLog(LOG_ERROR, "Recv failed: %s", strerror(errno));
#endif
        atomic_store(&conn->connected, false);
        return -1;
    }

    if (bytes_recv == 0) {
        TraceLog(LOG_ERROR, "Server closed connection");
        atomic_store(&conn->connected, false);
        return -1;
    }

    buffer[bytes_recv] = '\0';
    return (int)bytes_recv;
}

void disconnect_from_server(ClientConnection* conn)
{
    if (!conn)
        return;

    bool was_connected = atomic_exchange(&conn->connected, false);
    pq_close(&conn->queue);

    if (conn->socket_fd != -1) {
#ifdef _WIN32
        shutdown(conn->socket_fd, SD_BOTH);
#else
        shutdown(conn->socket_fd, SHUT_RDWR);
#endif
    }

    if (conn->sender_thread_started && !pthread_equal(pthread_self(), conn->sender_thread)) {
        pthread_join(conn->sender_thread, NULL);
        conn->sender_thread_started = false;
    }

    if (conn->socket_fd != -1) {
        closesocket(conn->socket_fd);
        conn->socket_fd = -1;
    }

    if (was_connected)
        TraceLog(LOG_INFO, "Disconnected");
}
