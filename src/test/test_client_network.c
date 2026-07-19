#include "platform.h"

#include "client_network.h"
#include "protocol.h"
#include "unity.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int listening_socket;
    pthread_t thread;
    atomic_bool stop;
    atomic_bool failed;
    atomic_bool saw_hello;
    atomic_bool saw_chat;
    char hello_name[PROTOCOL_DISPLAY_NAME_MAX + 1u];
    char chat_text[PROTOCOL_CHAT_MAX + 1u];
} FakeServer;

typedef struct {
    bool welcomed;
    bool received_chat;
    char sender[PROTOCOL_DISPLAY_NAME_MAX + 1u];
    char text[PROTOCOL_CHAT_MAX + 1u];
} ClientCapture;

static FakeServer server;
static ClientConnection* connection;
static char port_text[16];

static bool send_all_bytes(int socket_fd, const uint8_t* bytes, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
#ifdef _WIN32
        int sent = send(socket_fd, (const char*)bytes + offset, (int)(length - offset), 0);
#else
        ssize_t sent = send(socket_fd, bytes + offset, length - offset, MSG_NOSIGNAL);
#endif
        if (sent <= 0)
            return false;
        offset += (size_t)sent;
    }
    return true;
}

static void capture_client_message(void* context, const RelayMessage* message)
{
    ClientCapture* capture = context;
    if (message->type == RELAY_MESSAGE_WELCOME)
        capture->welcomed = true;
    if (message->type == RELAY_MESSAGE_CHAT_DELIVER) {
        capture->received_chat = true;
        snprintf(capture->sender, sizeof(capture->sender), "%s",
            message->as.chat_deliver.display_name);
        snprintf(capture->text, sizeof(capture->text), "%s", message->as.chat_deliver.text);
    }
}

static void capture_server_message(void* context, const RelayMessage* message)
{
    FakeServer* fake = context;
    if (message->type == RELAY_MESSAGE_HELLO) {
        snprintf(fake->hello_name, sizeof(fake->hello_name), "%s",
            message->as.hello.display_name);
        atomic_store(&fake->saw_hello, true);
    } else if (message->type == RELAY_MESSAGE_CHAT_SEND) {
        snprintf(fake->chat_text, sizeof(fake->chat_text), "%s", message->as.chat_send.text);
        atomic_store(&fake->saw_chat, true);
    }
}

static bool send_server_messages(int socket_fd)
{
    RelayMessage welcome = { .type = RELAY_MESSAGE_WELCOME };
    welcome.as.welcome.participant_id = 77;
    RelayMessage chat = { .type = RELAY_MESSAGE_CHAT_DELIVER };
    chat.as.chat_deliver.participant_id = 88;
    snprintf(chat.as.chat_deliver.display_name, sizeof(chat.as.chat_deliver.display_name),
        "Server peer");
    snprintf(chat.as.chat_deliver.text, sizeof(chat.as.chat_deliver.text), "hello from Relay");

    uint8_t* welcome_frame = NULL;
    uint8_t* chat_frame = NULL;
    size_t welcome_length = 0;
    size_t chat_length = 0;
    if (!protocol_encode(&welcome, &welcome_frame, &welcome_length)
        || !protocol_encode(&chat, &chat_frame, &chat_length)) {
        free(welcome_frame);
        free(chat_frame);
        return false;
    }
    bool sent = send_all_bytes(socket_fd, welcome_frame, 2)
        && send_all_bytes(socket_fd, welcome_frame + 2, welcome_length - 2)
        && send_all_bytes(socket_fd, chat_frame, chat_length);
    free(chat_frame);
    free(welcome_frame);
    return sent;
}

