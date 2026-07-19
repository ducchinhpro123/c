#ifndef RELAY_PROTOCOL_H
#define RELAY_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROTOCOL_VERSION 2u
#define PROTOCOL_FRAME_HEADER_SIZE 5u
#define PROTOCOL_DISPLAY_NAME_MAX 24u
#define PROTOCOL_CHAT_MAX 4000u
#define PROTOCOL_FILENAME_MAX 255u
#define PROTOCOL_REASON_MAX 255u
#define PROTOCOL_FILE_CHUNK_MAX (1024u * 1024u)
#define PROTOCOL_FILE_MAX_SIZE (500ull * 1024ull * 1024ull)
#define PROTOCOL_MAX_PAYLOAD (PROTOCOL_FILE_CHUNK_MAX + 64u)

typedef enum {
    RELAY_MESSAGE_HELLO = 1,
    RELAY_MESSAGE_WELCOME = 2,
    RELAY_MESSAGE_CHAT_SEND = 3,
    RELAY_MESSAGE_CHAT_DELIVER = 4,
    RELAY_MESSAGE_FILE_OFFER_CREATE = 5,
    RELAY_MESSAGE_FILE_OFFER_CREATED = 6,
    RELAY_MESSAGE_FILE_OFFER_PUBLISHED = 7,
    RELAY_MESSAGE_FILE_OFFER_RESPONSE = 8,
    RELAY_MESSAGE_FILE_TRANSFER_READY = 9,
    RELAY_MESSAGE_FILE_CHUNK = 10,
    RELAY_MESSAGE_FILE_TRANSFER_END = 11,
    RELAY_MESSAGE_FILE_DELIVERY_RESULT = 12,
    RELAY_MESSAGE_FILE_DELIVERY_UPDATE = 13,
    RELAY_MESSAGE_FILE_OFFER_DECLINED = 14,
    RELAY_MESSAGE_FILE_TRANSFER_CANCEL = 15,
    RELAY_MESSAGE_ACTION_REJECTED = 16
} RelayMessageType;

typedef struct {
    RelayMessageType type;
    union {
        struct {
            uint16_t version;
            char display_name[PROTOCOL_DISPLAY_NAME_MAX + 1u];
        } hello;
        struct {
            uint64_t participant_id;
        } welcome;
        struct {
            char text[PROTOCOL_CHAT_MAX + 1u];
        } chat_send;
        struct {
            uint64_t participant_id;
            char display_name[PROTOCOL_DISPLAY_NAME_MAX + 1u];
            char text[PROTOCOL_CHAT_MAX + 1u];
        } chat_deliver;
        struct {
            uint64_t request_id;
            char filename[PROTOCOL_FILENAME_MAX + 1u];
            uint64_t total_size;
            uint32_t chunk_size;
        } file_offer_create;
        struct {
            uint64_t request_id;
            uint64_t offer_id;
            uint32_t offer_window_ms;
        } file_offer_created;
        struct {
            uint64_t offer_id;
            uint64_t sender_id;
            char sender_name[PROTOCOL_DISPLAY_NAME_MAX + 1u];
            char filename[PROTOCOL_FILENAME_MAX + 1u];
            uint64_t total_size;
            uint32_t offer_window_ms;
        } file_offer_published;
        struct {
            uint64_t offer_id;
            bool accepted;
        } file_offer_response;
        struct {
            uint64_t offer_id;
            uint16_t recipient_count;
        } file_transfer_ready;
        struct {
            uint64_t offer_id;
            uint64_t offset;
            uint8_t* data;
            uint32_t data_length;
        } file_chunk;
        struct {
            uint64_t offer_id;
            uint64_t total_size;
        } file_transfer_end;
        struct {
            uint64_t offer_id;
            bool success;
            char reason[PROTOCOL_REASON_MAX + 1u];
        } file_delivery_result;
        struct {
            uint64_t offer_id;
            uint64_t recipient_id;
            char recipient_name[PROTOCOL_DISPLAY_NAME_MAX + 1u];
            bool success;
            char reason[PROTOCOL_REASON_MAX + 1u];
        } file_delivery_update;
        struct {
            uint64_t offer_id;
        } file_offer_declined;
        struct {
            uint64_t offer_id;
            char reason[PROTOCOL_REASON_MAX + 1u];
        } file_transfer_cancel;
        struct {
            RelayMessageType rejected_type;
            uint64_t correlation_id;
            char reason[PROTOCOL_REASON_MAX + 1u];
        } action_rejected;
    } as;
} RelayMessage;

typedef struct {
    uint8_t* buffer;
    size_t length;
    size_t capacity;
} ProtocolDecoder;

typedef void (*RelayMessageHandler)(void* context, const RelayMessage* message);

bool protocol_display_name_is_valid(const char* display_name);
bool protocol_message_is_valid(const RelayMessage* message);

bool protocol_encode(const RelayMessage* message, uint8_t** frame, size_t* frame_length);

void protocol_decoder_init(ProtocolDecoder* decoder);
void protocol_decoder_reset(ProtocolDecoder* decoder);
void protocol_decoder_destroy(ProtocolDecoder* decoder);
bool protocol_decoder_feed(ProtocolDecoder* decoder, const uint8_t* bytes, size_t length,
    RelayMessageHandler handler, void* context);

void protocol_message_destroy(RelayMessage* message);

#endif
