// platform.h MUST be first to define NOGDI/NOUSER before any Windows headers
#include "platform.h"

#include <raylib.h>

#include "client_network.h"
#include "file_transfer.h"
#include "file_transfer_state.h"
#include "ui_components.h"
#include "message.h"
#include "packet_queue.h"
#include "warning_dialog.h"
#include "window.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Raygui implementation is now in ui_components.c or shared, but main might need it if using directly
// We moved implementation to ui_components.c to avoid dupes

#define FPS 60
#define USERNAME_BUFFER 64
#define FILENAME_TRUNCATE_THRESHOLD 25
#define MAX_USERNAME_LENGTH 24

Message messages[MAX_MESSAGES];

// For buffering
static unsigned char* g_chunk_buffer = NULL;
static unsigned char* g_payload_buffer = NULL;

MessageQueue g_mq = { 0 }; // Init message queue

// Buffers that must not live on the small Windows thread stack
static char g_message_recv[MSG_BUFFER];

#define TRANSFER_PUMP_BUDGET_MS 8.0f
// #define INCOMING_STREAM_CAPACITY (MSG_BUFFER * 5)

static bool should_scroll_to_bottom = false;

// Function Prototypes (Locally scoped logic)
static void start_outgoing_transfer(ClientConnection* conn, const char* file_path);
static void pump_outgoing_transfers(ClientConnection* conn);
static void process_incoming_stream(const char* data, size_t len);
static void handle_file_packet(int type, const char* data, size_t len);
static void handle_text_payload(char* message);
static void parse_and_add_chat_message(const char* incoming);
static void process_file_drop(ClientConnection* conn);
static bool ensure_transfer_buffers();
static void ensure_asset_workdir(void);

// State machine variables
typedef enum {
    STATE_HEADER,
    STATE_BODY
} StreamState;

static StreamState stream_state = STATE_HEADER;
static PacketHeader current_header;
static size_t bytes_needed = sizeof(PacketHeader);

// Main function
int main()
{
    if (init_network() != 0) {
        fprintf(stderr, "Failed to initialize network\n");
        return 1;
    }

    ensure_asset_workdir();

    const char* window_title = "C&F";
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, window_title);
    SetTargetFPS(FPS);
    Font comic_font = LoadFont("resources/fonts/ComicMono.ttf");

    char server_ip[16] = "127.0.0.1";
    int server_port = 8898;
    char port_str[6] = "8898";
    char username[USERNAME_BUFFER];
    memset(username, 0, sizeof(username));

    bool is_connected = false;
    bool debugging = false;

    // Init client connection
    ClientConnection conn;
    init_client_connection(&conn);

    init_message_queue(&g_mq);

    // Ensure received/ exists relative to the resolved working directory
    ensure_receive_directory();
    scan_received_folder();

    while (!WindowShouldClose()) {
        // For file tranfer handling
        if (is_connected) {
            process_file_drop(&conn);
            pump_outgoing_transfers(&conn);
        }

        ClearBackground(RAYWHITE);

        BeginDrawing();

        if (!is_connected) {
            introduction_window(comic_font);
            connection_screen(&server_port, server_ip, port_str, username, &is_connected, &conn);
        }

        if (is_connected) {
            panel_scroll_msg(comic_font, &g_mq, &should_scroll_to_bottom);
            text_input(&conn, username, &g_mq, &should_scroll_to_bottom);

            /*-------------------------- RECEIVE --------------------------*/
            int recv_status;
            do {
                recv_status = recv_msg(&conn, g_message_recv, MSG_BUFFER - 1);
                if (recv_status > 0) {
                    process_incoming_stream(g_message_recv, (size_t)recv_status);
                }
            } while (recv_status > 0);

            if (recv_status < 0) {
                is_connected = false;
                abort_all_transfers();
                disconnect_from_server(&conn);
                show_error("Connection lost");
            }
        }

        // Display welcome mesasge at the center horizontally
        welcome_msg(GetFontDefault());

        if (is_connected) {
            files_displaying(comic_font);
            if (has_active_transfer()) {
                draw_transfer_status(comic_font);
            }
        }

        setting_button();

        debugging_button(&debugging);
        if (debugging) {
            show_fps();
            debug_mq(&g_mq);
        }

        draw_warning_dialog();

        EndDrawing();
    }

    free(g_chunk_buffer); g_chunk_buffer = NULL;
    free(g_payload_buffer); g_payload_buffer = NULL;

    cleanup_network();
    CloseWindow();
    return 0;
}