static void* fake_server_main(void* argument)
{
    FakeServer* fake = argument;
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int client = accept(fake->listening_socket, (struct sockaddr*)&address, &length);
    if (client == -1) {
        atomic_store(&fake->failed, true);
        return NULL;
    }
    ProtocolDecoder decoder;
    protocol_decoder_init(&decoder);
    bool sent_responses = false;
    while (!atomic_load(&fake->stop) && !atomic_load(&fake->saw_chat)) {
        uint8_t buffer[4096];
#ifdef _WIN32
        int received = recv(client, (char*)buffer, sizeof(buffer), 0);
#else
        ssize_t received = recv(client, buffer, sizeof(buffer), 0);
#endif
        if (received <= 0)
            break;
        if (!protocol_decoder_feed(&decoder, buffer, (size_t)received,
                capture_server_message, fake)) {
            atomic_store(&fake->failed, true);
            break;
        }
        if (atomic_load(&fake->saw_hello) && !sent_responses) {
            sent_responses = true;
            if (!send_server_messages(client)) {
                atomic_store(&fake->failed, true);
                break;
            }
        }
    }
    protocol_decoder_destroy(&decoder);
    closesocket(client);
    return NULL;
}

static void wait_one_millisecond(void)
{
#ifdef _WIN32
    Sleep(1);
#else
    struct timespec interval = { .tv_sec = 0, .tv_nsec = 1000 * 1000 };
    (void)nanosleep(&interval, NULL);
#endif
}

void setUp(void)
{
    memset(&server, 0, sizeof(server));
    server.listening_socket = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_NOT_EQUAL_INT(-1, server.listening_socket);
    struct sockaddr_in address = { 0 };
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    TEST_ASSERT_EQUAL_INT(0, bind(server.listening_socket,
        (struct sockaddr*)&address, sizeof(address)));
    TEST_ASSERT_EQUAL_INT(0, listen(server.listening_socket, 1));
    socklen_t length = sizeof(address);
    TEST_ASSERT_EQUAL_INT(0, getsockname(server.listening_socket,
        (struct sockaddr*)&address, &length));
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)ntohs(address.sin_port));
    atomic_init(&server.stop, false);
    atomic_init(&server.failed, false);
    atomic_init(&server.saw_hello, false);
    atomic_init(&server.saw_chat, false);
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&server.thread, NULL, fake_server_main, &server));
    connection = client_connection_create();
    TEST_ASSERT_NOT_NULL(connection);
}

void tearDown(void)
{
    client_connection_destroy(connection);
    connection = NULL;
    atomic_store(&server.stop, true);
    closesocket(server.listening_socket);
    pthread_join(server.thread, NULL);
}

void test_connection_owns_handshake_queue_incremental_decode_and_shutdown(void)
{
    TEST_ASSERT_EQUAL_INT(0, connect_to_server(connection, "127.0.0.1", port_text, "Alice"));
    ClientCapture captured = { 0 };
    for (unsigned attempt = 0; attempt < 2000u && !captured.received_chat; ++attempt) {
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0,
            client_connection_poll(connection, capture_client_message, &captured));
        wait_one_millisecond();
    }
    TEST_ASSERT_FALSE(atomic_load(&server.failed));
    TEST_ASSERT_TRUE(atomic_load(&server.saw_hello));
    TEST_ASSERT_EQUAL_STRING("Alice", server.hello_name);
    TEST_ASSERT_TRUE(captured.welcomed);
    TEST_ASSERT_TRUE(captured.received_chat);
    TEST_ASSERT_EQUAL_UINT64(77, client_connection_participant_id(connection));
    TEST_ASSERT_EQUAL_STRING("Server peer", captured.sender);
    TEST_ASSERT_EQUAL_STRING("hello from Relay", captured.text);

    TEST_ASSERT_EQUAL(RELAY_SEND_OK, client_connection_send_chat(connection, "queued outbound"));
    for (unsigned attempt = 0; attempt < 2000u && !atomic_load(&server.saw_chat); ++attempt)
        wait_one_millisecond();
    TEST_ASSERT_TRUE(atomic_load(&server.saw_chat));
    TEST_ASSERT_EQUAL_STRING("queued outbound", server.chat_text);

    disconnect_from_server(connection);
    TEST_ASSERT_FALSE(client_connection_is_connected(connection));
    TEST_ASSERT_EQUAL(RELAY_SEND_CLOSED, client_connection_send_chat(connection, "too late"));
}

int main(void)
{
    if (init_network() != 0)
        return 1;
    UNITY_BEGIN();
    RUN_TEST(test_connection_owns_handshake_queue_incremental_decode_and_shutdown);
    int result = UNITY_END();
    cleanup_network();
    return result;
}
