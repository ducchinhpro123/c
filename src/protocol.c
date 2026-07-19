#include "protocol.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t* bytes;
    size_t length;
    size_t position;
} Writer;

typedef struct {
    const uint8_t* bytes;
    size_t length;
    size_t position;
} Reader;

static bool message_type_is_valid(uint8_t type)
{
    return type >= RELAY_MESSAGE_HELLO && type <= RELAY_MESSAGE_ACTION_REJECTED;
}

static bool text_is_valid(const char* text, size_t maximum, bool allow_newlines)
{
    if (!text)
        return false;

    size_t length = strnlen(text, maximum + 1u);
    if (length == 0 || length > maximum)
        return false;

    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == 0x7f || c == 0 || (c < 0x20 && !(allow_newlines && (c == '\n' || c == '\t'))))
            return false;
    }
    return true;
}

bool protocol_display_name_is_valid(const char* display_name)
{
    if (!text_is_valid(display_name, PROTOCOL_DISPLAY_NAME_MAX, false))
        return false;
    size_t length = strlen(display_name);
    return display_name[0] != ' ' && display_name[length - 1u] != ' ';
}

static bool reason_is_valid(const char* reason)
{
    if (!reason)
        return false;
    if (reason[0] == '\0')
        return true;
    return text_is_valid(reason, PROTOCOL_REASON_MAX, true);
}

bool protocol_message_is_valid(const RelayMessage* message)
{
    if (!message || !message_type_is_valid((uint8_t)message->type))
        return false;

    switch (message->type) {
    case RELAY_MESSAGE_HELLO:
        return message->as.hello.version == PROTOCOL_VERSION
            && protocol_display_name_is_valid(message->as.hello.display_name);
    case RELAY_MESSAGE_WELCOME:
        return message->as.welcome.participant_id != 0;
    case RELAY_MESSAGE_CHAT_SEND:
        return text_is_valid(message->as.chat_send.text, PROTOCOL_CHAT_MAX, true);
    case RELAY_MESSAGE_CHAT_DELIVER:
        return message->as.chat_deliver.participant_id != 0
            && protocol_display_name_is_valid(message->as.chat_deliver.display_name)
            && text_is_valid(message->as.chat_deliver.text, PROTOCOL_CHAT_MAX, true);
    case RELAY_MESSAGE_FILE_OFFER_CREATE:
        return message->as.file_offer_create.request_id != 0
            && text_is_valid(message->as.file_offer_create.filename, PROTOCOL_FILENAME_MAX, false)
            && message->as.file_offer_create.total_size <= PROTOCOL_FILE_MAX_SIZE
            && message->as.file_offer_create.chunk_size > 0
            && message->as.file_offer_create.chunk_size <= PROTOCOL_FILE_CHUNK_MAX;
    case RELAY_MESSAGE_FILE_OFFER_CREATED:
        return message->as.file_offer_created.request_id != 0
            && message->as.file_offer_created.offer_id != 0
            && message->as.file_offer_created.offer_window_ms > 0;
    case RELAY_MESSAGE_FILE_OFFER_PUBLISHED:
        return message->as.file_offer_published.offer_id != 0
            && message->as.file_offer_published.sender_id != 0
            && protocol_display_name_is_valid(message->as.file_offer_published.sender_name)
            && text_is_valid(message->as.file_offer_published.filename, PROTOCOL_FILENAME_MAX, false)
            && message->as.file_offer_published.total_size <= PROTOCOL_FILE_MAX_SIZE
            && message->as.file_offer_published.offer_window_ms > 0;
    case RELAY_MESSAGE_FILE_OFFER_RESPONSE:
        return message->as.file_offer_response.offer_id != 0;
    case RELAY_MESSAGE_FILE_TRANSFER_READY:
        return message->as.file_transfer_ready.offer_id != 0
            && message->as.file_transfer_ready.recipient_count > 0;
    case RELAY_MESSAGE_FILE_CHUNK:
        return message->as.file_chunk.offer_id != 0
            && message->as.file_chunk.data != NULL
            && message->as.file_chunk.data_length > 0
            && message->as.file_chunk.data_length <= PROTOCOL_FILE_CHUNK_MAX;
    case RELAY_MESSAGE_FILE_TRANSFER_END:
        return message->as.file_transfer_end.offer_id != 0
            && message->as.file_transfer_end.total_size <= PROTOCOL_FILE_MAX_SIZE;
    case RELAY_MESSAGE_FILE_DELIVERY_RESULT:
        return message->as.file_delivery_result.offer_id != 0
            && reason_is_valid(message->as.file_delivery_result.reason);
    case RELAY_MESSAGE_FILE_DELIVERY_UPDATE:
        return message->as.file_delivery_update.offer_id != 0
            && message->as.file_delivery_update.recipient_id != 0
            && protocol_display_name_is_valid(message->as.file_delivery_update.recipient_name)
            && reason_is_valid(message->as.file_delivery_update.reason);
    case RELAY_MESSAGE_FILE_OFFER_DECLINED:
        return message->as.file_offer_declined.offer_id != 0;
    case RELAY_MESSAGE_FILE_TRANSFER_CANCEL:
        return message->as.file_transfer_cancel.offer_id != 0
            && reason_is_valid(message->as.file_transfer_cancel.reason);
    case RELAY_MESSAGE_ACTION_REJECTED:
        return message_type_is_valid((uint8_t)message->as.action_rejected.rejected_type)
            && reason_is_valid(message->as.action_rejected.reason);
    }
    return false;
}

