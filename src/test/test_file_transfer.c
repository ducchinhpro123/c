#include "unity.h"
#include "fff.h"
#include "client_logic.h"
#include "file_transfer_state.h"
#include "client_network.h"
#include "message.h"
#include <string.h>
#include <stdlib.h>

DEFINE_FFF_GLOBALS;

// Fakes for functions called by handle_file_packet or its dependencies
// Fake state variables normally in file_transfer_state.c
char incoming_stream[INCOMING_STREAM_CAPACITY];
size_t incoming_stream_len = 0;

FAKE_VALUE_FUNC(IncomingTransfer*, get_incoming_transfer, const char*);
FAKE_VALUE_FUNC(IncomingTransfer*, get_free_incoming);
FAKE_VOID_FUNC(finalize_incoming_transfer, IncomingTransfer*, bool, const char*);
FAKE_VOID_FUNC(add_message, MessageQueue*, const char*, const char*);
FAKE_VALUE_FUNC(int, recv_msg, ClientConnection*, char*, int);
FAKE_VOID_FUNC(disconnect_from_server, ClientConnection*);
FAKE_VALUE_FUNC(int, send_msg, ClientConnection*, const char*);
FAKE_VOID_FUNC(abort_all_transfers);
FAKE_VOID_FUNC(ensure_receive_directory);

// We need a realish MessageQueue for the tests
MessageQueue g_mq;

void setUp(void)
{
    RESET_FAKE(get_incoming_transfer);
    RESET_FAKE(get_free_incoming);
    RESET_FAKE(finalize_incoming_transfer);
    RESET_FAKE(add_message);
    RESET_FAKE(recv_msg);
    RESET_FAKE(disconnect_from_server);
    RESET_FAKE(send_msg);
    RESET_FAKE(abort_all_transfers);
    RESET_FAKE(ensure_receive_directory);
    FFF_RESET_HISTORY();
    
    incoming_stream_len = 0;
    memset(&g_mq, 0, sizeof(MessageQueue));
    // Reset any other global state if necessary
}

void tearDown(void)
{
}

// Helper to simulate handle_file_packet call (it's static in client_logic.c, so we might need to expose it for testing or test through process_incoming_stream)
// Looking at client_logic.c: handle_file_packet is static.
// process_incoming_stream calls handle_packet, which calls handle_file_packet.
// So we should test via process_incoming_stream or handle_packet if exposed.
// Since handle_packet is ALSO static, we test via process_incoming_stream.

void test_handle_file_start_normal(void)
{
    // Arrange
    IncomingTransfer mock_slot = {0};
    get_free_incoming_fake.return_val = &mock_slot;
    
    PacketHeader header;
    header.type = PACKET_TYPE_FILE_START;
    const char* payload = "sender|fileid123|test.txt|1024";
    header.length = htonl(strlen(payload));
    
    char buffer[sizeof(PacketHeader) + 1024];
    memcpy(buffer, &header, sizeof(PacketHeader));
    memcpy(buffer + sizeof(PacketHeader), payload, strlen(payload));
    
    // Act
    process_incoming_stream(&g_mq, buffer, sizeof(PacketHeader) + strlen(payload));
    
    // Assert
    TEST_ASSERT_EQUAL(1, get_free_incoming_fake.call_count);
    TEST_ASSERT_TRUE(mock_slot.active);
    TEST_ASSERT_EQUAL_STRING("fileid123", mock_slot.file_id);
    TEST_ASSERT_EQUAL_STRING("test.txt", mock_slot.filename);
    TEST_ASSERT_EQUAL(1024, mock_slot.total_bytes);
}

void test_handle_file_start_full_slots(void)
{
    // Arrange
    get_free_incoming_fake.return_val = NULL; // No free slots
    
    PacketHeader header;
    header.type = PACKET_TYPE_FILE_START;
    const char* payload = "sender|fileid123|test.txt|1024";
    header.length = htonl(strlen(payload));
    
    char buffer[sizeof(PacketHeader) + 1024];
    memcpy(buffer, &header, sizeof(PacketHeader));
    memcpy(buffer + sizeof(PacketHeader), payload, strlen(payload));
    
    // Act
    process_incoming_stream(&g_mq, buffer, sizeof(PacketHeader) + strlen(payload));
    
    // Assert
    TEST_ASSERT_EQUAL(1, get_free_incoming_fake.call_count);
    // Should probably log or notify if full, but current implementation just returns.
}

