#include "client_logic.h"
#include "file_transfer_state.h"
#include "file_transfer.h"
#include "packet_queue.h"
#include "message.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <raylib.h> // For TraceLog

#ifndef MSG_BUFFER
#define MSG_BUFFER 4096
#endif

// Internal state variables (moved from client_gui.c)
// Note: incoming_stream and incoming_stream_len are in file_transfer_state.h/c

typedef enum {
    STATE_HEADER,
    STATE_BODY
} StreamState;

static StreamState stream_state = STATE_HEADER;
static PacketHeader current_header;
static size_t bytes_needed = sizeof(PacketHeader);

static bool should_scroll_to_bottom = false; // We might need to expose this or handle it via return value

// Buffers
static char g_message_recv[MSG_BUFFER];

// Forward declarations
static void handle_packet(MessageQueue* mq, uint8_t type, const char* data, size_t len);
static void handle_text_payload(MessageQueue* mq, char* message);
static void parse_and_add_chat_message(MessageQueue* mq, const char* incoming);
static void handle_file_packet(MessageQueue* mq, int type, const char* data, size_t len);

// Exposed function
void process_incoming_stream(MessageQueue* mq, const char* data, size_t len)
{
    if (len == 0)
        return;

    if (incoming_stream_len + len > INCOMING_STREAM_CAPACITY) {
        TraceLog(LOG_WARNING, "Incoming stream overflow, dropping data");
        incoming_stream_len = 0;
        stream_state = STATE_HEADER;
        bytes_needed = sizeof(PacketHeader);
        return;
    }

    memcpy(incoming_stream + incoming_stream_len, data, len);
    incoming_stream_len += len;

    size_t processed = 0;
    while (incoming_stream_len - processed >= bytes_needed) {
        if (stream_state == STATE_HEADER) {
            memcpy(&current_header, incoming_stream + processed, sizeof(PacketHeader));
            current_header.length = ntohl(current_header.length);
            processed += sizeof(PacketHeader);

            if (current_header.length > 0) {
                stream_state = STATE_BODY;
                bytes_needed = current_header.length;
            } else {
                handle_packet(mq, current_header.type, NULL, 0);
                stream_state = STATE_HEADER;
                bytes_needed = sizeof(PacketHeader);
            }
        } else if (stream_state == STATE_BODY) {
            handle_packet(mq, current_header.type, incoming_stream + processed, current_header.length);
            processed += current_header.length;
            stream_state = STATE_HEADER;
            bytes_needed = sizeof(PacketHeader);
        }
    }

    if (processed > 0) {
        incoming_stream_len -= processed;
        memmove(incoming_stream, incoming_stream + processed, incoming_stream_len);
    }
}

bool update_client_state(ClientConnection* conn, MessageQueue* mq)
{
    if (!conn || !conn->connected) return false;

    should_scroll_to_bottom = false;
    int recv_status = 0;
    do {
        // Backpressure: If buffer is nearly full (> 95%), stop reading to let processor catch up.
        if (incoming_stream_len > INCOMING_STREAM_CAPACITY - MSG_BUFFER) {
             // TraceLog(LOG_WARNING, "Backpressure applied: Buffer full (%zu/%d)", incoming_stream_len, INCOMING_STREAM_CAPACITY);
            break;
        }

        recv_status = recv_msg(conn, g_message_recv, MSG_BUFFER - 1);
        if (recv_status > 0) {
            process_incoming_stream(mq, g_message_recv, (size_t)recv_status);
        }
    } while (recv_status > 0);

    if (recv_status < 0) {
        conn->connected = false;
        // In GUI we did abort_all_transfers() and show_error(). 
        // Logic layer can abort transfers, but showing error is UI.
        // We'll leave UI notification to the caller by checking conn->connected.
        // Or we can add a system message.
        abort_all_transfers();
        disconnect_from_server(conn);
        add_message(mq, "SYSTEM", "Connection lost");
        should_scroll_to_bottom = true;
    }

    return should_scroll_to_bottom;
}

// Helpers

static void handle_packet(MessageQueue* mq, uint8_t type, const char* data, size_t len)
{
    if (type == PACKET_TYPE_TEXT) {
        if (len >= MSG_BUFFER)
            len = MSG_BUFFER - 1;
        char* buffer = (char*)malloc(len + 1);
        if (!buffer)
            return;
        if (len > 0)
            memcpy(buffer, data, len);
        buffer[len] = '\0';
        handle_text_payload(mq, buffer);
        free(buffer);
    } else if (type >= PACKET_TYPE_FILE_START && type <= PACKET_TYPE_FILE_ABORT) {
        handle_file_packet(mq, type, data, len);
    } else {
        TraceLog(LOG_WARNING, "Unknown packet type: %d", type);
    }
}

static void handle_text_payload(MessageQueue* mq, char* message)
{
    parse_and_add_chat_message(mq, message);
}

