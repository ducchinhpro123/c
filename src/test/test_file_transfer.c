#include "file_transfer.h"
#include "unity.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAPTURED_MESSAGE_MAX 32u

typedef struct {
    RelayMessage messages[CAPTURED_MESSAGE_MAX];
    size_t count;
    bool connected;
    bool backpressure_chunk_once;
    bool backpressure_control_once;
} FakeTransport;

static char test_directory[] = "/tmp/relay-file-transfer-XXXXXX";
static FileTransferModule* module;
static FakeTransport fake;
static RelayTransport transport;
static char last_notice[512];

static void clear_captured(void)
{
    for (size_t i = 0; i < fake.count; ++i) {
        if (fake.messages[i].type == RELAY_MESSAGE_FILE_CHUNK)
            free(fake.messages[i].as.file_chunk.data);
    }
    memset(fake.messages, 0, sizeof(fake.messages));
    fake.count = 0;
}

static RelaySendResult fake_send(void* context, const RelayMessage* message)
{
    FakeTransport* state = context;
    if (!state->connected)
        return RELAY_SEND_CLOSED;
    if (state->backpressure_chunk_once && message->type == RELAY_MESSAGE_FILE_CHUNK) {
        state->backpressure_chunk_once = false;
        return RELAY_SEND_BACKPRESSURE;
    }
    if (state->backpressure_control_once && message->type != RELAY_MESSAGE_FILE_CHUNK) {
        state->backpressure_control_once = false;
        return RELAY_SEND_BACKPRESSURE;
    }
    TEST_ASSERT_LESS_THAN(CAPTURED_MESSAGE_MAX, state->count);
    RelayMessage* captured = &state->messages[state->count++];
    *captured = *message;
    if (message->type == RELAY_MESSAGE_FILE_CHUNK) {
        captured->as.file_chunk.data = malloc(message->as.file_chunk.data_length);
        TEST_ASSERT_NOT_NULL(captured->as.file_chunk.data);
        memcpy(captured->as.file_chunk.data, message->as.file_chunk.data,
            message->as.file_chunk.data_length);
    }
    return RELAY_SEND_OK;
}

static bool fake_connected(void* context)
{
    return ((FakeTransport*)context)->connected;
}

static void capture_notice(void* context, const char* message)
{
    (void)context;
    snprintf(last_notice, sizeof(last_notice), "%s", message);
}