static bool write_bytes(Writer* writer, const void* source, size_t length)
{
    if (!writer || length > writer->length - writer->position)
        return false;
    if (length > 0)
        memcpy(writer->bytes + writer->position, source, length);
    writer->position += length;
    return true;
}

static bool write_u8(Writer* writer, uint8_t value)
{
    return write_bytes(writer, &value, sizeof(value));
}

static bool write_u16(Writer* writer, uint16_t value)
{
    uint8_t bytes[2] = { (uint8_t)(value >> 8), (uint8_t)value };
    return write_bytes(writer, bytes, sizeof(bytes));
}

static bool write_u32(Writer* writer, uint32_t value)
{
    uint8_t bytes[4] = {
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8), (uint8_t)value
    };
    return write_bytes(writer, bytes, sizeof(bytes));
}

static bool write_u64(Writer* writer, uint64_t value)
{
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = (uint8_t)(value >> ((7u - i) * 8u));
    return write_bytes(writer, bytes, sizeof(bytes));
}

static bool write_string(Writer* writer, const char* value)
{
    size_t length = strlen(value);
    return length <= UINT16_MAX && write_u16(writer, (uint16_t)length)
        && write_bytes(writer, value, length);
}

static bool read_bytes(Reader* reader, void* destination, size_t length)
{
    if (!reader || length > reader->length - reader->position)
        return false;
    if (length > 0)
        memcpy(destination, reader->bytes + reader->position, length);
    reader->position += length;
    return true;
}

static bool read_u8(Reader* reader, uint8_t* value)
{
    return read_bytes(reader, value, sizeof(*value));
}

static bool read_u16(Reader* reader, uint16_t* value)
{
    uint8_t bytes[2];
    if (!read_bytes(reader, bytes, sizeof(bytes)))
        return false;
    *value = ((uint16_t)bytes[0] << 8) | bytes[1];
    return true;
}

static bool read_u32(Reader* reader, uint32_t* value)
{
    uint8_t bytes[4];
    if (!read_bytes(reader, bytes, sizeof(bytes)))
        return false;
    *value = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16)
        | ((uint32_t)bytes[2] << 8) | bytes[3];
    return true;
}

static bool read_u64(Reader* reader, uint64_t* value)
{
    uint8_t bytes[8];
    if (!read_bytes(reader, bytes, sizeof(bytes)))
        return false;
    *value = 0;
    for (size_t i = 0; i < sizeof(bytes); ++i)
        *value = (*value << 8) | bytes[i];
    return true;
}

static bool read_string(Reader* reader, char* value, size_t capacity)
{
    uint16_t length = 0;
    if (!read_u16(reader, &length) || (size_t)length >= capacity)
        return false;
    if (!read_bytes(reader, value, length))
        return false;
    value[length] = '\0';
    return true;
}

static size_t string_wire_size(const char* value)
{
    return 2u + strlen(value);
}