void test_handle_file_chunk_without_start(void)
{
    // Arrange
    get_incoming_transfer_fake.return_val = NULL; // Not found
    
    PacketHeader header;
    header.type = PACKET_TYPE_FILE_CHUNK;
    char payload[FILE_ID_LEN + 10];
    memset(payload, 0, sizeof(payload)); // Use 0 instead of 'A'
    memcpy(payload, "fileid123", 9);
    
    header.length = htonl(sizeof(payload));
    
    char buffer[sizeof(PacketHeader) + sizeof(payload)];
    memcpy(buffer, &header, sizeof(PacketHeader));
    memcpy(buffer + sizeof(PacketHeader), payload, sizeof(payload));
    
    // Act
    process_incoming_stream(&g_mq, buffer, sizeof(PacketHeader) + sizeof(payload));
    
    // Assert
    TEST_ASSERT_EQUAL(1, get_incoming_transfer_fake.call_count);
    TEST_ASSERT_EQUAL_STRING("fileid123", get_incoming_transfer_fake.arg0_val);
}

void test_handle_file_start_duplicate_id(void)
{
    // Arrange
    IncomingTransfer existing_slot = { .active = true };
    strcpy(existing_slot.file_id, "fileid123");
    get_incoming_transfer_fake.return_val = &existing_slot;
    
    PacketHeader header;
    header.type = PACKET_TYPE_FILE_START;
    const char* payload = "sender|fileid123|test.txt|1024";
    header.length = htonl(strlen(payload));
    
    char buffer[sizeof(PacketHeader) + 1024];
    memcpy(buffer, &header, sizeof(PacketHeader));
    memcpy(buffer + sizeof(PacketHeader), payload, strlen(payload));
    
    // Act
    process_incoming_stream(&g_mq, buffer, sizeof(PacketHeader) + strlen(payload));
    
    // Assert
    TEST_ASSERT_EQUAL(1, get_incoming_transfer_fake.call_count);
    TEST_ASSERT_EQUAL(0, get_free_incoming_fake.call_count); // Should NOT get a new slot
}

void test_handle_file_end_size_mismatch_under(void)
{
    // Arrange
    IncomingTransfer slot = {
        .active = true,
        .total_bytes = 1000,
        .received_bytes = 500, // Under
        .fp = (FILE*)1 // Mock pointer
    };
    strcpy(slot.file_id, "fileid123");
    get_incoming_transfer_fake.return_val = &slot;
    
    PacketHeader header;
    header.type = PACKET_TYPE_FILE_END;
    const char* payload = "sender|fileid123";
    header.length = htonl(strlen(payload));
    
    char buffer[sizeof(PacketHeader) + 1024];
    memcpy(buffer, &header, sizeof(PacketHeader));
    memcpy(buffer + sizeof(PacketHeader), payload, strlen(payload));
    
    // Act
    process_incoming_stream(&g_mq, buffer, sizeof(PacketHeader) + strlen(payload));
    
    // Assert
    TEST_ASSERT_EQUAL(1, finalize_incoming_transfer_fake.call_count);
    TEST_ASSERT_FALSE(finalize_incoming_transfer_fake.arg1_val); // success = false
    TEST_ASSERT_EQUAL_STRING("incomplete file", finalize_incoming_transfer_fake.arg2_val);
}

void test_handle_file_abort(void)
{
    // Arrange
    IncomingTransfer slot = { .active = true };
    strcpy(slot.file_id, "fileid123");
    get_incoming_transfer_fake.return_val = &slot;
    
    PacketHeader header;
    header.type = PACKET_TYPE_FILE_ABORT;
    const char* payload = "sender|fileid123|User canceled";
    header.length = htonl(strlen(payload));
    
    char buffer[sizeof(PacketHeader) + 1024];
    memcpy(buffer, &header, sizeof(PacketHeader));
    memcpy(buffer + sizeof(PacketHeader), payload, strlen(payload));
    
    // Act
    process_incoming_stream(&g_mq, buffer, sizeof(PacketHeader) + strlen(payload));
    
    // Assert
    TEST_ASSERT_EQUAL(1, finalize_incoming_transfer_fake.call_count);
    TEST_ASSERT_FALSE(finalize_incoming_transfer_fake.arg1_val); // success = false
    TEST_ASSERT_EQUAL_STRING("User canceled", finalize_incoming_transfer_fake.arg2_val);
}

