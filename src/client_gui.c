// platform.h MUST be first to define NOGDI/NOUSER before any Windows headers
#include "platform.h"

#include <raylib.h>

#include "client_logic.h"
#include "client_network.h"
#include "file_transfer.h"
#include "file_transfer_state.h"
#include "message.h"
#include "packet_queue.h"
#include "ui_components.h"
#include "warning_dialog.h"
#include "window.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Raygui implementation is now in ui_components.c or shared, but main might need it if using directly
// We moved implementation to ui_components.c to avoid dupes

#define FPS 60
#define UI_FONT_BASE_SIZE 64
#define USERNAME_BUFFER 64
#define FILENAME_TRUNCATE_THRESHOLD 25
#define MAX_USERNAME_LENGTH 24

MessageQueue g_mq = { 0 }; // Init message queue

#define TRANSFER_PUMP_BUDGET_MS 8.0f
// #define INCOMING_STREAM_CAPACITY (MSG_BUFFER * 5)

static bool should_scroll_to_bottom = false;

// Function Prototypes (Locally scoped logic)
static void start_outgoing_transfer(ClientConnection* conn, const char* file_path);
static void pump_outgoing_transfers(ClientConnection* conn);
static void process_file_drop(ClientConnection* conn);
static void ensure_asset_workdir(void);

// Main function
int main()
{
    if (init_network() != 0) {
        fprintf(stderr, "Failed to initialize network\n");
        return 1;
    }

    ensure_asset_workdir();

    const char* window_title = "Relay - private LAN chat and file transfer";
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, window_title);
    SetTargetFPS(FPS);
    // Rasterize above the largest text size used by the UI. A smaller atlas gets
    // visibly pixelated when the 54 px welcome heading scales it up.
    Font comic_font = LoadFontEx(
        "resources/fonts/Inter-VariableFont_opsz,wght.ttf", UI_FONT_BASE_SIZE, NULL, 0);
    SetTextureFilter(comic_font.texture, TEXTURE_FILTER_BILINEAR);
    init_ui_theme();
    GuiSetFont(comic_font);

    char server_ip[256] = "127.0.0.1";
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
        BeginDrawing();
        ClearBackground((Color) { 245, 247, 251, 255 });

        if (!is_connected) {
            introduction_window(comic_font);
            connection_screen(&server_port, server_ip, port_str, username, &is_connected, &conn);
        }

        if (is_connected) {
            welcome_msg(comic_font);
            panel_scroll_msg(comic_font, &g_mq, &should_scroll_to_bottom);
            text_input(&conn, username, &g_mq, &should_scroll_to_bottom);

            if (update_client_state(&conn, &g_mq)) {
                should_scroll_to_bottom = true;
            }

            if (!atomic_load(&conn.connected)) {
                is_connected = false;
                abort_all_transfers();
                disconnect_from_server(&conn);
                show_error("Connection lost");
            }

            files_displaying(comic_font);

            process_file_drop(&conn);
            pump_outgoing_transfers(&conn);

            if (has_active_transfer()) {
                draw_transfer_status(comic_font);
            }

            // Show pending transfer dialog if any
            if (has_pending_transfer()) {
                draw_pending_transfers(comic_font, &conn);
            }
        }

        debugging_button(&debugging);
        if (debugging) {
            show_fps();
        }

        draw_warning_dialog();

        EndDrawing();
    }

    UnloadFont(comic_font);

    abort_all_transfers();
    disconnect_from_server(&conn);
    pq_destroy(&conn.queue);
    destroy_message_queue(&g_mq);
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