static size_t payload_size(const RelayMessage* message)
{
    switch (message->type) {
    case RELAY_MESSAGE_HELLO:
        return 2u + string_wire_size(message->as.hello.display_name);
    case RELAY_MESSAGE_WELCOME:
        return 8u;
    case RELAY_MESSAGE_CHAT_SEND:
        return string_wire_size(message->as.chat_send.text);
    case RELAY_MESSAGE_CHAT_DELIVER:
        return 8u + string_wire_size(message->as.chat_deliver.display_name)
            + string_wire_size(message->as.chat_deliver.text);
    case RELAY_MESSAGE_FILE_OFFER_CREATE:
        return 8u + string_wire_size(message->as.file_offer_create.filename) + 8u + 4u;
    case RELAY_MESSAGE_FILE_OFFER_CREATED:
        return 8u + 8u + 4u;
    case RELAY_MESSAGE_FILE_OFFER_PUBLISHED:
        return 8u + 8u + string_wire_size(message->as.file_offer_published.sender_name)
            + string_wire_size(message->as.file_offer_published.filename) + 8u + 4u;
    case RELAY_MESSAGE_FILE_OFFER_RESPONSE:
        return 8u + 1u;
    case RELAY_MESSAGE_FILE_TRANSFER_READY:
        return 8u + 2u;
    case RELAY_MESSAGE_FILE_CHUNK:
        return 8u + 8u + message->as.file_chunk.data_length;
    case RELAY_MESSAGE_FILE_TRANSFER_END:
        return 8u + 8u;
    case RELAY_MESSAGE_FILE_DELIVERY_RESULT:
        return 8u + 1u + string_wire_size(message->as.file_delivery_result.reason);
    case RELAY_MESSAGE_FILE_DELIVERY_UPDATE:
        return 8u + 8u + string_wire_size(message->as.file_delivery_update.recipient_name)
            + 1u + string_wire_size(message->as.file_delivery_update.reason);
    case RELAY_MESSAGE_FILE_OFFER_DECLINED:
        return 8u;
    case RELAY_MESSAGE_FILE_TRANSFER_CANCEL:
        return 8u + string_wire_size(message->as.file_transfer_cancel.reason);
    case RELAY_MESSAGE_ACTION_REJECTED:
        return 1u + 8u + string_wire_size(message->as.action_rejected.reason);
    }
    return 0;
}

static bool encode_payload(Writer* writer, const RelayMessage* message)
{
    switch (message->type) {
    case RELAY_MESSAGE_HELLO:
        return write_u16(writer, message->as.hello.version)
            && write_string(writer, message->as.hello.display_name);
    case RELAY_MESSAGE_WELCOME:
        return write_u64(writer, message->as.welcome.participant_id);
    case RELAY_MESSAGE_CHAT_SEND:
        return write_string(writer, message->as.chat_send.text);
    case RELAY_MESSAGE_CHAT_DELIVER:
        return write_u64(writer, message->as.chat_deliver.participant_id)
            && write_string(writer, message->as.chat_deliver.display_name)
            && write_string(writer, message->as.chat_deliver.text);
    case RELAY_MESSAGE_FILE_OFFER_CREATE:
        return write_u64(writer, message->as.file_offer_create.request_id)
            && write_string(writer, message->as.file_offer_create.filename)
            && write_u64(writer, message->as.file_offer_create.total_size)
            && write_u32(writer, message->as.file_offer_create.chunk_size);
    case RELAY_MESSAGE_FILE_OFFER_CREATED:
        return write_u64(writer, message->as.file_offer_created.request_id)
            && write_u64(writer, message->as.file_offer_created.offer_id)
            && write_u32(writer, message->as.file_offer_created.offer_window_ms);
    case RELAY_MESSAGE_FILE_OFFER_PUBLISHED:
        return write_u64(writer, message->as.file_offer_published.offer_id)
            && write_u64(writer, message->as.file_offer_published.sender_id)
            && write_string(writer, message->as.file_offer_published.sender_name)
            && write_string(writer, message->as.file_offer_published.filename)
            && write_u64(writer, message->as.file_offer_published.total_size)
            && write_u32(writer, message->as.file_offer_published.offer_window_ms);
    case RELAY_MESSAGE_FILE_OFFER_RESPONSE:
        return write_u64(writer, message->as.file_offer_response.offer_id)
            && write_u8(writer, message->as.file_offer_response.accepted ? 1u : 0u);
    case RELAY_MESSAGE_FILE_TRANSFER_READY:
        return write_u64(writer, message->as.file_transfer_ready.offer_id)
            && write_u16(writer, message->as.file_transfer_ready.recipient_count);
    case RELAY_MESSAGE_FILE_CHUNK:
        return write_u64(writer, message->as.file_chunk.offer_id)
            && write_u64(writer, message->as.file_chunk.offset)
            && write_bytes(writer, message->as.file_chunk.data, message->as.file_chunk.data_length);
    case RELAY_MESSAGE_FILE_TRANSFER_END:
        return write_u64(writer, message->as.file_transfer_end.offer_id)
            && write_u64(writer, message->as.file_transfer_end.total_size);
    case RELAY_MESSAGE_FILE_DELIVERY_RESULT:
        return write_u64(writer, message->as.file_delivery_result.offer_id)
            && write_u8(writer, message->as.file_delivery_result.success ? 1u : 0u)
            && write_string(writer, message->as.file_delivery_result.reason);
    case RELAY_MESSAGE_FILE_DELIVERY_UPDATE:
        return write_u64(writer, message->as.file_delivery_update.offer_id)
            && write_u64(writer, message->as.file_delivery_update.recipient_id)
            && write_string(writer, message->as.file_delivery_update.recipient_name)
            && write_u8(writer, message->as.file_delivery_update.success ? 1u : 0u)
            && write_string(writer, message->as.file_delivery_update.reason);
    case RELAY_MESSAGE_FILE_OFFER_DECLINED:
        return write_u64(writer, message->as.file_offer_declined.offer_id);
    case RELAY_MESSAGE_FILE_TRANSFER_CANCEL:
        return write_u64(writer, message->as.file_transfer_cancel.offer_id)
            && write_string(writer, message->as.file_transfer_cancel.reason);
    case RELAY_MESSAGE_ACTION_REJECTED:
        return write_u8(writer, (uint8_t)message->as.action_rejected.rejected_type)
            && write_u64(writer, message->as.action_rejected.correlation_id)
            && write_string(writer, message->as.action_rejected.reason);
    }
    return false;
}