static void parse_and_add_chat_message(MessageQueue* mq, const char* incoming)
{
    if (!incoming || *incoming == '\0')
        return;

    size_t len = strnlen(incoming, MSG_BUFFER - 1);
    char* buffer = (char*)malloc(len + 1);
    if (!buffer)
        return;

    memcpy(buffer, incoming, len);
    buffer[len] = '\0';

    char* colon = strchr(buffer, ':');
    if (colon && colon > buffer) {
        size_t sender_len = (size_t)(colon - buffer);
        if (sender_len >= sizeof(mq->messages[0].sender))
            sender_len = sizeof(mq->messages[0].sender) - 1;

        char sender[sizeof(mq->messages[0].sender)];
        memcpy(sender, buffer, sender_len);
        sender[sender_len] = '\0';

        char* text_start = colon + 1;
        while (*text_start == ' ' || *text_start == '\t')
            text_start++;

        add_message(mq, sender, text_start);
    } else {
        add_message(mq, "SYSTEM", buffer);
    }

    free(buffer);
}

static void handle_file_packet(MessageQueue* mq, int type, const char* data, size_t len)
{
    if (type == PACKET_TYPE_FILE_START) {
        char buffer[1024];
        if (len >= sizeof(buffer))
            len = sizeof(buffer) - 1;
        memcpy(buffer, data, len);
        buffer[len] = '\0';

        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        const char* filename = strtok_r(NULL, "|", &save_ptr);
        const char* total_bytes_str = strtok_r(NULL, "|", &save_ptr);
        const char* chunk_size_str = strtok_r(NULL, "|", &save_ptr);

        if (!sender || !file_id || !filename || !total_bytes_str) {
            TraceLog(LOG_ERROR, "FILE_START validation failed - missing fields");
            return;
        }

        unsigned long long total_bytes = strtoull(total_bytes_str, NULL, 10);
        (void)chunk_size_str;

        if (total_bytes > FILE_TRANSFER_MAX_SIZE)
            return;

        IncomingTransfer* slot = get_incoming_transfer(file_id);
        if (!slot)
            slot = get_free_incoming();
        if (!slot) {
            add_message(mq, "SYSTEM", "Too many incoming transfers, discarding file");
            return;
        }

        ensure_receive_directory();

        memset(slot, 0, sizeof(*slot));
        slot->active = true;
        strncpy(slot->file_id, file_id, sizeof(slot->file_id) - 1);
        strncpy(slot->filename, filename, sizeof(slot->filename) - 1);
        sanitize_filename(slot->filename);
        strncpy(slot->sender, sender, sizeof(slot->sender) - 1);
        slot->total_bytes = (size_t)total_bytes;
        slot->received_bytes = 0;

        char save_path[FILE_PATH_MAX_LEN];
        snprintf(save_path, sizeof(save_path), "received/%s", slot->filename);

        int duplicate_index = 1;
        while (access(save_path, F_OK) == 0 && duplicate_index < 1000) {
            snprintf(save_path, sizeof(save_path), "received/%s(%d)", slot->filename, duplicate_index++);
        }

        strncpy(slot->save_path, save_path, sizeof(slot->save_path) - 1);
        slot->save_path[sizeof(slot->save_path) - 1] = '\0';
        slot->fp = fopen(slot->save_path, "wb");
        if (!slot->fp) {
            add_message(mq, "SYSTEM", "Failed to open file for writing");
            slot->active = false;
            return;
        }

        char buf[512];
        snprintf(buf, sizeof(buf), "Receiving %.200s from %.120s (%.2f MB)",
            slot->filename,
            slot->sender,
            slot->total_bytes / (1024.0 * 1024.0));
        add_message(mq, "SYSTEM", buf);

    } else if (type == PACKET_TYPE_FILE_CHUNK) {
        if (len <= FILE_ID_LEN)
            return;

        char file_id[FILE_ID_LEN + 1];
        memcpy(file_id, data, FILE_ID_LEN);
        file_id[FILE_ID_LEN] = '\0';

        const char* chunk_data = data + FILE_ID_LEN;
        size_t chunk_len = len - FILE_ID_LEN;

        IncomingTransfer* slot = get_incoming_transfer(file_id);
        if (!slot || !slot->fp)
            return;

        size_t written = fwrite(chunk_data, 1, chunk_len, slot->fp);
        if (written != chunk_len) {
            finalize_incoming_transfer(slot, false, "disk write error");
            return;
        }

        slot->received_bytes += written;
        if (slot->received_bytes > slot->total_bytes) {
            finalize_incoming_transfer(slot, false, "received more than expected");
        }

    } else if (type == PACKET_TYPE_FILE_END) {
        char buffer[512];
        if (len >= sizeof(buffer))
            len = sizeof(buffer) - 1;
        memcpy(buffer, data, len);
        buffer[len] = '\0';

        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        (void)sender;

        if (!file_id)
            return;

        IncomingTransfer* slot = get_incoming_transfer(file_id);
        if (!slot)
            return;

        if (slot->received_bytes == slot->total_bytes) {
            finalize_incoming_transfer(slot, true, NULL);
        } else {
            finalize_incoming_transfer(slot, false, "incomplete file");
        }

    } else if (type == PACKET_TYPE_FILE_ABORT) {
        char buffer[512];
        if (len >= sizeof(buffer))
            len = sizeof(buffer) - 1;
        memcpy(buffer, data, len);
        buffer[len] = '\0';

        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        const char* reason = strtok_r(NULL, "|", &save_ptr);
        (void)sender;

        if (!file_id)
            return;
        IncomingTransfer* slot = get_incoming_transfer(file_id);
        if (slot)
            finalize_incoming_transfer(slot, false, reason ? reason : "Aborted by sender");
    }
}