void test_handle_file_chunk_over_size(void)
{
    // Arrange
    FILE* dev_null = fopen("/dev/null", "wb");
    TEST_ASSERT_NOT_NULL(dev_null);
    
    IncomingTransfer slot = {
        .active = true,
        .total_bytes = 100,
        .received_bytes = 90,
        .fp = dev_null
    };
    strcpy(slot.file_id, "fileid123");
    get_incoming_transfer_fake.return_val = &slot;
    
    PacketHeader header;
    header.type = PACKET_TYPE_FILE_CHUNK;
    char payload[FILE_ID_LEN + 20]; // 20 bytes payload
    memset(payload, 0, sizeof(payload));
    memcpy(payload, "fileid123", 9);
    
    header.length = htonl(sizeof(payload));
    
    char buffer[sizeof(PacketHeader) + sizeof(payload)];
    memcpy(buffer, &header, sizeof(PacketHeader));
    memcpy(buffer + sizeof(PacketHeader), payload, sizeof(payload));
    
    // Act
    process_incoming_stream(&g_mq, buffer, sizeof(PacketHeader) + sizeof(payload));
    
    // Assert
    // received_bytes becomes 110, which is > total_bytes (100)
    TEST_ASSERT_EQUAL(1, finalize_incoming_transfer_fake.call_count);
    TEST_ASSERT_FALSE(finalize_incoming_transfer_fake.arg1_val);
    TEST_ASSERT_EQUAL_STRING("received more than expected", finalize_incoming_transfer_fake.arg2_val);
    
    fclose(dev_null);
}

void test_handle_file_start_invalid_format(void)
{
    // Arrange
    get_free_incoming_fake.return_val = (IncomingTransfer*)1; // Should not be reached
    
    PacketHeader header;
    header.type = PACKET_TYPE_FILE_START;
    const char* payload = "sender|only_two_fields";
    header.length = htonl(strlen(payload));
    
    char buffer[sizeof(PacketHeader) + 1024];
    memcpy(buffer, &header, sizeof(PacketHeader));
    memcpy(buffer + sizeof(PacketHeader), payload, strlen(payload));
    
    // Act
    process_incoming_stream(&g_mq, buffer, sizeof(PacketHeader) + strlen(payload));
    
    // Assert
    TEST_ASSERT_EQUAL(0, get_free_incoming_fake.call_count); // Should return early
}

void test_filename_sanitization(void)
{
    char filename[] = "../../etc/passwd";
    sanitize_filename(filename);
    TEST_ASSERT_EQUAL_STRING(".._.._etc_passwd", filename);
    
    char filename2[] = "C:\\Windows\\System32\\cmd.exe";
    sanitize_filename(filename2);
    // C (0), : (1), \ (2), W (3) ...
    // Expected: C__Windows_System32_cmd.exe
    TEST_ASSERT_EQUAL_STRING("C__Windows_System32_cmd.exe", filename2);
}

// Since we can't easily have multiple mains, we'll combine tests into one runner or use a separate one.
// For now, I'll just put a main here and we will fix nob.c to build multiple runners.

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_handle_file_start_normal);
    RUN_TEST(test_handle_file_start_full_slots);
    RUN_TEST(test_handle_file_chunk_without_start);
    RUN_TEST(test_handle_file_start_duplicate_id);
    RUN_TEST(test_handle_file_end_size_mismatch_under);
    RUN_TEST(test_handle_file_abort);
    RUN_TEST(test_handle_file_chunk_over_size);
    RUN_TEST(test_handle_file_start_invalid_format);
    RUN_TEST(test_filename_sanitization);
    return UNITY_END();
}