// Logic implementations

static void process_file_drop(ClientConnection* conn)
{
    if (!IsFileDropped())
        return;

    FilePathList dropped_files = LoadDroppedFiles();
    TraceLog(LOG_INFO, "File path entries count: %d", dropped_files.count);

    for (unsigned int i = 0; i < dropped_files.count; ++i) {
        start_outgoing_transfer(conn, dropped_files.paths[i]);
    }
    // Update status UI immediately? Not strictly needed as loop draws it
    UnloadDroppedFiles(dropped_files);
}

static bool ensure_transfer_buffers()
{
    if (!g_chunk_buffer) g_chunk_buffer = (unsigned char*) malloc(FILE_CHUNK_SIZE);
    if (!g_payload_buffer) g_payload_buffer = (unsigned char*) malloc(FILE_ID_LEN + FILE_CHUNK_SIZE);

    if (!g_payload_buffer || !g_chunk_buffer) {
        free(g_chunk_buffer); g_chunk_buffer = NULL;
        free(g_payload_buffer); g_payload_buffer = NULL;
        return false;
    }
    return true;
}

static void pump_outgoing_transfers(ClientConnection* conn)
{
    char message_buffer[256 + FILE_ID_LEN + 8];

    if (!conn->connected)
        return;

    const size_t MAX_QUEUE_SIZE = 10 * 1024 * 1024;
    if (pq_get_data_size(&conn->queue) > MAX_QUEUE_SIZE) {
        return; 
    }

    if (!has_active_transfer()) return;

    if (!ensure_transfer_buffers()) {
        TraceLog(LOG_ERROR, "Failed to allocate persistent buffers for file transfer");
        return;
    }

    unsigned char* chunk_buffer = g_chunk_buffer;
    unsigned char* payload_buffer = g_payload_buffer;

    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        OutgoingTransfer* t = &outgoing_transfers[i];
        if (!t->active)
            continue;

        if (!t->meta_sent) {
            char meta[1024];
            snprintf(meta, sizeof(meta), "%s|%s|%s|%zu|%zu",
                t->sender, t->file_id, t->filename, t->total_bytes, t->chunk_size);

            send_packet(conn, PACKET_TYPE_FILE_START, meta, (uint32_t)strlen(meta));
            t->meta_sent = true;
        }

        int chunks_sent_this_frame = 0;
        
        while (t->sent_bytes < t->total_bytes && chunks_sent_this_frame < MAX_CHUNKS_PER_FRAME) {
            if (pq_get_data_size(&conn->queue) > MAX_QUEUE_SIZE)
                break;

            size_t remaining = t->total_bytes - t->sent_bytes;
            size_t to_read = (remaining > t->chunk_size) ? t->chunk_size : remaining;

            size_t read_count = fread(chunk_buffer, 1, to_read, t->fp);
            if (read_count > 0) {
                memcpy(payload_buffer, t->file_id, FILE_ID_LEN);
                memcpy(payload_buffer + FILE_ID_LEN, chunk_buffer, read_count);

                if (send_packet(conn, PACKET_TYPE_FILE_CHUNK, payload_buffer, (uint32_t)(FILE_ID_LEN + read_count)) == 0) {
                    t->sent_bytes += read_count;
                    t->next_chunk_index++;
                    chunks_sent_this_frame++;
                } else {
                    TraceLog(LOG_ERROR, "Failed to send file chunk");
                    fseek(t->fp, -((long)read_count), SEEK_CUR);
                    break; 
                }
            }

            if (read_count < to_read) {
                if (ferror(t->fp)) {
                    close_outgoing_transfer(conn, t, "File read error");
                }
                break;
            }
        }

        if (t->sent_bytes >= t->total_bytes) {
            snprintf(message_buffer, sizeof(message_buffer), "%s|%s", t->sender, t->file_id);
            send_packet(conn, PACKET_TYPE_FILE_END, message_buffer, (uint32_t)strlen(message_buffer));

            char buf[512];
            snprintf(buf, sizeof(buf), "SYSTEM: Finished sending %s", t->filename);
            add_message(&g_mq, "SYSTEM", buf);

            close_outgoing_transfer(conn, t, NULL);
            scan_received_folder();
        }
    }
}

