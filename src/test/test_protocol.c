#include "protocol.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

static RelayMessage captured[8];
static size_t captured_count;

void setUp(void)
{
    for (size_t i = 0; i < captured_count; ++i)
        protocol_message_destroy(&captured[i]);
    memset(captured, 0, sizeof(captured));
    captured_count = 0;
}

void tearDown(void)
{
    setUp();
}

static void capture(void* context, const RelayMessage* message)
{
    (void)context;
    TEST_ASSERT_LESS_THAN(8, captured_count);
    captured[captured_count] = *message;
    if (message->type == RELAY_MESSAGE_FILE_CHUNK) {
        captured[captured_count].as.file_chunk.data = malloc(message->as.file_chunk.data_length);
        TEST_ASSERT_NOT_NULL(captured[captured_count].as.file_chunk.data);
        memcpy(captured[captured_count].as.file_chunk.data, message->as.file_chunk.data,
            message->as.file_chunk.data_length);
    }
    captured_count++;
}

static void encode(const RelayMessage* message, uint8_t** frame, size_t* length)
{
    TEST_ASSERT_TRUE(protocol_encode(message, frame, length));
    TEST_ASSERT_NOT_NULL(*frame);
    TEST_ASSERT_GREATER_THAN(PROTOCOL_FRAME_HEADER_SIZE, *length);
}

void test_round_trips_typed_file_offer_with_delimiter_filename(void)
{
    RelayMessage source = { .type = RELAY_MESSAGE_FILE_OFFER_PUBLISHED };
    source.as.file_offer_published.offer_id = 41;
    source.as.file_offer_published.sender_id = 7;
    strcpy(source.as.file_offer_published.sender_name, "Alice:LAN");
    strcpy(source.as.file_offer_published.filename, "report|final.txt");
    source.as.file_offer_published.total_size = 123456;
    source.as.file_offer_published.offer_window_ms = 60000;

    uint8_t* frame = NULL;
    size_t length = 0;
    encode(&source, &frame, &length);

    ProtocolDecoder decoder;
    protocol_decoder_init(&decoder);
    TEST_ASSERT_TRUE(protocol_decoder_feed(&decoder, frame, length, capture, NULL));
    TEST_ASSERT_EQUAL(1, captured_count);
    TEST_ASSERT_EQUAL(RELAY_MESSAGE_FILE_OFFER_PUBLISHED, captured[0].type);
    TEST_ASSERT_EQUAL_UINT64(41, captured[0].as.file_offer_published.offer_id);
    TEST_ASSERT_EQUAL_STRING("Alice:LAN", captured[0].as.file_offer_published.sender_name);
    TEST_ASSERT_EQUAL_STRING("report|final.txt", captured[0].as.file_offer_published.filename);
    TEST_ASSERT_EQUAL_UINT64(123456, captured[0].as.file_offer_published.total_size);

    protocol_decoder_destroy(&decoder);
    free(frame);
}

void test_incremental_decoder_handles_every_fragment_split(void)
{
    RelayMessage source = { .type = RELAY_MESSAGE_CHAT_DELIVER };
    source.as.chat_deliver.participant_id = 9;
    strcpy(source.as.chat_deliver.display_name, "Bob");
    strcpy(source.as.chat_deliver.text, "fragmented hello");

    uint8_t* frame = NULL;
    size_t length = 0;
    encode(&source, &frame, &length);

    for (size_t split = 1; split < length; ++split) {
        ProtocolDecoder decoder;
        protocol_decoder_init(&decoder);
        captured_count = 0;
        TEST_ASSERT_TRUE(protocol_decoder_feed(&decoder, frame, split, capture, NULL));
        TEST_ASSERT_EQUAL(0, captured_count);
        TEST_ASSERT_TRUE(protocol_decoder_feed(&decoder, frame + split, length - split, capture, NULL));
        TEST_ASSERT_EQUAL(1, captured_count);
        TEST_ASSERT_EQUAL_STRING("fragmented hello", captured[0].as.chat_deliver.text);
        protocol_decoder_destroy(&decoder);
    }
    free(frame);
}

