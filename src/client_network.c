#include "platform.h"
#include "client_network.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define CLIENT_OUTBOUND_MAX_BYTES (32u * 1024u * 1024u)
#define CLIENT_RECEIVE_CHUNK (64u * 1024u)

typedef struct FrameNode {
    uint8_t* bytes;
    size_t length;
    struct FrameNode* next;
} FrameNode;

typedef struct {
    FrameNode* head;
    FrameNode* tail;
    size_t bytes;
    bool closed;
    pthread_mutex_t mutex;
    pthread_cond_t available;
} FrameQueue;

struct ClientConnection {
    int socket_fd;
    atomic_bool connected;
    bool sender_thread_started;
    pthread_t sender_thread;
    FrameQueue outbound;
    ProtocolDecoder decoder;
    char display_name[PROTOCOL_DISPLAY_NAME_MAX + 1u];
    atomic_uint_fast64_t participant_id;
};

static void frame_queue_init(FrameQueue* queue)
{
    memset(queue, 0, sizeof(*queue));
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->available, NULL);
}

static void frame_node_destroy(FrameNode* node)
{
    if (!node)
        return;
    free(node->bytes);
    free(node);
}

static RelaySendResult frame_queue_push(FrameQueue* queue, uint8_t* bytes, size_t length)
{
    FrameNode* node = malloc(sizeof(*node));
    if (!node)
        return RELAY_SEND_ERROR;
    node->bytes = bytes;
    node->length = length;
    node->next = NULL;

    pthread_mutex_lock(&queue->mutex);
    RelaySendResult result = RELAY_SEND_OK;
    if (queue->closed) {
        result = RELAY_SEND_CLOSED;
    } else if (length > CLIENT_OUTBOUND_MAX_BYTES - queue->bytes) {
        result = RELAY_SEND_BACKPRESSURE;
    } else {
        if (queue->tail)
            queue->tail->next = node;
        else
            queue->head = node;
        queue->tail = node;
        queue->bytes += length;
        pthread_cond_signal(&queue->available);
    }
    pthread_mutex_unlock(&queue->mutex);

    if (result != RELAY_SEND_OK)
        free(node);
    return result;
}

static FrameNode* frame_queue_pop(FrameQueue* queue)
{
    pthread_mutex_lock(&queue->mutex);
    while (!queue->head && !queue->closed)
        pthread_cond_wait(&queue->available, &queue->mutex);
    FrameNode* node = queue->head;
    if (node) {
        queue->head = node->next;
        if (!queue->head)
            queue->tail = NULL;
        queue->bytes -= node->length;
    }
    pthread_mutex_unlock(&queue->mutex);
    return node;
}

static void frame_queue_close(FrameQueue* queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->closed = true;
    pthread_cond_broadcast(&queue->available);
    pthread_mutex_unlock(&queue->mutex);
}

static void frame_queue_reopen(FrameQueue* queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->closed = false;
    pthread_mutex_unlock(&queue->mutex);
}

static void frame_queue_discard(FrameQueue* queue)
{
    pthread_mutex_lock(&queue->mutex);
    FrameNode* node = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    queue->bytes = 0;
    pthread_mutex_unlock(&queue->mutex);
    while (node) {
        FrameNode* next = node->next;
        frame_node_destroy(node);
        node = next;
    }
}

static void frame_queue_destroy(FrameQueue* queue)
{
    frame_queue_close(queue);
    frame_queue_discard(queue);
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->available);
}

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

static void optimize_socket_for_lan(int socket_fd)
{
#ifdef _WIN32
    char nodelay = 1;
    int buffer_size = 8 * 1024 * 1024;
    (void)setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, (char*)&buffer_size, sizeof(buffer_size));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, (char*)&buffer_size, sizeof(buffer_size));
#else
    int nodelay = 1;
    int buffer_size = 8 * 1024 * 1024;
    (void)setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
#endif
}

