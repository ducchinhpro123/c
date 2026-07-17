#include "client_logic.h"
#include "client_network.h"
#include "fff.h"
#include "file_transfer_state.h"
#include "message.h"
#include "unity.h"
#include <stdlib.h>
#include <string.h>

DEFINE_FFF_GLOBALS;

// Fakes for functions called by handle_file_packet or its dependencies
// Fake state variables normally in file_transfer_state.c
char incoming_stream[INCOMING_STREAM_CAPACITY];
size_t incoming_stream_len = 0;

FAKE_VALUE_FUNC(IncomingTransfer*, get_incoming_transfer, const char*);
FAKE_VALUE_FUNC(IncomingTransfer*, get_free_incoming);
FAKE_VALUE_FUNC(OutgoingTransfer*, get_outgoing_transfer, const char*);
FAKE_VOID_FUNC(finalize_incoming_transfer, IncomingTransfer*, bool, const char*);
FAKE_VOID_FUNC(close_outgoing_transfer, struct ClientConnection*, OutgoingTransfer*, const char*);
FAKE_VOID_FUNC(add_message, MessageQueue*, const char*, const char*);
FAKE_VALUE_FUNC(int, recv_msg, ClientConnection*, char*, int);
FAKE_VOID_FUNC(disconnect_from_server, ClientConnection*);
FAKE_VALUE_FUNC(int, send_msg, ClientConnection*, const char*);
FAKE_VOID_FUNC(abort_all_transfers);
FAKE_VOID_FUNC(ensure_receive_directory);

// We need a realish MessageQueue for the tests
MessageQueue g_mq;
static char captured_file_id[FILE_ID_LEN + 1];
static char captured_finalize_reason[256];

static IncomingTransfer* capture_incoming_file_id(const char* file_id)
{
    snprintf(captured_file_id, sizeof(captured_file_id), "%s", file_id ? file_id : "");
    return NULL;
}

static void capture_finalize_reason(IncomingTransfer* transfer, bool success, const char* reason)
{
    (void)transfer;
    (void)success;
    snprintf(captured_finalize_reason, sizeof(captured_finalize_reason), "%s", reason ? reason : "");
}

void setUp(void)
{
    RESET_FAKE(get_incoming_transfer);
    RESET_FAKE(get_free_incoming);
    RESET_FAKE(get_outgoing_transfer);
    RESET_FAKE(finalize_incoming_transfer);
    RESET_FAKE(close_outgoing_transfer);
    RESET_FAKE(add_message);
    RESET_FAKE(recv_msg);
    RESET_FAKE(disconnect_from_server);
    RESET_FAKE(send_msg);
    RESET_FAKE(abort_all_transfers);
    RESET_FAKE(ensure_receive_directory);
    FFF_RESET_HISTORY();
    captured_file_id[0] = '\0';
    captured_finalize_reason[0] = '\0';

    reset_client_stream();
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
    IncomingTransfer mock_slot = { 0 };
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
    TEST_ASSERT_EQUAL(TRANSFER_STATE_PENDING, mock_slot.state); // Now uses state enum
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
    get_incoming_transfer_fake.custom_fake = capture_incoming_file_id;

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
    TEST_ASSERT_EQUAL_STRING("fileid123", captured_file_id);
}

void test_handle_file_start_duplicate_id(void)
{
    // Arrange
    IncomingTransfer existing_slot = { .state = TRANSFER_STATE_PENDING };
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
        .state = TRANSFER_STATE_ACCEPTED,
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
    IncomingTransfer slot = { .state = TRANSFER_STATE_ACCEPTED };
    strcpy(slot.file_id, "fileid123");
    get_incoming_transfer_fake.return_val = &slot;
    finalize_incoming_transfer_fake.custom_fake = capture_finalize_reason;

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
    TEST_ASSERT_EQUAL_STRING("User canceled", captured_finalize_reason);
}

void test_handle_file_chunk_over_size(void)
{
    // Arrange
    FILE* dev_null = fopen("/dev/null", "wb");
    TEST_ASSERT_NOT_NULL(dev_null);

    IncomingTransfer slot = {
        .state = TRANSFER_STATE_ACCEPTED,
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

    char filename3[] = "report. ";
    sanitize_filename(filename3);
    TEST_ASSERT_EQUAL_STRING("report__", filename3);

    char filename4[] = "bad\x01name";
    sanitize_filename(filename4);
    TEST_ASSERT_EQUAL_STRING("bad_name", filename4);
}

void test_rejects_oversized_packet_header(void)
{
    PacketHeader header = {
        .type = PACKET_TYPE_TEXT,
        .length = htonl(PROTOCOL_TEXT_MAX_LEN + 1)
    };

    TEST_ASSERT_FALSE(process_incoming_stream(&g_mq, (const char*)&header, sizeof(header)));
    TEST_ASSERT_EQUAL(0, incoming_stream_len);
}

void test_rejects_unknown_packet_type(void)
{
    PacketHeader header = {
        .type = 255,
        .length = htonl(1)
    };

    TEST_ASSERT_FALSE(process_incoming_stream(&g_mq, (const char*)&header, sizeof(header)));
    TEST_ASSERT_EQUAL(0, incoming_stream_len);
}

void test_rejects_invalid_file_size(void)
{
    PacketHeader header;
    header.type = PACKET_TYPE_FILE_START;
    const char* payload = "sender|fileid123|test.txt|12junk|1024";
    header.length = htonl(strlen(payload));
    char buffer[sizeof(PacketHeader) + 128];
    memcpy(buffer, &header, sizeof(header));
    memcpy(buffer + sizeof(header), payload, strlen(payload));

    TEST_ASSERT_TRUE(process_incoming_stream(&g_mq, buffer, sizeof(header) + strlen(payload)));
    TEST_ASSERT_EQUAL(0, get_free_incoming_fake.call_count);
}

void test_secure_file_id_format(void)
{
    char first[FILE_ID_LEN];
    char second[FILE_ID_LEN];
    TEST_ASSERT_TRUE(generate_file_id(first, sizeof(first)));
    TEST_ASSERT_TRUE(generate_file_id(second, sizeof(second)));
    TEST_ASSERT_EQUAL(FILE_ID_LEN - 1, strlen(first));
    TEST_ASSERT_TRUE(protocol_file_id_is_valid(first));
    TEST_ASSERT_TRUE(strcmp(first, second) != 0);
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
    RUN_TEST(test_rejects_oversized_packet_header);
    RUN_TEST(test_rejects_unknown_packet_type);
    RUN_TEST(test_rejects_invalid_file_size);
    RUN_TEST(test_secure_file_id_format);
    return UNITY_END();
}
