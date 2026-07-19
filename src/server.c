#include "server.h"

#include "platform.h"
#include "protocol.h"
#include "relay_policy.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SERVER_OUTBOUND_MAX_BYTES (32u * 1024u * 1024u)
#define SERVER_RECEIVE_CHUNK (64u * 1024u)

typedef struct OutboundFrame {
    uint8_t* bytes;
    size_t length;
    size_t offset;
    struct OutboundFrame* next;
} OutboundFrame;

typedef struct {
    bool active;
    bool disconnect_requested;
    int socket_fd;
    uint64_t participant_id;
    char display_name[PROTOCOL_DISPLAY_NAME_MAX + 1u];
    char ip_address[64];
    ProtocolDecoder decoder;
    OutboundFrame* outbound_head;
    OutboundFrame* outbound_tail;
    size_t outbound_bytes;
} ServerClient;

static int server_fd = -1;
static ServerClient clients[MAX_CLIENTS];
static size_t client_count;
static bool server_running;
static RelayPolicy* policy;
static server_msg_cb message_callback;

static uint64_t monotonic_milliseconds(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

static bool socket_would_block(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
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
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, (char*)&buffer_size,
        sizeof(buffer_size));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, (char*)&buffer_size,
        sizeof(buffer_size));
#else
    int nodelay = 1;
    int buffer_size = 8 * 1024 * 1024;
    (void)setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
#endif
}

static ServerClient* client_by_participant(uint64_t participant_id)
{
    for (size_t i = 0; i < client_count; ++i) {
        if (clients[i].active && clients[i].participant_id == participant_id)
            return &clients[i];
    }
    return NULL;
}

static void discard_outbound(ServerClient* client)
{
    OutboundFrame* frame = client->outbound_head;
    while (frame) {
        OutboundFrame* next = frame->next;
        free(frame->bytes);
        free(frame);
        frame = next;
    }
    client->outbound_head = NULL;
    client->outbound_tail = NULL;
    client->outbound_bytes = 0;
}

static bool queue_message(ServerClient* client, const RelayMessage* message)
{
    if (!client || !client->active || client->disconnect_requested)
        return false;
    uint8_t* bytes = NULL;
    size_t length = 0;
    if (!protocol_encode(message, &bytes, &length))
        return false;
    if (length > SERVER_OUTBOUND_MAX_BYTES - client->outbound_bytes) {
        free(bytes);
        return false;
    }
    OutboundFrame* frame = calloc(1, sizeof(*frame));
    if (!frame) {
        free(bytes);
        return false;
    }
    frame->bytes = bytes;
    frame->length = length;
    if (client->outbound_tail)
        client->outbound_tail->next = frame;
    else
        client->outbound_head = frame;
    client->outbound_tail = frame;
    client->outbound_bytes += length;
    return true;
}

static bool policy_send(void* context, uint64_t participant_id,
    const RelayMessage* message)
{
    (void)context;
    ServerClient* client = client_by_participant(participant_id);
    if (!client)
        return false;
    if (queue_message(client, message))
        return true;
    client->disconnect_requested = true;
    return false;
}

static RelayPolicyEffects policy_effects(void)
{
    RelayPolicyEffects effects = { .send = policy_send, .context = NULL };
    return effects;
}

static bool flush_outbound(ServerClient* client)
{
    while (client->outbound_head) {
        OutboundFrame* frame = client->outbound_head;
        size_t remaining = frame->length - frame->offset;
#ifdef _WIN32
        int requested = remaining > (size_t)INT_MAX ? INT_MAX : (int)remaining;
        int sent = send(client->socket_fd, (const char*)frame->bytes + frame->offset,
            requested, 0);
#else
        ssize_t sent = send(client->socket_fd, frame->bytes + frame->offset,
            remaining, MSG_NOSIGNAL);
#endif
        if (sent > 0) {
            frame->offset += (size_t)sent;
            if (frame->offset != frame->length)
                continue;
            client->outbound_head = frame->next;
            if (!client->outbound_head)
                client->outbound_tail = NULL;
            client->outbound_bytes -= frame->length;
            free(frame->bytes);
            free(frame);
            continue;
        }
        if (sent < 0 && socket_would_block())
            return true;
        return false;
    }
    return true;
}

static void handle_decoded_message(void* context, const RelayMessage* message)
{
    ServerClient* client = context;
    if (!client || client->disconnect_requested)
        return;
    if (client->participant_id == 0) {
        if (message->type != RELAY_MESSAGE_HELLO
            || message->as.hello.version != PROTOCOL_VERSION
            || !relay_policy_join(policy, message->as.hello.display_name,
                &client->participant_id)) {
            client->disconnect_requested = true;
            return;
        }
        snprintf(client->display_name, sizeof(client->display_name), "%s",
            message->as.hello.display_name);
        RelayMessage welcome = { .type = RELAY_MESSAGE_WELCOME };
        welcome.as.welcome.participant_id = client->participant_id;
        if (!queue_message(client, &welcome))
            client->disconnect_requested = true;
        return;
    }
    if (message->type == RELAY_MESSAGE_HELLO || message->type == RELAY_MESSAGE_WELCOME) {
        client->disconnect_requested = true;
        return;
    }
    if (message_callback && message->type == RELAY_MESSAGE_CHAT_SEND)
        message_callback(message->as.chat_send.text, client->display_name);
    RelayPolicyEffects effects = policy_effects();
    relay_policy_handle(policy, client->participant_id, message,
        monotonic_milliseconds(), &effects);
}

