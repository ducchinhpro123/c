// platform.h MUST be first to define NOGDI/NOUSER before any Windows headers
#include "platform.h"

#include <raylib.h>

#include "client_network.h"
#include "file_transfer.h"
#include "file_transfer_state.h"
#include "ui_components.h"
#include <inttypes.h>
#include "message.h"
#include "packet_queue.h"
#include "warning_dialog.h"
#include "window.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "client_logic.h"

// Raygui implementation is now in ui_components.c or shared, but main might need it if using directly
// We moved implementation to ui_components.c to avoid dupes

#define FPS 60
#define USERNAME_BUFFER 64
#define FILENAME_TRUNCATE_THRESHOLD 25
#define MAX_USERNAME_LENGTH 24

Message messages[MAX_MESSAGES];

// For buffering
// Buffers
static unsigned char* g_chunk_buffer = NULL;
static unsigned char* g_payload_buffer = NULL;

MessageQueue g_mq = { 0 }; // Init message queue

#define TRANSFER_PUMP_BUDGET_MS 8.0f
// #define INCOMING_STREAM_CAPACITY (MSG_BUFFER * 5)

static bool should_scroll_to_bottom = false;

// Function Prototypes (Locally scoped logic)
static void start_outgoing_transfer(ClientConnection* conn, const char* file_path);
static void pump_outgoing_transfers(ClientConnection* conn);
static void process_file_drop(ClientConnection* conn);
static bool ensure_transfer_buffers();
static void ensure_asset_workdir(void);

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
            if (update_client_state(&conn, &g_mq)) {
                should_scroll_to_bottom = true;
            }

            if (!conn.connected) {
                is_connected = false;
                // update_client_state handles disconnect_from_server and abort_all_transfers internally if recv fails
                // But we check conn.connected to switch UI
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
