#include "client_logic.h"
#include "file_transfer.h"
#include "file_transfer_state.h"
#include "message.h"
#include "packet_queue.h"
#include <errno.h>
#include <raylib.h> // For TraceLog
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_RECV_BUFFER (64 * 1024)

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
static char g_message_recv[NETWORK_RECV_BUFFER];

// Forward declarations
static void handle_packet(MessageQueue* mq, uint8_t type, const char* data, size_t len);
static void handle_text_payload(MessageQueue* mq, char* message);
static void parse_and_add_chat_message(MessageQueue* mq, const char* incoming);
static void handle_file_packet(MessageQueue* mq, int type, const char* data, size_t len);

// Exposed function
void reset_client_stream(void)
{
    incoming_stream_len = 0;
    stream_state = STATE_HEADER;
    bytes_needed = sizeof(PacketHeader);
    memset(&current_header, 0, sizeof(current_header));
}

bool process_incoming_stream(MessageQueue* mq, const char* data, size_t len)
{
    if (len == 0)
        return true;

    if (!mq || !data)
        return false;

    if (incoming_stream_len > INCOMING_STREAM_CAPACITY || len > INCOMING_STREAM_CAPACITY - incoming_stream_len) {
        TraceLog(LOG_WARNING, "Incoming stream exceeded its protocol limit");
        reset_client_stream();
        return false;
    }

    memcpy(incoming_stream + incoming_stream_len, data, len);
    incoming_stream_len += len;

    size_t processed = 0;
    while (incoming_stream_len - processed >= bytes_needed) {
        if (stream_state == STATE_HEADER) {
            memcpy(&current_header, incoming_stream + processed, sizeof(PacketHeader));
            current_header.length = ntohl(current_header.length);
            processed += sizeof(PacketHeader);

            if (!protocol_packet_is_valid(current_header.type, current_header.length)) {
                TraceLog(LOG_WARNING, "Rejected packet type=%u length=%u",
                    current_header.type, current_header.length);
                reset_client_stream();
                return false;
            }

            stream_state = STATE_BODY;
            bytes_needed = current_header.length;
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
    return true;
}

bool update_client_state(ClientConnection* conn, MessageQueue* mq)
{
    if (!conn || !atomic_load(&conn->connected))
        return false;

    should_scroll_to_bottom = false;
    int recv_status = 0;
    do {
        // Backpressure: If buffer is nearly full (> 95%), stop reading to let processor catch up.
        if (incoming_stream_len > INCOMING_STREAM_CAPACITY - NETWORK_RECV_BUFFER) {
            // TraceLog(LOG_WARNING, "Backpressure applied: Buffer full (%zu/%d)", incoming_stream_len, INCOMING_STREAM_CAPACITY);
            break;
        }

        recv_status = recv_msg(conn, g_message_recv, sizeof(g_message_recv));
        if (recv_status > 0) {
            if (!process_incoming_stream(mq, g_message_recv, (size_t)recv_status)) {
                recv_status = -1;
                add_message(mq, "SYSTEM", "Server sent an invalid packet; disconnected for safety");
                should_scroll_to_bottom = true;
                break;
            }
        }
    } while (recv_status > 0);

    if (recv_status < 0) {
        atomic_store(&conn->connected, false);
        // In GUI we did abort_all_transfers() and show_error().
        // Logic layer can abort transfers, but showing error is UI.
        // We'll leave UI notification to the caller by checking conn->connected.
        // Or we can add a system message.
        abort_all_transfers();
        disconnect_from_server(conn);
        reset_client_stream();
        if (!should_scroll_to_bottom)
            add_message(mq, "SYSTEM", "Connection lost");
        should_scroll_to_bottom = true;
    }

    return should_scroll_to_bottom;
}

// Helpers

static void handle_packet(MessageQueue* mq, uint8_t type, const char* data, size_t len)
{
    if (type == PACKET_TYPE_TEXT) {
        if (len > PROTOCOL_TEXT_MAX_LEN)
            return;
        char* buffer = (char*)malloc(len + 1);
        if (!buffer)
            return;
        if (len > 0)
            memcpy(buffer, data, len);
        buffer[len] = '\0';
        handle_text_payload(mq, buffer);
        free(buffer);
    } else if (type >= PACKET_TYPE_FILE_START && type <= PACKET_TYPE_FILE_ACCEPT) {
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

    size_t len = strnlen(incoming, PROTOCOL_TEXT_MAX_LEN);
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
        if (!protocol_text_is_valid(data, len))
            return;
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

        if (!sender || !file_id || !filename || !total_bytes_str || !protocol_username_is_valid(sender) || !protocol_file_id_is_valid(file_id) || filename[0] == '\0') {
            TraceLog(LOG_ERROR, "FILE_START validation failed - missing fields");
            return;
        }

        char* size_end = NULL;
        errno = 0;
        unsigned long long total_bytes = strtoull(total_bytes_str, &size_end, 10);
        if (errno != 0 || size_end == total_bytes_str || *size_end != '\0')
            return;

        if (chunk_size_str) {
            char* chunk_end = NULL;
            errno = 0;
            unsigned long chunk_size = strtoul(chunk_size_str, &chunk_end, 10);
            if (errno != 0 || chunk_end == chunk_size_str || *chunk_end != '\0' || chunk_size == 0 || chunk_size > FILE_CHUNK_SIZE)
                return;
        }

        if (total_bytes > FILE_TRANSFER_MAX_SIZE || strlen(file_id) >= FILE_ID_LEN || strlen(filename) >= FILE_NAME_MAX_LEN || strlen(sender) > PROTOCOL_USERNAME_MAX_LEN)
            return;

        // Check if transfer already exists (duplicate FILE_START)
        IncomingTransfer* slot = get_incoming_transfer(file_id);
        if (slot) {
            TraceLog(LOG_WARNING, "Duplicate FILE_START for %s, ignoring", file_id);
            return;
        }

        slot = get_free_incoming();
        if (!slot) {
            add_message(mq, "SYSTEM", "Too many incoming transfers, discarding file");
            return;
        }

        // Initialize transfer in PENDING state - don't open file yet
        memset(slot, 0, sizeof(*slot));
        slot->state = TRANSFER_STATE_PENDING;
        strncpy(slot->file_id, file_id, sizeof(slot->file_id) - 1);
        strncpy(slot->filename, filename, sizeof(slot->filename) - 1);
        sanitize_filename(slot->filename);
        strncpy(slot->sender, sender, sizeof(slot->sender) - 1);
        slot->total_bytes = (size_t)total_bytes;
        slot->received_bytes = 0;
        slot->fp = NULL;

        char buf[512];
        snprintf(buf, sizeof(buf), "Incoming file: %.200s from %.120s (%.2f MB) - Accept or Reject?",
            slot->filename,
            slot->sender,
            (double)slot->total_bytes / (1024.0 * 1024.0));
        add_message(mq, "SYSTEM", buf);

    } else if (type == PACKET_TYPE_FILE_CHUNK) {
        if (len <= FILE_ID_LEN)
            return;

        char file_id[FILE_ID_LEN + 1];
        memcpy(file_id, data, FILE_ID_LEN);
        file_id[FILE_ID_LEN] = '\0';
        if (!protocol_file_id_is_valid(file_id))
            return;

        const char* chunk_data = data + FILE_ID_LEN;
        size_t chunk_len = len - FILE_ID_LEN;

        IncomingTransfer* slot = get_incoming_transfer(file_id);
        if (!slot)
            return;

        // If transfer is pending, just discard chunks until user accepts
        if (slot->state == TRANSFER_STATE_PENDING) {
            // Buffer chunks? For now, we discard - sender will need to resend
            // This is a design choice: we could buffer, but that uses memory
            return;
        }

        // If transfer was rejected, also discard
        if (slot->state == TRANSFER_STATE_REJECTED) {
            return;
        }

        // Only write if accepted and file is open
        if (slot->state != TRANSFER_STATE_ACCEPTED || !slot->fp)
            return;

        if (slot->received_bytes > slot->total_bytes || chunk_len > slot->total_bytes - slot->received_bytes) {
            finalize_incoming_transfer(slot, false, "received more than expected");
            return;
        }

        size_t written = fwrite(chunk_data, 1, chunk_len, slot->fp);
        if (written != chunk_len) {
            finalize_incoming_transfer(slot, false, "disk write error");
            return;
        }

        slot->received_bytes += written;

    } else if (type == PACKET_TYPE_FILE_END) {
        char buffer[512];
        if (!protocol_text_is_valid(data, len))
            return;
        if (len >= sizeof(buffer))
            len = sizeof(buffer) - 1;
        memcpy(buffer, data, len);
        buffer[len] = '\0';

        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        (void)sender;

        if (!file_id || !protocol_file_id_is_valid(file_id))
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
        if (!protocol_text_is_valid(data, len))
            return;
        if (len >= sizeof(buffer))
            len = sizeof(buffer) - 1;
        memcpy(buffer, data, len);
        buffer[len] = '\0';

        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        const char* reason = strtok_r(NULL, "|", &save_ptr);
        (void)sender;

        if (!file_id || !protocol_file_id_is_valid(file_id))
            return;

        // Check if this is an incoming transfer being aborted by sender
        IncomingTransfer* in_slot = get_incoming_transfer(file_id);
        if (in_slot) {
            finalize_incoming_transfer(in_slot, false, reason ? reason : "Aborted by sender");
        }

        // Also check if receiver rejected our outgoing transfer
        OutgoingTransfer* out_slot = get_outgoing_transfer(file_id);
        if (out_slot && out_slot->active) {
            char buf[512];
            snprintf(buf, sizeof(buf), "Transfer rejected: %.200s (%s)",
                out_slot->filename, reason ? reason : "Rejected by receiver");
            add_message(mq, "SYSTEM", buf);
            close_outgoing_transfer(NULL, out_slot, NULL); // Don't send another abort
        }

    } else if (type == PACKET_TYPE_FILE_ACCEPT) {
        // Receiver accepted our file transfer - start sending chunks
        char buffer[512];
        if (!protocol_text_is_valid(data, len))
            return;
        if (len >= sizeof(buffer))
            len = sizeof(buffer) - 1;
        memcpy(buffer, data, len);
        buffer[len] = '\0';

        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        (void)sender;

        if (!file_id || !protocol_file_id_is_valid(file_id))
            return;

        OutgoingTransfer* slot = get_outgoing_transfer(file_id);
        if (slot && slot->active && !slot->accepted) {
            slot->accepted = true;
            TraceLog(LOG_INFO, "Transfer %s accepted by receiver, starting data transfer", file_id);

            char buf[512];
            snprintf(buf, sizeof(buf), "Transfer accepted: %.200s - sending data...", slot->filename);
            add_message(mq, "SYSTEM", buf);
        }
    }
}
