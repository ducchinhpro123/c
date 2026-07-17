#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The protocol is intentionally small and bounded. Every peer must validate a
// header before allocating memory or waiting for its payload.
#define PROTOCOL_USERNAME_MAX_LEN 24u
#define PROTOCOL_TEXT_MAX_LEN 4096u
#define PROTOCOL_CHAT_CONTENT_MAX_LEN 4000u
#define PROTOCOL_FILE_ID_LEN 32u
#define PROTOCOL_FILE_CHUNK_MAX (1024u * 1024u)
#define PROTOCOL_METADATA_MAX_LEN 1024u
#define PROTOCOL_MAX_PAYLOAD (PROTOCOL_FILE_ID_LEN + PROTOCOL_FILE_CHUNK_MAX)

typedef enum {
    PACKET_TYPE_TEXT = 0,
    PACKET_TYPE_FILE_START = 1,
    PACKET_TYPE_FILE_CHUNK = 2,
    PACKET_TYPE_FILE_END = 3,
    PACKET_TYPE_FILE_ABORT = 4,
    PACKET_TYPE_FILE_ACCEPT = 5
} PacketType;

#pragma pack(push, 1)
typedef struct {
    uint8_t type;
    uint32_t length; // Network byte order on the wire.
} PacketHeader;
#pragma pack(pop)

static inline bool protocol_packet_is_valid(uint8_t type, uint32_t length)
{
    switch (type) {
    case PACKET_TYPE_TEXT:
        return length > 0 && length <= PROTOCOL_TEXT_MAX_LEN;
    case PACKET_TYPE_FILE_START:
        return length > 0 && length <= PROTOCOL_METADATA_MAX_LEN;
    case PACKET_TYPE_FILE_CHUNK:
        return length > PROTOCOL_FILE_ID_LEN && length <= PROTOCOL_MAX_PAYLOAD;
    case PACKET_TYPE_FILE_END:
    case PACKET_TYPE_FILE_ABORT:
    case PACKET_TYPE_FILE_ACCEPT:
        return length > 0 && length <= PROTOCOL_METADATA_MAX_LEN;
    default:
        return false;
    }
}

static inline bool protocol_username_is_valid(const char* username)
{
    if (!username)
        return false;

    size_t len = 0;
    for (; username[len] != '\0'; ++len) {
        unsigned char c = (unsigned char)username[len];
        if (len >= PROTOCOL_USERNAME_MAX_LEN || c < 0x20 || c == 0x7f || c == ':' || c == '|')
            return false;
    }

    return len > 0 && username[0] != ' ' && username[len - 1] != ' ';
}

static inline bool protocol_text_is_valid(const char* text, size_t len)
{
    if (!text || len == 0 || len > PROTOCOL_TEXT_MAX_LEN)
        return false;

    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\0' || (c < 0x20 && c != '\n' && c != '\t'))
            return false;
    }
    return true;
}

static inline bool protocol_file_id_is_valid(const char* file_id)
{
    if (!file_id)
        return false;
    size_t len = 0;
    for (; file_id[len] != '\0'; ++len) {
        char c = file_id[len];
        if (len >= PROTOCOL_FILE_ID_LEN - 1 || !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_'))
            return false;
    }
    return len > 0;
}

#endif // PROTOCOL_H