static ssize_t send_all(int socket_fd, const uint8_t* bytes, size_t length)
{
    size_t sent = 0;
    unsigned stalled = 0;
    while (sent < length) {
#ifdef _WIN32
        ssize_t result = send(socket_fd, (const char*)bytes + sent, (int)(length - sent), 0);
#else
        ssize_t result = send(socket_fd, bytes + sent, length - sent, MSG_NOSIGNAL);
#endif
        if (result > 0) {
            sent += (size_t)result;
            stalled = 0;
            continue;
        }
        if (result == 0) {
            if (++stalled > 20)
                return -1;
            (void)wait_socket_writable(socket_fd, 100);
            continue;
        }
#ifdef _WIN32
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
#endif
            if (wait_socket_writable(socket_fd, 100) > 0)
                continue;
            if (++stalled <= 20)
                continue;
        }
        return -1;
    }
    return (ssize_t)sent;
}

static void* sender_thread_main(void* argument)
{
    ClientConnection* connection = argument;
    for (;;) {
        FrameNode* frame = frame_queue_pop(&connection->outbound);
        if (!frame)
            break;
        if (atomic_load(&connection->connected)
            && send_all(connection->socket_fd, frame->bytes, frame->length)
                != (ssize_t)frame->length) {
            atomic_store(&connection->connected, false);
            frame_queue_close(&connection->outbound);
        }
        frame_node_destroy(frame);
    }
    return NULL;
}

ClientConnection* client_connection_create(void)
{
    ClientConnection* connection = calloc(1, sizeof(*connection));
    if (!connection)
        return NULL;
    connection->socket_fd = -1;
    atomic_init(&connection->connected, false);
    atomic_init(&connection->participant_id, 0);
    frame_queue_init(&connection->outbound);
    protocol_decoder_init(&connection->decoder);
    return connection;
}

void client_connection_destroy(ClientConnection* connection)
{
    if (!connection)
        return;
    disconnect_from_server(connection);
    protocol_decoder_destroy(&connection->decoder);
    frame_queue_destroy(&connection->outbound);
    free(connection);
}

int connect_to_server(ClientConnection* connection, const char* host, const char* port,
    const char* display_name)
{
    if (!connection || !host || !port || !protocol_display_name_is_valid(display_name)
        || atomic_load(&connection->connected) || connection->sender_thread_started
        || connection->socket_fd != -1)
        return -1;

    struct addrinfo hints;
    struct addrinfo* addresses = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &addresses) != 0)
        return -1;

    struct addrinfo* address = NULL;
    for (address = addresses; address; address = address->ai_next) {
        connection->socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (connection->socket_fd == -1)
            continue;
        if (!set_socket_nonblocking(connection->socket_fd)) {
            closesocket(connection->socket_fd);
            connection->socket_fd = -1;
            continue;
        }
        int result = connect(connection->socket_fd, address->ai_addr, address->ai_addrlen);
        bool in_progress = false;
#ifdef _WIN32
        if (result == -1) {
            int error = WSAGetLastError();
            in_progress = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
        }
#else
        if (result == -1)
            in_progress = errno == EINPROGRESS;
#endif
        if (result == -1 && in_progress) {
            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);
            int ready = wait_socket_writable(connection->socket_fd, 3000);
            if (ready > 0 && getsockopt(connection->socket_fd, SOL_SOCKET, SO_ERROR,
#ifdef _WIN32
                    (char*)&socket_error,
#else
                    &socket_error,
#endif
                    &error_length) == 0
                && socket_error == 0)
                result = 0;
        }
        if (result == 0)
            break;
        closesocket(connection->socket_fd);
        connection->socket_fd = -1;
    }
    freeaddrinfo(addresses);
    if (!address)
        return -1;

    optimize_socket_for_lan(connection->socket_fd);
    frame_queue_discard(&connection->outbound);
    frame_queue_reopen(&connection->outbound);
    protocol_decoder_reset(&connection->decoder);
    atomic_store(&connection->participant_id, 0);
    snprintf(connection->display_name, sizeof(connection->display_name), "%s", display_name);
    atomic_store(&connection->connected, true);

    if (pthread_create(&connection->sender_thread, NULL, sender_thread_main, connection) != 0) {
        atomic_store(&connection->connected, false);
        closesocket(connection->socket_fd);
        connection->socket_fd = -1;
        frame_queue_close(&connection->outbound);
        return -1;
    }
    connection->sender_thread_started = true;

    RelayMessage hello = { .type = RELAY_MESSAGE_HELLO };
    hello.as.hello.version = PROTOCOL_VERSION;
    snprintf(hello.as.hello.display_name, sizeof(hello.as.hello.display_name), "%s", display_name);
    if (client_connection_send(connection, &hello) != RELAY_SEND_OK) {
        disconnect_from_server(connection);
        return -1;
    }
    return 0;
}