void test_decoder_emits_coalesced_messages(void)
{
    RelayMessage welcome = { .type = RELAY_MESSAGE_WELCOME };
    welcome.as.welcome.participant_id = 1;
    RelayMessage declined = { .type = RELAY_MESSAGE_FILE_OFFER_DECLINED };
    declined.as.file_offer_declined.offer_id = 22;

    uint8_t *first = NULL, *second = NULL;
    size_t first_length = 0, second_length = 0;
    encode(&welcome, &first, &first_length);
    encode(&declined, &second, &second_length);
    uint8_t* combined = malloc(first_length + second_length);
    TEST_ASSERT_NOT_NULL(combined);
    memcpy(combined, first, first_length);
    memcpy(combined + first_length, second, second_length);

    ProtocolDecoder decoder;
    protocol_decoder_init(&decoder);
    TEST_ASSERT_TRUE(protocol_decoder_feed(&decoder, combined, first_length + second_length, capture, NULL));
    TEST_ASSERT_EQUAL(2, captured_count);
    TEST_ASSERT_EQUAL(RELAY_MESSAGE_WELCOME, captured[0].type);
    TEST_ASSERT_EQUAL(RELAY_MESSAGE_FILE_OFFER_DECLINED, captured[1].type);

    protocol_decoder_destroy(&decoder);
    free(combined);
    free(second);
    free(first);
}

void test_round_trips_binary_file_chunk(void)
{
    uint8_t data[] = { 0, 1, 2, 0xff, '|', 0 };
    RelayMessage source = { .type = RELAY_MESSAGE_FILE_CHUNK };
    source.as.file_chunk.offer_id = 99;
    source.as.file_chunk.offset = 1024;
    source.as.file_chunk.data = data;
    source.as.file_chunk.data_length = sizeof(data);

    uint8_t* frame = NULL;
    size_t length = 0;
    encode(&source, &frame, &length);
    ProtocolDecoder decoder;
    protocol_decoder_init(&decoder);
    TEST_ASSERT_TRUE(protocol_decoder_feed(&decoder, frame, length, capture, NULL));
    TEST_ASSERT_EQUAL(1, captured_count);
    TEST_ASSERT_EQUAL_UINT64(1024, captured[0].as.file_chunk.offset);
    TEST_ASSERT_EQUAL(sizeof(data), captured[0].as.file_chunk.data_length);
    TEST_ASSERT_EQUAL_MEMORY(data, captured[0].as.file_chunk.data, sizeof(data));

    protocol_decoder_destroy(&decoder);
    free(frame);
}

void test_decoder_rejects_oversized_frame_before_allocation(void)
{
    uint8_t header[PROTOCOL_FRAME_HEADER_SIZE] = {
        RELAY_MESSAGE_CHAT_SEND,
        0x7f, 0xff, 0xff, 0xff
    };
    ProtocolDecoder decoder;
    protocol_decoder_init(&decoder);
    TEST_ASSERT_FALSE(protocol_decoder_feed(&decoder, header, sizeof(header), capture, NULL));
    TEST_ASSERT_EQUAL(0, decoder.length);
    protocol_decoder_destroy(&decoder);
}

void test_encoder_rejects_wrong_protocol_version_and_invalid_chunk(void)
{
    RelayMessage hello = { .type = RELAY_MESSAGE_HELLO };
    hello.as.hello.version = 0;
    strcpy(hello.as.hello.display_name, "Alice");
    uint8_t* frame = NULL;
    size_t length = 0;
    TEST_ASSERT_FALSE(protocol_encode(&hello, &frame, &length));

    RelayMessage chunk = { .type = RELAY_MESSAGE_FILE_CHUNK };
    chunk.as.file_chunk.offer_id = 1;
    chunk.as.file_chunk.data_length = 1;
    TEST_ASSERT_FALSE(protocol_encode(&chunk, &frame, &length));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_trips_typed_file_offer_with_delimiter_filename);
    RUN_TEST(test_incremental_decoder_handles_every_fragment_split);
    RUN_TEST(test_decoder_emits_coalesced_messages);
    RUN_TEST(test_round_trips_binary_file_chunk);
    RUN_TEST(test_decoder_rejects_oversized_frame_before_allocation);
    RUN_TEST(test_encoder_rejects_wrong_protocol_version_and_invalid_chunk);
    return UNITY_END();
}