bool protocol_encode(const RelayMessage* message, uint8_t** frame, size_t* frame_length)
{
    if (!frame || !frame_length)
        return false;
    *frame = NULL;
    *frame_length = 0;
    if (!protocol_message_is_valid(message))
        return false;

    size_t body_length = payload_size(message);
    if (body_length == 0 || body_length > PROTOCOL_MAX_PAYLOAD || body_length > UINT32_MAX)
        return false;

    size_t total_length = PROTOCOL_FRAME_HEADER_SIZE + body_length;
    uint8_t* bytes = malloc(total_length);
    if (!bytes)
        return false;

    Writer writer = { .bytes = bytes, .length = total_length, .position = 0 };
    if (!write_u8(&writer, (uint8_t)message->type)
        || !write_u32(&writer, (uint32_t)body_length)
        || !encode_payload(&writer, message)
        || writer.position != total_length) {
        free(bytes);
        return false;
    }

    *frame = bytes;
    *frame_length = total_length;
    return true;
}

static bool decode_payload(RelayMessageType type, const uint8_t* payload, size_t payload_length,
    RelayMessage* message)
{
    Reader reader = { .bytes = payload, .length = payload_length, .position = 0 };
    memset(message, 0, sizeof(*message));
    message->type = type;
    uint8_t flag = 0;
    uint8_t rejected_type = 0;

    switch (type) {
    case RELAY_MESSAGE_HELLO:
        if (!read_u16(&reader, &message->as.hello.version)
            || !read_string(&reader, message->as.hello.display_name, sizeof(message->as.hello.display_name)))
            return false;
        break;
    case RELAY_MESSAGE_WELCOME:
        if (!read_u64(&reader, &message->as.welcome.participant_id))
            return false;
        break;
    case RELAY_MESSAGE_CHAT_SEND:
        if (!read_string(&reader, message->as.chat_send.text, sizeof(message->as.chat_send.text)))
            return false;
        break;
    case RELAY_MESSAGE_CHAT_DELIVER:
        if (!read_u64(&reader, &message->as.chat_deliver.participant_id)
            || !read_string(&reader, message->as.chat_deliver.display_name, sizeof(message->as.chat_deliver.display_name))
            || !read_string(&reader, message->as.chat_deliver.text, sizeof(message->as.chat_deliver.text)))
            return false;
        break;
    case RELAY_MESSAGE_FILE_OFFER_CREATE:
        if (!read_u64(&reader, &message->as.file_offer_create.request_id)
            || !read_string(&reader, message->as.file_offer_create.filename, sizeof(message->as.file_offer_create.filename))
            || !read_u64(&reader, &message->as.file_offer_create.total_size)
            || !read_u32(&reader, &message->as.file_offer_create.chunk_size))
            return false;
        break;
    case RELAY_MESSAGE_FILE_OFFER_CREATED:
        if (!read_u64(&reader, &message->as.file_offer_created.request_id)
            || !read_u64(&reader, &message->as.file_offer_created.offer_id)
            || !read_u32(&reader, &message->as.file_offer_created.offer_window_ms))
            return false;
        break;
    case RELAY_MESSAGE_FILE_OFFER_PUBLISHED:
        if (!read_u64(&reader, &message->as.file_offer_published.offer_id)
            || !read_u64(&reader, &message->as.file_offer_published.sender_id)
            || !read_string(&reader, message->as.file_offer_published.sender_name, sizeof(message->as.file_offer_published.sender_name))
            || !read_string(&reader, message->as.file_offer_published.filename, sizeof(message->as.file_offer_published.filename))
            || !read_u64(&reader, &message->as.file_offer_published.total_size)
            || !read_u32(&reader, &message->as.file_offer_published.offer_window_ms))
            return false;
        break;
    case RELAY_MESSAGE_FILE_OFFER_RESPONSE:
        if (!read_u64(&reader, &message->as.file_offer_response.offer_id) || !read_u8(&reader, &flag) || flag > 1)
            return false;
        message->as.file_offer_response.accepted = flag != 0;
        break;
    case RELAY_MESSAGE_FILE_TRANSFER_READY:
        if (!read_u64(&reader, &message->as.file_transfer_ready.offer_id)
            || !read_u16(&reader, &message->as.file_transfer_ready.recipient_count))
            return false;
        break;
    case RELAY_MESSAGE_FILE_CHUNK:
        if (!read_u64(&reader, &message->as.file_chunk.offer_id)
            || !read_u64(&reader, &message->as.file_chunk.offset))
            return false;
        if (reader.length - reader.position == 0 || reader.length - reader.position > PROTOCOL_FILE_CHUNK_MAX)
            return false;
        message->as.file_chunk.data_length = (uint32_t)(reader.length - reader.position);
        message->as.file_chunk.data = malloc(message->as.file_chunk.data_length);
        if (!message->as.file_chunk.data
            || !read_bytes(&reader, message->as.file_chunk.data, message->as.file_chunk.data_length)) {
            protocol_message_destroy(message);
            return false;
        }
        break;
    case RELAY_MESSAGE_FILE_TRANSFER_END:
        if (!read_u64(&reader, &message->as.file_transfer_end.offer_id)
            || !read_u64(&reader, &message->as.file_transfer_end.total_size))
            return false;
        break;
    case RELAY_MESSAGE_FILE_DELIVERY_RESULT:
        if (!read_u64(&reader, &message->as.file_delivery_result.offer_id)
            || !read_u8(&reader, &flag) || flag > 1
            || !read_string(&reader, message->as.file_delivery_result.reason, sizeof(message->as.file_delivery_result.reason)))
            return false;
        message->as.file_delivery_result.success = flag != 0;
        break;
    case RELAY_MESSAGE_FILE_DELIVERY_UPDATE:
        if (!read_u64(&reader, &message->as.file_delivery_update.offer_id)
            || !read_u64(&reader, &message->as.file_delivery_update.recipient_id)
            || !read_string(&reader, message->as.file_delivery_update.recipient_name, sizeof(message->as.file_delivery_update.recipient_name))
            || !read_u8(&reader, &flag) || flag > 1
            || !read_string(&reader, message->as.file_delivery_update.reason, sizeof(message->as.file_delivery_update.reason)))
            return false;
        message->as.file_delivery_update.success = flag != 0;
        break;
    case RELAY_MESSAGE_FILE_OFFER_DECLINED:
        if (!read_u64(&reader, &message->as.file_offer_declined.offer_id))
            return false;
        break;
    case RELAY_MESSAGE_FILE_TRANSFER_CANCEL:
        if (!read_u64(&reader, &message->as.file_transfer_cancel.offer_id)
            || !read_string(&reader, message->as.file_transfer_cancel.reason, sizeof(message->as.file_transfer_cancel.reason)))
            return false;
        break;
    case RELAY_MESSAGE_ACTION_REJECTED:
        if (!read_u8(&reader, &rejected_type) || !message_type_is_valid(rejected_type)
            || !read_u64(&reader, &message->as.action_rejected.correlation_id)
            || !read_string(&reader, message->as.action_rejected.reason, sizeof(message->as.action_rejected.reason)))
            return false;
        message->as.action_rejected.rejected_type = (RelayMessageType)rejected_type;
        break;
    }

    return reader.position == reader.length && protocol_message_is_valid(message);
}