static void cleanup_directory(void)
{
    DIR* directory = opendir(test_directory);
    if (directory) {
        struct dirent* entry;
        while ((entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            char path[1024];
            int written = snprintf(path, sizeof(path), "%s/%s", test_directory, entry->d_name);
            if (written > 0 && (size_t)written < sizeof(path))
                (void)remove(path);
        }
        closedir(directory);
    }
    (void)rmdir(test_directory);
}

static void write_source(const char* path, const uint8_t* bytes, size_t length)
{
    FILE* file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(length, fwrite(bytes, 1, length, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

void setUp(void)
{
    strcpy(test_directory, "/tmp/relay-file-transfer-XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(test_directory));
    memset(&fake, 0, sizeof(fake));
    fake.connected = true;
    transport.context = &fake;
    transport.send = fake_send;
    transport.connected = fake_connected;
    last_notice[0] = '\0';
    module = file_transfer_create(test_directory, capture_notice, NULL);
    TEST_ASSERT_NOT_NULL(module);
}

void tearDown(void)
{
    file_transfer_destroy(module);
    module = NULL;
    clear_captured();
    cleanup_directory();
}

void test_outgoing_file_is_streamed_once_then_waits_for_each_delivery(void)
{
    const uint8_t contents[] = { 0, 1, 2, 3, 0xff };
    char source[1024];
    snprintf(source, sizeof(source), "%s/source.bin", test_directory);
    write_source(source, contents, sizeof(contents));

    TEST_ASSERT_TRUE(file_transfer_offer_file(module, &transport, source));
    TEST_ASSERT_EQUAL_size_t(1, fake.count);
    TEST_ASSERT_EQUAL(RELAY_MESSAGE_FILE_OFFER_CREATE, fake.messages[0].type);
    uint64_t request_id = fake.messages[0].as.file_offer_create.request_id;
    TEST_ASSERT_NOT_EQUAL_UINT64(0, request_id);

    RelayMessage created = { .type = RELAY_MESSAGE_FILE_OFFER_CREATED };
    created.as.file_offer_created.request_id = request_id;
    created.as.file_offer_created.offer_id = 42;
    file_transfer_handle_message(module, &transport, &created);

    RelayMessage ready = { .type = RELAY_MESSAGE_FILE_TRANSFER_READY };
    ready.as.file_transfer_ready.offer_id = 42;
    ready.as.file_transfer_ready.recipient_count = 2;
    file_transfer_handle_message(module, &transport, &ready);
    TEST_ASSERT_EQUAL_size_t(1, file_transfer_active_count(module));

    file_transfer_pump(module, &transport);
    TEST_ASSERT_EQUAL_size_t(3, fake.count);
    TEST_ASSERT_EQUAL(RELAY_MESSAGE_FILE_CHUNK, fake.messages[1].type);
    TEST_ASSERT_EQUAL_UINT64(0, fake.messages[1].as.file_chunk.offset);
    TEST_ASSERT_EQUAL_MEMORY(contents, fake.messages[1].as.file_chunk.data, sizeof(contents));
    TEST_ASSERT_EQUAL(RELAY_MESSAGE_FILE_TRANSFER_END, fake.messages[2].type);

    RelayMessage result = { .type = RELAY_MESSAGE_FILE_DELIVERY_UPDATE };
    result.as.file_delivery_update.offer_id = 42;
    result.as.file_delivery_update.recipient_id = 2;
    result.as.file_delivery_update.success = true;
    strcpy(result.as.file_delivery_update.recipient_name, "Bob");
    file_transfer_handle_message(module, &transport, &result);
    TEST_ASSERT_EQUAL_size_t(1, file_transfer_active_count(module));
    result.as.file_delivery_update.recipient_id = 3;
    strcpy(result.as.file_delivery_update.recipient_name, "Carol");
    file_transfer_handle_message(module, &transport, &result);
    TEST_ASSERT_EQUAL_size_t(0, file_transfer_active_count(module));
}

void test_incoming_file_is_published_only_after_complete_delivery(void)
{
    RelayMessage published = { .type = RELAY_MESSAGE_FILE_OFFER_PUBLISHED };
    published.as.file_offer_published.offer_id = 81;
    published.as.file_offer_published.sender_id = 7;
    published.as.file_offer_published.total_size = 4;
    strcpy(published.as.file_offer_published.sender_name, "Alice");
    strcpy(published.as.file_offer_published.filename, "report.txt");
    file_transfer_handle_message(module, &transport, &published);
    TEST_ASSERT_EQUAL_size_t(1, file_transfer_pending_count(module));

    FileOfferSnapshot offer;
    TEST_ASSERT_TRUE(file_transfer_pending(module, 0, &offer));
    TEST_ASSERT_EQUAL_UINT64(81, offer.offer_id);
    TEST_ASSERT_TRUE(file_transfer_respond(module, &transport, offer.offer_id, true, NULL));
    TEST_ASSERT_EQUAL_size_t(0, file_transfer_received_count(module));

    uint8_t bytes[] = { 'd', 'a', 't', 'a' };
    RelayMessage chunk = { .type = RELAY_MESSAGE_FILE_CHUNK };
    chunk.as.file_chunk.offer_id = 81;
    chunk.as.file_chunk.data = bytes;
    chunk.as.file_chunk.data_length = sizeof(bytes);
    file_transfer_handle_message(module, &transport, &chunk);
    TEST_ASSERT_EQUAL_size_t(0, file_transfer_received_count(module));

    RelayMessage end = { .type = RELAY_MESSAGE_FILE_TRANSFER_END };
    end.as.file_transfer_end.offer_id = 81;
    end.as.file_transfer_end.total_size = sizeof(bytes);
    file_transfer_handle_message(module, &transport, &end);
    TEST_ASSERT_EQUAL_size_t(1, file_transfer_received_count(module));
    TEST_ASSERT_EQUAL(RELAY_MESSAGE_FILE_DELIVERY_RESULT,
        fake.messages[fake.count - 1u].type);
    TEST_ASSERT_TRUE(fake.messages[fake.count - 1u].as.file_delivery_result.success);

    char received_path[1024];
    snprintf(received_path, sizeof(received_path), "%s/report.txt", test_directory);
    FILE* file = fopen(received_path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    uint8_t actual[4];
    TEST_ASSERT_EQUAL_size_t(sizeof(actual), fread(actual, 1, sizeof(actual), file));
    fclose(file);
    TEST_ASSERT_EQUAL_MEMORY(bytes, actual, sizeof(bytes));
}

void test_bad_chunk_fails_only_that_delivery_and_removes_partial_file(void)
{
    RelayMessage published = { .type = RELAY_MESSAGE_FILE_OFFER_PUBLISHED };
    published.as.file_offer_published.offer_id = 90;
    published.as.file_offer_published.total_size = 4;
    strcpy(published.as.file_offer_published.sender_name, "Alice");
    strcpy(published.as.file_offer_published.filename, "broken.bin");
    file_transfer_handle_message(module, &transport, &published);
    TEST_ASSERT_TRUE(file_transfer_respond(module, &transport, 90, true, NULL));

    uint8_t bytes[] = { 1, 2 };
    RelayMessage chunk = { .type = RELAY_MESSAGE_FILE_CHUNK };
    chunk.as.file_chunk.offer_id = 90;
    chunk.as.file_chunk.offset = 1;
    chunk.as.file_chunk.data = bytes;
    chunk.as.file_chunk.data_length = sizeof(bytes);
    file_transfer_handle_message(module, &transport, &chunk);

    TEST_ASSERT_EQUAL_size_t(0, file_transfer_active_count(module));
    TEST_ASSERT_EQUAL(RELAY_MESSAGE_FILE_DELIVERY_RESULT,
        fake.messages[fake.count - 1u].type);
    TEST_ASSERT_FALSE(fake.messages[fake.count - 1u].as.file_delivery_result.success);
    char partial[1024];
    snprintf(partial, sizeof(partial), "%s/.relay-%016llx.part", test_directory,
        (unsigned long long)90);
    TEST_ASSERT_EQUAL_INT(-1, access(partial, F_OK));
}

void test_backpressure_retries_same_chunk_without_advancing_progress(void)
{
    const uint8_t contents[] = { 8, 9, 10 };
    char source[1024];
    snprintf(source, sizeof(source), "%s/retry.bin", test_directory);
    write_source(source, contents, sizeof(contents));
    TEST_ASSERT_TRUE(file_transfer_offer_file(module, &transport, source));
    uint64_t request_id = fake.messages[0].as.file_offer_create.request_id;

    RelayMessage created = { .type = RELAY_MESSAGE_FILE_OFFER_CREATED };
    created.as.file_offer_created.request_id = request_id;
    created.as.file_offer_created.offer_id = 101;
    file_transfer_handle_message(module, &transport, &created);
    RelayMessage ready = { .type = RELAY_MESSAGE_FILE_TRANSFER_READY };
    ready.as.file_transfer_ready.offer_id = 101;
    ready.as.file_transfer_ready.recipient_count = 1;
    file_transfer_handle_message(module, &transport, &ready);

    fake.backpressure_chunk_once = true;
    file_transfer_pump(module, &transport);
    TEST_ASSERT_EQUAL_size_t(1, fake.count);
    FileTransferProgress progress;
    TEST_ASSERT_TRUE(file_transfer_progress(module, 0, &progress));
    TEST_ASSERT_EQUAL_UINT64(0, progress.transferred_size);

    file_transfer_pump(module, &transport);
    TEST_ASSERT_EQUAL(RELAY_MESSAGE_FILE_CHUNK, fake.messages[1].type);
    TEST_ASSERT_EQUAL_UINT64(0, fake.messages[1].as.file_chunk.offset);
    TEST_ASSERT_EQUAL_MEMORY(contents, fake.messages[1].as.file_chunk.data, sizeof(contents));
}

void test_delivery_failure_during_streaming_is_counted_before_transfer_end(void)
{
    const uint8_t contents[] = { 3, 4, 5 };
    char source[1024];
    snprintf(source, sizeof(source), "%s/early-failure.bin", test_directory);
    write_source(source, contents, sizeof(contents));
    TEST_ASSERT_TRUE(file_transfer_offer_file(module, &transport, source));
    uint64_t request_id = fake.messages[0].as.file_offer_create.request_id;

    RelayMessage created = { .type = RELAY_MESSAGE_FILE_OFFER_CREATED };
    created.as.file_offer_created.request_id = request_id;
    created.as.file_offer_created.offer_id = 111;
    file_transfer_handle_message(module, &transport, &created);
    RelayMessage ready = { .type = RELAY_MESSAGE_FILE_TRANSFER_READY };
    ready.as.file_transfer_ready.offer_id = 111;
    ready.as.file_transfer_ready.recipient_count = 2;
    file_transfer_handle_message(module, &transport, &ready);

    RelayMessage failed = { .type = RELAY_MESSAGE_FILE_DELIVERY_UPDATE };
    failed.as.file_delivery_update.offer_id = 111;
    failed.as.file_delivery_update.recipient_id = 2;
    strcpy(failed.as.file_delivery_update.recipient_name, "Bob");
    strcpy(failed.as.file_delivery_update.reason, "disk full");
    file_transfer_handle_message(module, &transport, &failed);
    file_transfer_pump(module, &transport);
    TEST_ASSERT_EQUAL_size_t(1, file_transfer_active_count(module));

    RelayMessage succeeded = { .type = RELAY_MESSAGE_FILE_DELIVERY_UPDATE };
    succeeded.as.file_delivery_update.offer_id = 111;
    succeeded.as.file_delivery_update.recipient_id = 3;
    succeeded.as.file_delivery_update.success = true;
    strcpy(succeeded.as.file_delivery_update.recipient_name, "Carol");
    file_transfer_handle_message(module, &transport, &succeeded);
    TEST_ASSERT_EQUAL_size_t(0, file_transfer_active_count(module));
}

void test_existing_received_file_is_never_overwritten(void)
{
    const uint8_t old_contents[] = { 'o', 'l', 'd' };
    const uint8_t new_contents[] = { 'n', 'e', 'w' };
    char existing[1024];
    snprintf(existing, sizeof(existing), "%s/report.txt", test_directory);
    write_source(existing, old_contents, sizeof(old_contents));

    RelayMessage published = { .type = RELAY_MESSAGE_FILE_OFFER_PUBLISHED };
    published.as.file_offer_published.offer_id = 120;
    published.as.file_offer_published.total_size = sizeof(new_contents);
    strcpy(published.as.file_offer_published.sender_name, "Alice");
    strcpy(published.as.file_offer_published.filename, "report.txt");
    file_transfer_handle_message(module, &transport, &published);
    TEST_ASSERT_TRUE(file_transfer_respond(module, &transport, 120, true, NULL));

    RelayMessage chunk = { .type = RELAY_MESSAGE_FILE_CHUNK };
    chunk.as.file_chunk.offer_id = 120;
    chunk.as.file_chunk.data = (uint8_t*)new_contents;
    chunk.as.file_chunk.data_length = sizeof(new_contents);
    file_transfer_handle_message(module, &transport, &chunk);
    RelayMessage end = { .type = RELAY_MESSAGE_FILE_TRANSFER_END };
    end.as.file_transfer_end.offer_id = 120;
    end.as.file_transfer_end.total_size = sizeof(new_contents);
    file_transfer_handle_message(module, &transport, &end);

    FILE* original = fopen(existing, "rb");
    TEST_ASSERT_NOT_NULL(original);
    uint8_t actual_old[sizeof(old_contents)];
    TEST_ASSERT_EQUAL_size_t(sizeof(actual_old),
        fread(actual_old, 1, sizeof(actual_old), original));
    fclose(original);
    TEST_ASSERT_EQUAL_MEMORY(old_contents, actual_old, sizeof(old_contents));

    char duplicate[1024];
    snprintf(duplicate, sizeof(duplicate), "%s/report.txt(1)", test_directory);
    FILE* received = fopen(duplicate, "rb");
    TEST_ASSERT_NOT_NULL(received);
    uint8_t actual_new[sizeof(new_contents)];
    TEST_ASSERT_EQUAL_size_t(sizeof(actual_new),
        fread(actual_new, 1, sizeof(actual_new), received));
    fclose(received);
    TEST_ASSERT_EQUAL_MEMORY(new_contents, actual_new, sizeof(new_contents));
}

void test_delivery_result_is_deferred_across_control_backpressure(void)
{
    RelayMessage published = { .type = RELAY_MESSAGE_FILE_OFFER_PUBLISHED };
    published.as.file_offer_published.offer_id = 130;
    published.as.file_offer_published.total_size = 1;
    strcpy(published.as.file_offer_published.sender_name, "Alice");
    strcpy(published.as.file_offer_published.filename, "deferred.bin");
    file_transfer_handle_message(module, &transport, &published);
    TEST_ASSERT_TRUE(file_transfer_respond(module, &transport, 130, true, NULL));

    uint8_t byte = 9;
    RelayMessage chunk = { .type = RELAY_MESSAGE_FILE_CHUNK };
    chunk.as.file_chunk.offer_id = 130;
    chunk.as.file_chunk.data = &byte;
    chunk.as.file_chunk.data_length = 1;
    file_transfer_handle_message(module, &transport, &chunk);
    size_t count_before_end = fake.count;
    fake.backpressure_control_once = true;
    RelayMessage end = { .type = RELAY_MESSAGE_FILE_TRANSFER_END };
    end.as.file_transfer_end.offer_id = 130;
    end.as.file_transfer_end.total_size = 1;
    file_transfer_handle_message(module, &transport, &end);
    TEST_ASSERT_EQUAL_size_t(count_before_end, fake.count);

    file_transfer_pump(module, &transport);
    TEST_ASSERT_EQUAL_size_t(count_before_end + 1u, fake.count);
    TEST_ASSERT_EQUAL(RELAY_MESSAGE_FILE_DELIVERY_RESULT,
        fake.messages[fake.count - 1u].type);
    TEST_ASSERT_TRUE(fake.messages[fake.count - 1u].as.file_delivery_result.success);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_outgoing_file_is_streamed_once_then_waits_for_each_delivery);
    RUN_TEST(test_incoming_file_is_published_only_after_complete_delivery);
    RUN_TEST(test_bad_chunk_fails_only_that_delivery_and_removes_partial_file);
    RUN_TEST(test_backpressure_retries_same_chunk_without_advancing_progress);
    RUN_TEST(test_delivery_failure_during_streaming_is_counted_before_transfer_end);
    RUN_TEST(test_existing_received_file_is_never_overwritten);
    RUN_TEST(test_delivery_result_is_deferred_across_control_backpressure);
    return UNITY_END();
}