static void remove_client(size_t index)
{
    if (index >= client_count)
        return;
    ServerClient* client = &clients[index];
    uint64_t participant_id = client->participant_id;
    client->active = false;
    if (client->socket_fd != -1)
        closesocket(client->socket_fd);
    protocol_decoder_destroy(&client->decoder);
    discard_outbound(client);

    if (participant_id != 0 && policy) {
        RelayPolicyEffects effects = policy_effects();
        relay_policy_leave(policy, participant_id, monotonic_milliseconds(), &effects);
    }
    if (index + 1u < client_count)
        memmove(&clients[index], &clients[index + 1u],
            (client_count - index - 1u) * sizeof(clients[0]));
    client_count--;
    memset(&clients[client_count], 0, sizeof(clients[client_count]));
}

void server_set_msg_cb(server_msg_cb callback)
{
    message_callback = callback;
}

int get_client_count(void)
{
    return (int)client_count;
}

bool is_server_running(void)
{
    return server_running;
}

bool init_server(void)
{
    if (server_running)
        return true;
    if (init_network() != 0)
        return false;
    policy = relay_policy_create();
    if (!policy) {
        cleanup_network();
        return false;
    }
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
        goto fail;

#ifdef _WIN32
    char reuse = 1;
#else
    int reuse = 1;
#endif
    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = { 0 };
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PORT);
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) != 0
        || listen(server_fd, MAX_CLIENTS) != 0
        || !set_socket_nonblocking(server_fd))
        goto fail;
    memset(clients, 0, sizeof(clients));
    client_count = 0;
    server_running = true;
    return true;

fail:
    if (server_fd != -1) {
        closesocket(server_fd);
        server_fd = -1;
    }
    relay_policy_destroy(policy);
    policy = NULL;
    cleanup_network();
    return false;
}

void cleanup_server(void)
{
    while (client_count > 0)
        remove_client(client_count - 1u);
    if (server_fd != -1) {
        closesocket(server_fd);
        server_fd = -1;
    }
    relay_policy_destroy(policy);
    policy = NULL;
    server_running = false;
    cleanup_network();
}

int server_accept_client(void)
{
    if (!server_running || server_fd == -1)
        return -1;
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);
    int accepted = accept(server_fd, (struct sockaddr*)&address, &address_length);
    if (accepted == -1)
        return -1;
    if (client_count >= MAX_CLIENTS) {
        closesocket(accepted);
        return -1;
    }
    optimize_socket_for_lan(accepted);
    if (!set_socket_nonblocking(accepted)) {
        closesocket(accepted);
        return -1;
    }

    ServerClient* client = &clients[client_count++];
    memset(client, 0, sizeof(*client));
    client->active = true;
    client->socket_fd = accepted;
    protocol_decoder_init(&client->decoder);
    const char* printable = inet_ntoa(address.sin_addr);
    snprintf(client->ip_address, sizeof(client->ip_address), "%s",
        printable ? printable : "unknown");
    return accepted;
}

void server_recv_msgs(void)
{
    if (!server_running || !policy)
        return;
    RelayPolicyEffects effects = policy_effects();
    relay_policy_tick(policy, monotonic_milliseconds(), &effects);

    size_t index = 0;
    while (index < client_count) {
        ServerClient* client = &clients[index];
        if (!flush_outbound(client))
            client->disconnect_requested = true;

        while (!client->disconnect_requested) {
            uint8_t buffer[SERVER_RECEIVE_CHUNK];
#ifdef _WIN32
            int received = recv(client->socket_fd, (char*)buffer, sizeof(buffer), 0);
#else
            ssize_t received = recv(client->socket_fd, buffer, sizeof(buffer), 0);
#endif
            if (received > 0) {
                if (!protocol_decoder_feed(&client->decoder, buffer, (size_t)received,
                        handle_decoded_message, client))
                    client->disconnect_requested = true;
                continue;
            }
            if (received == 0) {
                client->disconnect_requested = true;
                break;
            }
            if (socket_would_block())
                break;
            client->disconnect_requested = true;
        }
        if (!client->disconnect_requested && !flush_outbound(client))
            client->disconnect_requested = true;
        if (client->disconnect_requested) {
            remove_client(index);
            continue;
        }
        index++;
    }
}