void protocol_decoder_init(ProtocolDecoder* decoder)
{
    if (decoder)
        memset(decoder, 0, sizeof(*decoder));
}

void protocol_decoder_reset(ProtocolDecoder* decoder)
{
    if (decoder)
        decoder->length = 0;
}

void protocol_decoder_destroy(ProtocolDecoder* decoder)
{
    if (!decoder)
        return;
    free(decoder->buffer);
    memset(decoder, 0, sizeof(*decoder));
}

static bool decoder_reserve(ProtocolDecoder* decoder, size_t needed)
{
    if (needed <= decoder->capacity)
        return true;
    if (needed > PROTOCOL_MAX_PAYLOAD + PROTOCOL_FRAME_HEADER_SIZE)
        return false;
    size_t capacity = decoder->capacity ? decoder->capacity : 4096u;
    while (capacity < needed) {
        if (capacity > (PROTOCOL_MAX_PAYLOAD + PROTOCOL_FRAME_HEADER_SIZE) / 2u) {
            capacity = PROTOCOL_MAX_PAYLOAD + PROTOCOL_FRAME_HEADER_SIZE;
            break;
        }
        capacity *= 2u;
    }
    uint8_t* resized = realloc(decoder->buffer, capacity);
    if (!resized)
        return false;
    decoder->buffer = resized;
    decoder->capacity = capacity;
    return true;
}