void disconnect_from_server(ClientConnection* connection)
{
    if (!connection)
        return;
    atomic_store(&connection->connected, false);
    frame_queue_close(&connection->outbound);
    if (connection->socket_fd != -1) {
#ifdef _WIN32
        (void)shutdown(connection->socket_fd, SD_BOTH);
#else
        (void)shutdown(connection->socket_fd, SHUT_RDWR);
#endif
    }
    if (connection->sender_thread_started
        && !pthread_equal(pthread_self(), connection->sender_thread)) {
        pthread_join(connection->sender_thread, NULL);
        connection->sender_thread_started = false;
    }
    if (connection->socket_fd != -1) {
        closesocket(connection->socket_fd);
        connection->socket_fd = -1;
    }
    frame_queue_discard(&connection->outbound);
    protocol_decoder_reset(&connection->decoder);
    atomic_store(&connection->participant_id, 0);
}

bool client_connection_is_connected(const ClientConnection* connection)
{
    return connection && atomic_load(&connection->connected);
}

uint64_t client_connection_participant_id(const ClientConnection* connection)
{
    return connection ? atomic_load(&connection->participant_id) : 0;
}

const char* client_connection_display_name(const ClientConnection* connection)
{
    return connection ? connection->display_name : "";
}

RelaySendResult client_connection_send(ClientConnection* connection,
    const RelayMessage* message)
{
    if (!connection || !atomic_load(&connection->connected))
        return RELAY_SEND_CLOSED;
    uint8_t* frame = NULL;
    size_t frame_length = 0;
    if (!protocol_encode(message, &frame, &frame_length))
        return RELAY_SEND_ERROR;
    RelaySendResult result = frame_queue_push(&connection->outbound, frame, frame_length);
    if (result != RELAY_SEND_OK)
        free(frame);
    return result;
}

RelaySendResult client_connection_send_chat(ClientConnection* connection, const char* text)
{
    RelayMessage message = { .type = RELAY_MESSAGE_CHAT_SEND };
    if (!text || strnlen(text, PROTOCOL_CHAT_MAX + 1u) > PROTOCOL_CHAT_MAX)
        return RELAY_SEND_ERROR;
    memcpy(message.as.chat_send.text, text, strlen(text) + 1u);
    return client_connection_send(connection, &message);
}

typedef struct {
    ClientConnection* connection;
    RelayMessageHandler handler;
    void* context;
} PollContext;

static void handle_incoming(void* opaque, const RelayMessage* message)
{
    PollContext* poll = opaque;
    if (message->type == RELAY_MESSAGE_WELCOME)
        atomic_store(&poll->connection->participant_id, message->as.welcome.participant_id);
    poll->handler(poll->context, message);
}

int client_connection_poll(ClientConnection* connection, RelayMessageHandler handler,
    void* context)
{
    if (!connection || !handler)
        return -1;
    if (!atomic_load(&connection->connected)) {
        disconnect_from_server(connection);
        return -1;
    }
    uint8_t buffer[CLIENT_RECEIVE_CHUNK];
    PollContext poll = { .connection = connection, .handler = handler, .context = context };
    int messages_available = 0;
    for (;;) {
#ifdef _WIN32
        int received = recv(connection->socket_fd, (char*)buffer, (int)sizeof(buffer), 0);
#else
        ssize_t received = recv(connection->socket_fd, buffer, sizeof(buffer), 0);
#endif
        if (received > 0) {
            messages_available = 1;
            if (!protocol_decoder_feed(&connection->decoder, buffer, (size_t)received,
                    handle_incoming, &poll)) {
                disconnect_from_server(connection);
                return -1;
            }
            continue;
        }
        if (received == 0) {
            disconnect_from_server(connection);
            return -1;
        }
#ifdef _WIN32
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK)
            break;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
#endif
        disconnect_from_server(connection);
        return -1;
    }
    return messages_available;
}

static RelaySendResult transport_send(void* context, const RelayMessage* message)
{
    return client_connection_send(context, message);
}

static bool transport_connected(void* context)
{
    return client_connection_is_connected(context);
}

RelayTransport client_connection_transport(ClientConnection* connection)
{
    return (RelayTransport) {
        .context = connection,
        .send = transport_send,
        .connected = transport_connected
    };
}