static void parse_and_add_chat_message(const char* incoming)
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
        if (sender_len >= sizeof(g_mq.messages[0].sender))
            sender_len = sizeof(g_mq.messages[0].sender) - 1;

        char sender[sizeof(g_mq.messages[0].sender)];
        memcpy(sender, buffer, sender_len);
        sender[sender_len] = '\0';

        char* text_start = colon + 1;
        while (*text_start == ' ' || *text_start == '\t')
            text_start++;

        add_message(&g_mq, sender, text_start);
    } else {
        add_message(&g_mq, "SYSTEM", buffer);
    }

    should_scroll_to_bottom = true;
    free(buffer);
}

static void handle_text_payload(char* message)
{
    parse_and_add_chat_message(message);
}

static void handle_packet(uint8_t type, const char* data, size_t len)
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
        handle_text_payload(buffer);
        free(buffer);
    } else if (type >= PACKET_TYPE_FILE_START && type <= PACKET_TYPE_FILE_ABORT) {
        handle_file_packet(type, data, len);
    } else {
        TraceLog(LOG_WARNING, "Unknown packet type: %d", type);
    }
}

static void handle_file_packet(int type, const char* data, size_t len)
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
            add_message(&g_mq, "SYSTEM", "Too many incoming transfers, discarding file");
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
            add_message(&g_mq, "SYSTEM", "Failed to open file for writing");
            slot->active = false;
            return;
        }

        char buf[512];
        snprintf(buf, sizeof(buf), "SYSTEM: Receiving %.200s from %.120s (%.2f MB)",
            slot->filename,
            slot->sender,
            slot->total_bytes / (1024.0 * 1024.0));
        add_message(&g_mq, "SYSTEM", buf);

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

static void process_incoming_stream(const char* data, size_t len)
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
            // TraceLog(LOG_INFO, "DEBUG: Packet Header Type %d, Length %u", current_header.type, current_header.length);
            processed += sizeof(PacketHeader);

            if (current_header.length > 0) {
                stream_state = STATE_BODY;
                bytes_needed = current_header.length;
            } else {
                handle_packet(current_header.type, NULL, 0);
                stream_state = STATE_HEADER;
                bytes_needed = sizeof(PacketHeader);
            }
        } else if (stream_state == STATE_BODY) {
            handle_packet(current_header.type, incoming_stream + processed, current_header.length);
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

static void start_outgoing_transfer(ClientConnection* conn, const char* file_path)
{
    if (!conn || !file_path)
        return;

    if (!conn->connected) {
        show_error("Connect before sending files");
        return;
    }

    OutgoingTransfer* slot = get_free_outgoing();
    if (!slot) {
        show_error("Too many outgoing transfers in progress");
        return;
    }

    struct stat st = { 0 };
    if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        show_error("Invalid file");
        return;
    }

    if ((unsigned long long)st.st_size > FILE_TRANSFER_MAX_SIZE) {
        show_error("File exceeds 500 MB limit");
        return;
    }

    FILE* fp = fopen(file_path, "rb");
    if (!fp) {
        show_error("Failed to open file");
        return;
    }

    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->fp = fp;
    slot->total_bytes = (size_t)st.st_size; 
    slot->chunk_size = FILE_CHUNK_SIZE;
    slot->sent_bytes = 0;
    slot->next_chunk_index = 0;
    slot->chunks_total = (slot->total_bytes + slot->chunk_size - 1) / slot->chunk_size;
    if (conn->username[0] == '\0') {
        strncpy(slot->sender, "Unknown", sizeof(slot->sender) - 1);
        slot->sender[sizeof(slot->sender) - 1] = '\0';
    } else {
        size_t len = strlen(conn->username);
        if (len >= sizeof(slot->sender)) {
            len = sizeof(slot->sender) - 1;
        }
        memcpy(slot->sender, conn->username, len);
        slot->sender[len] = '\0';
    }

    const char* base_name = strrchr(file_path, '/');
    base_name = base_name ? base_name + 1 : file_path;
    strncpy(slot->filename, base_name, sizeof(slot->filename) - 1);
    sanitize_filename(slot->filename);

    generate_file_id(slot->file_id, sizeof(slot->file_id));

    char buf[512];
    snprintf(buf, sizeof(buf), "Sending %s (%.2f MB)", slot->filename, slot->total_bytes / (1024.0 * 1024.0));
    add_message(&g_mq, "SYSTEM", buf);
}

static void ensure_asset_workdir(void)
{
    if (FileExists("resources/fonts/ComicMono.ttf"))
        return;
    #ifdef _WIN32
        _chdir("..");
    #else
        chdir("..");
    #endif

    if (!FileExists("resources/fonts/ComicMono.ttf")) {
        TraceLog(LOG_ERROR, "Asset directory not found: expected resources/fonts near executable");
    }
}
