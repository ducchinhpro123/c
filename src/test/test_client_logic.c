#include "unity.h"
#include "fff.h"
#include "client_logic.h"
#include "client_network.h"
#include "message.h"
#include "file_transfer_state.h"
#include "file_transfer.h"
#include <string.h>
#include <stdlib.h>

DEFINE_FFF_GLOBALS;

// Mocks for client_network
FAKE_VALUE_FUNC(int, recv_msg, ClientConnection*, char*, int);
FAKE_VOID_FUNC(disconnect_from_server, ClientConnection*);

// Mocks for file_transfer / file_transfer_state
FAKE_VOID_FUNC(abort_all_transfers);
FAKE_VALUE_FUNC(IncomingTransfer*, get_incoming_transfer, const char*);
FAKE_VALUE_FUNC(IncomingTransfer*, get_free_incoming);
FAKE_VOID_FUNC(finalize_incoming_transfer, IncomingTransfer*, bool, const char*);
FAKE_VOID_FUNC(ensure_receive_directory);
FAKE_VOID_FUNC(sanitize_filename, char*);

// Mocks for message
FAKE_VOID_FUNC(add_message, MessageQueue*, const char*, const char*);

// Define globals since we are not linking file_transfer_state.c
char incoming_stream[INCOMING_STREAM_CAPACITY];
size_t incoming_stream_len;

// Globals used by mocks or client_logic if needed (though client_logic calls get_incoming_transfer)
// If client_logic accesses global arrays directly? No, it uses getters mostly.
// But let's check if client_logic uses other globals.
// It uses incoming_stream. 

void setUp(void)
{
    RESET_FAKE(recv_msg);
    RESET_FAKE(disconnect_from_server);
    RESET_FAKE(abort_all_transfers);
    RESET_FAKE(get_incoming_transfer);
    RESET_FAKE(get_free_incoming);
    RESET_FAKE(finalize_incoming_transfer);
    RESET_FAKE(ensure_receive_directory);
    RESET_FAKE(sanitize_filename);
    RESET_FAKE(add_message);
    
    // Clear the stream buffer logic
    incoming_stream_len = 0;
}

void tearDown(void)
{
}

// Tests

void test_backpressure_applied_when_buffer_full(void)
{
    // Arrange
    ClientConnection* conn = calloc(1, sizeof(ClientConnection));
    conn->connected = true;
    MessageQueue* mq = calloc(1, sizeof(MessageQueue));

    // Simulate buffer is almost full (greater than CAPACITY - MSG_BUFFER)
    // defined in file_transfer_state.h
    incoming_stream_len = INCOMING_STREAM_CAPACITY - MSG_BUFFER + 100;
    
    // Act
    update_client_state(conn, mq);

    // Assert
    // recv_msg should NOT be called because backpressure should trigger
    TEST_ASSERT_EQUAL(0, recv_msg_fake.call_count);
    
    free(conn);
    free(mq);
}

void test_normal_recv_when_buffer_empty(void)
{
    // Arrange
    ClientConnection* conn = calloc(1, sizeof(ClientConnection));
    conn->connected = true;
    MessageQueue* mq = calloc(1, sizeof(MessageQueue));

    incoming_stream_len = 0;

    // Simulate recv_msg returning 0 (no data)
    recv_msg_fake.return_val = 0;

    // Act
    update_client_state(conn, mq);

    // Assert
    TEST_ASSERT_EQUAL(1, recv_msg_fake.call_count);
    
    free(conn);
    free(mq);
}

void test_disconnect_on_recv_error(void)
{
    // Arrange
    ClientConnection* conn = calloc(1, sizeof(ClientConnection));
    conn->connected = true;
    MessageQueue* mq = calloc(1, sizeof(MessageQueue));
    
    incoming_stream_len = 0;

    // Simulate recv_msg error
    recv_msg_fake.return_val = -1;

    // Act
    update_client_state(conn, mq);

    // Assert
    TEST_ASSERT_EQUAL(1, recv_msg_fake.call_count);
    TEST_ASSERT_FALSE(conn->connected);
    TEST_ASSERT_EQUAL(1, disconnect_from_server_fake.call_count);
    TEST_ASSERT_EQUAL(1, abort_all_transfers_fake.call_count);
    
    free(conn);
    free(mq);
}

int main(void)
{
    UNITY_BEGIN();
    
    RUN_TEST(test_backpressure_applied_when_buffer_full);
    RUN_TEST(test_normal_recv_when_buffer_empty);
    RUN_TEST(test_disconnect_on_recv_error);
    
    return UNITY_END();
}