bool protocol_decoder_feed(ProtocolDecoder* decoder, const uint8_t* bytes, size_t length,
    RelayMessageHandler handler, void* context)
{
    if (!decoder || !handler || (length > 0 && !bytes))
        return false;
    if (length > PROTOCOL_MAX_PAYLOAD + PROTOCOL_FRAME_HEADER_SIZE - decoder->length
        || !decoder_reserve(decoder, decoder->length + length)) {
        protocol_decoder_reset(decoder);
        return false;
    }
    if (length > 0) {
        memcpy(decoder->buffer + decoder->length, bytes, length);
        decoder->length += length;
    }

    size_t processed = 0;
    while (decoder->length - processed >= PROTOCOL_FRAME_HEADER_SIZE) {
        Reader header = {
            .bytes = decoder->buffer + processed,
            .length = PROTOCOL_FRAME_HEADER_SIZE,
            .position = 0
        };
        uint8_t type = 0;
        uint32_t payload_length = 0;
        if (!read_u8(&header, &type) || !read_u32(&header, &payload_length)
            || !message_type_is_valid(type) || payload_length == 0
            || payload_length > PROTOCOL_MAX_PAYLOAD) {
            protocol_decoder_reset(decoder);
            return false;
        }
        size_t frame_length = PROTOCOL_FRAME_HEADER_SIZE + (size_t)payload_length;
        if (decoder->length - processed < frame_length)
            break;

        RelayMessage message;
        if (!decode_payload((RelayMessageType)type,
                decoder->buffer + processed + PROTOCOL_FRAME_HEADER_SIZE,
                payload_length, &message)) {
            protocol_decoder_reset(decoder);
            return false;
        }
        handler(context, &message);
        protocol_message_destroy(&message);
        processed += frame_length;
    }

    if (processed > 0) {
        decoder->length -= processed;
        memmove(decoder->buffer, decoder->buffer + processed, decoder->length);
    }
    return true;
}

void protocol_message_destroy(RelayMessage* message)
{
    if (!message)
        return;
    if (message->type == RELAY_MESSAGE_FILE_CHUNK)
        free(message->as.file_chunk.data);
    memset(message, 0, sizeof(*message));
}