static void pump_outgoing_transfers(ClientConnection* conn)
{
    char message_buffer[256 + FILE_ID_LEN + 8];

    if (!atomic_load(&conn->connected))
        return;

    const size_t MAX_QUEUE_SIZE = 32 * 1024 * 1024; // Increased to 32MB for larger chunks
    size_t current_queue_size = pq_get_data_size(&conn->queue);
    if (current_queue_size > MAX_QUEUE_SIZE) {
        return;
    }

    if (!has_active_transfer())
        return;

    // Time-based throttling: spend up to TRANSFER_PUMP_BUDGET_MS per frame
    double start_time = GetTime();
    double budget_seconds = TRANSFER_PUMP_BUDGET_MS / 1000.0;

    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        OutgoingTransfer* t = &outgoing_transfers[i];
        if (!t->active)
            continue;

        // If hasn't sent metadata
        if (!t->meta_sent) {
            char meta[1024];
            snprintf(meta, sizeof(meta), "%s|%s|%s|%zu|%zu",
                t->sender, t->file_id, t->filename, t->total_bytes, t->chunk_size);

            send_packet(conn, PACKET_TYPE_FILE_START, meta, (uint32_t)strlen(meta));
            t->meta_sent = true;
            // Now wait for FILE_ACCEPT from receiver
            continue;
        }

        // Wait for receiver to accept before sending chunks
        if (!t->accepted) {
            // Still waiting for acceptance - don't send chunks yet
            continue;
        }

        // Use time-based budget instead of fixed chunk count
        while (t->sent_bytes < t->total_bytes) {
            // Check time budget
            if ((GetTime() - start_time) > budget_seconds)
                break;

            // Check queue size (less frequently with cached value)
            current_queue_size = pq_get_data_size(&conn->queue);
            if (current_queue_size > MAX_QUEUE_SIZE)
                break;

            size_t remaining = t->total_bytes - t->sent_bytes;
            size_t to_read = (remaining > t->chunk_size) ? t->chunk_size : remaining;

            // Allocate payload buffer for zero-copy transfer
            uint32_t payload_len = (uint32_t)(FILE_ID_LEN + to_read);
            unsigned char* payload = (unsigned char*)malloc(payload_len);
            if (!payload) {
                TraceLog(LOG_ERROR, "Failed to allocate payload buffer");
                break;
            }

            // Copy file_id header
            memcpy(payload, t->file_id, FILE_ID_LEN);

            // Read directly into payload buffer (avoiding double copy)
            size_t read_count = fread(payload + FILE_ID_LEN, 1, to_read, t->fp);
            if (read_count > 0) {
                // Zero-copy push: queue takes ownership of payload
                if (pq_push_zero_copy(&conn->queue, PACKET_TYPE_FILE_CHUNK, payload, (uint32_t)(FILE_ID_LEN + read_count)) == 0) {
                    t->sent_bytes += read_count;
                    t->next_chunk_index++;
                } else {
                    TraceLog(LOG_ERROR, "Failed to send file chunk");
                    free(payload);
                    fseek(t->fp, -((long)read_count), SEEK_CUR);
                    break;
                }
            } else {
                free(payload);
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
            snprintf(buf, sizeof(buf), "Finished sending %s", t->filename);
            add_message(&g_mq, "SYSTEM", buf);

            close_outgoing_transfer(conn, t, NULL);
            scan_received_folder();
        }
    }
}

static void start_outgoing_transfer(ClientConnection* conn, const char* file_path)
{
    if (!conn || !file_path)
        return;

    if (!atomic_load(&conn->connected)) {
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

    // Setup slot based on file metadata
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

    const char* forward_slash = strrchr(file_path, '/');
    const char* back_slash = strrchr(file_path, '\\');
    const char* base_name = forward_slash;
    if (!base_name || (back_slash && back_slash > base_name))
        base_name = back_slash;
    base_name = base_name ? base_name + 1 : file_path;
    strncpy(slot->filename, base_name, sizeof(slot->filename) - 1);
    sanitize_filename(slot->filename);

    if (!generate_file_id(slot->file_id, sizeof(slot->file_id))) {
        close_outgoing_transfer(NULL, slot, NULL);
        show_error("Could not create a secure transfer ID");
        return;
    }

    char buf[512];
    snprintf(buf, sizeof(buf), "Sending %s (%.2f MB)", slot->filename, slot->total_bytes / (1024.0 * 1024.0));
    add_message(&g_mq, "SYSTEM", buf);
}

static void ensure_asset_workdir(void)
{
    if (FileExists("resources/fonts/Inter-VariableFont_opsz,wght.ttf"))
        return;
#ifdef _WIN32
    _chdir("..");
#else
    chdir("..");
#endif

    if (!FileExists("resources/fonts/Inter-VariableFont_opsz,wght.ttf")) {
        TraceLog(LOG_ERROR, "Asset directory not found: expected resources/fonts near executable");
    }
}
