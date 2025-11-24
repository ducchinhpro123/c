#include "client_network.h"
#include "file_transfer.h"
#include "message.h"
#include "packet_queue.h"
#include "platform.h"
#include "warning_dialog.h"
#include "window.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <raylib.h>
#define RAYGUI_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "raygui.h"
#pragma GCC diagnostic pop

#define FPS 60
#define USERNAME_BUFFER 64
#define FILENAME_TRUNCATE_THRESHOLD 25

Message messages[MAX_MESSAGES];

static bool edit_mode = false;
static char text_buffer[MSG_BUFFER] = "";
static MessageQueue g_mq = { 0 }; // Init message queue

#define MAX_ACTIVE_TRANSFERS 8
#define MAX_CHUNKS_PER_BATCH 64
#define TRANSFER_PUMP_BUDGET_MS 8.0f
#define INCOMING_STREAM_CAPACITY (MSG_BUFFER * 2)

typedef struct {
    bool active;
    bool meta_sent;
    char file_id[FILE_ID_LEN];
    char filename[FILE_NAME_MAX_LEN];
    char sender[256];
    FILE* fp;
    size_t total_bytes;
    size_t sent_bytes;
    size_t chunk_size;
    size_t next_chunk_index;
    size_t chunks_total;
} OutgoingTransfer;

typedef struct {
    bool active;
    char file_id[FILE_ID_LEN];
    char filename[FILE_NAME_MAX_LEN];
    char sender[256];
    char save_path[FILE_PATH_MAX_LEN];
    FILE* fp;
    size_t total_bytes;
    size_t received_bytes;
} IncomingTransfer;

typedef struct {
    char filename[256];
    long size;
} FileEntry;

static FileEntry received_files[100]; // max 100 files
static int received_files_count = 0;

static OutgoingTransfer outgoing_transfers[MAX_ACTIVE_TRANSFERS];
static IncomingTransfer incoming_transfers[MAX_ACTIVE_TRANSFERS];
static char incoming_stream[INCOMING_STREAM_CAPACITY];
static size_t incoming_stream_len = 0;

static void text_input(ClientConnection* conn, const char* username);
static void process_file_drop(ClientConnection* conn);
static void start_outgoing_transfer(ClientConnection* conn, const char* file_path);
static void pump_outgoing_transfers(ClientConnection* conn);
static void draw_transfer_status(Font custom_font);
static void process_incoming_stream(const char* data, size_t len);
static void handle_file_packet(int type, const char* data, size_t len);
static void handle_text_payload(char* message);
static void abort_all_transfers(void);
static void parse_and_add_chat_message(const char* incoming);
static OutgoingTransfer* get_free_outgoing(void);
static IncomingTransfer* get_incoming_transfer(const char* file_id);
static IncomingTransfer* get_free_incoming(void);
static void close_outgoing_transfer(ClientConnection* conn, OutgoingTransfer* transfer, const char* error_msg);
static void finalize_incoming_transfer(IncomingTransfer* transfer, bool success, const char* reason);
static bool has_active_transfer(void);
static void scan_received_folder(void);
// Remove all the files in the received/ folder, this folder contains the fowarded files from other clients
static void remove_all_files(void);
// Remove a selected file in the received/ folder
static void remove_selected_file(const char* filepath);

static void remove_selected_file(const char* filepath)
{
    DIR* dir = opendir("received");
    if (dir == NULL) {
        TraceLog(LOG_ERROR, "Failed to open 'received' folder to delete files: %s", strerror(errno));
        return;
    }
    char full_filepath[512];
    snprintf(full_filepath, sizeof(full_filepath), "received/%s", filepath);
    if (remove(full_filepath) == 0) {
        TraceLog(LOG_INFO, "Deleted file: %s", full_filepath);
        scan_received_folder();
        received_files_count -= 1;
    } else {
        TraceLog(LOG_WARNING, "Failed to delete file %s: %s", full_filepath, strerror(errno));
    }

    closedir(dir);
}

static void remove_all_files(void)
{
    DIR* dir = opendir("received");
    if (dir == NULL) {
        TraceLog(LOG_ERROR, "Failed to open 'received' folder to delete files: %s", strerror(errno));
        return;
    }
    struct dirent* entry;
    char filepath[512];
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }

        snprintf(filepath, sizeof(filepath), "received/%s", entry->d_name);
        if (!remove(filepath)) {
            TraceLog(LOG_INFO, "Deleted file: %s", filepath);
        } else {
            TraceLog(LOG_WARNING, "Failed to delete file %s: %s", filepath, strerror(errno));
        }
    }

    closedir(dir);
    received_files_count = 0;
}

void debugging_button(bool* debugging)
{
    if (GuiButton((Rectangle) { WINDOW_WIDTH - 80, 40, 80, 20 }, "#152#Debugging")) {
        *debugging = !*debugging;
    }
}

void setting_button()
{
    if (GuiButton((Rectangle) { WINDOW_WIDTH - 70, 10, 70, 20 }, "#142#Settings")) {
        // TODO: handle setting button
    }
}

void center_text_horizontally(const char* text, int font_size, int y, Color color, Font custom_font)
{
    if (custom_font.baseSize == 0) {
        int text_width = MeasureText(text, font_size);
        int x = (WINDOW_WIDTH - text_width) / 2;
        DrawText(text, x, y, font_size, color);
    } else {
        Vector2 text_pos = MeasureTextEx(custom_font, text, font_size, 1);
        int x = (WINDOW_WIDTH - text_pos.x) / 2;
        DrawTextEx(custom_font, text, (Vector2) { x, y }, font_size, 1, color);
    }
}

static void text_input(ClientConnection* conn, const char* username)
{
    float box_width = 500;
    float box_height = 50;
    float x_pos = (WINDOW_WIDTH - box_width) / 2;
    float y_pos = (WINDOW_HEIGHT - box_height) - 10;
    bool was_sent = false;
    bool shift_enter_pressed = false;

    // Handle shift-enter for new lines before GuiTextBox to prevent focus loss
    if (edit_mode && IsKeyPressed(KEY_ENTER)) {
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
            size_t len = strlen(text_buffer);
            if (len < MSG_BUFFER - 2) {
                TraceLog(LOG_INFO, "Detected new line");
                text_buffer[len] = '\n';
                text_buffer[len + 1] = '\0';
                shift_enter_pressed = true;
                // Prevent GuiTextBox from toggling edit_mode by consuming the enter key
                edit_mode = true;
            }
        } else {
            was_sent = true;
        }
    }

    if (GuiTextBox((Rectangle) { (x_pos - ((box_width / 6) / 2)) - 150, y_pos, box_width, box_height }, text_buffer, MSG_BUFFER, edit_mode)) {
        // Only toggle if shift-enter wasn't just processed
        if (!shift_enter_pressed) {
            edit_mode = !edit_mode;
        } else {
            // Reset the flag for next frame
            shift_enter_pressed = false;
        }
    }

    // Button to send text
    if (GuiButton((Rectangle) { (x_pos + box_width - ((box_width / 6) / 2)) - 150, y_pos, box_width / 6, box_height }, "SEND")) {
        was_sent = true;
    }

    if (was_sent && strlen(text_buffer) > 0) {
        char tmp[MSG_BUFFER];
        strncpy(tmp, text_buffer, MSG_BUFFER);
        tmp[MSG_BUFFER - 1] = '\0';
        // simple trim message
        char *s = tmp, *e = tmp + strlen(tmp);
        while (*s && (*s == ' ' || *s == '\t' || *s == '\r'))
            s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
            *--e = '\0';

        if (*s) {
            char formatted_msg[MSG_BUFFER + 4];
            snprintf(formatted_msg, sizeof(formatted_msg), "%s: %s", username, s);
            int bytes_sent = send_msg(conn, formatted_msg);
            if (bytes_sent == 0) {
                add_message(&g_mq, "me", s);
                text_buffer[0] = '\0'; // reset
                was_sent = true;
                edit_mode = true;
                TraceLog(LOG_INFO, "Sent: %s", formatted_msg);
            }
        } else {
            was_sent = false;
            show_error("error sending message");
        }
    }
}

void welcome_msg(Font custom_font)
{
    center_text_horizontally("NetApp Client v1.0!", 50, 20, RED, custom_font);
}

int calculate_wrapped_lines(Font font, const char* text, float font_size, float spacing, float max_width)
{
    int total_lines = 0;
    char* text_cpy = strdup(text);
    char* line = strtok(text_cpy, "\n");

    while (line != NULL) {
        Vector2 line_size = MeasureTextEx(font, line, font_size, spacing);
        int wrap_lines = (int)(line_size.x / max_width) + 1;
        total_lines += wrap_lines;
        line = strtok(NULL, "\n");
    }

    free(text_cpy);
    return total_lines;
}

void draw_wrapped_text(Font font, const char* text, Vector2 pos, float font_size, float spacing, float max_width, Color color)
{
    char* text_cpy = strdup(text);
    char* word = strtok(text_cpy, " ");
    float cur_x = pos.x;
    float cur_y = pos.y;

    while (word != NULL) {
        char* next_word = strtok(NULL, " ");
        char word_buffer[8192]; // Increased from 256 to handle larger words

        // Safety check: truncate if word is too long
        size_t word_len = strlen(word);
        if (word_len >= sizeof(word_buffer) - 2) {
            TraceLog(LOG_WARNING, "Word too long (%zu bytes), truncating", word_len);
            word_len = sizeof(word_buffer) - 2;
        }

        if (next_word != NULL) {
            snprintf(word_buffer, sizeof(word_buffer), "%.*s ", (int)word_len, word);
        } else {
            snprintf(word_buffer, sizeof(word_buffer), "%.*s", (int)word_len, word);
        }

        Vector2 word_size = MeasureTextEx(font, word_buffer, font_size, spacing);

        if (cur_x + word_size.x > pos.x + max_width) {
            cur_x = pos.x; // reset it
            cur_y += word_size.y + spacing;
        }

        DrawTextEx(font, word_buffer, (Vector2) { cur_x, cur_y }, font_size, spacing, color);

        cur_x += word_size.x;
        word = next_word;
    }

    free(text_cpy);
}

// Main chat to display messages
void panel_scroll_msg(Font custom_font)
{
    int panel_width = 800;
    int panel_height = 700;

    int font_size = 21;
    float spacing = 1.0f;
    float gap = 6.0f;

    int x_pos = ((WINDOW_WIDTH - panel_width) / 2) - 150;
    int y_pos = (WINDOW_HEIGHT - panel_height) / 2;

    float total_len_height = 10.f; // for margin at the top
    for (int i = 0; i < g_mq.count; i++) {
        const char* msg = g_mq.messages[i].text;
        const char* sender = g_mq.messages[i].sender;
        char sender_label[258];
        snprintf(sender_label, sizeof(sender_label), "%s:", sender);

        Vector2 sender_size = MeasureTextEx(custom_font, sender_label, font_size, spacing);
        float avail_width = (panel_width - 15) - sender_size.x - gap;

        int lines_count = calculate_wrapped_lines(custom_font, msg, font_size, spacing, avail_width);
        float message_height = lines_count * (font_size + spacing);
        total_len_height += message_height + 15;
    }
    total_len_height += 20.f; // extra margin at the bottom

    Rectangle panel_rec = { x_pos, y_pos, panel_width, panel_height }; // The position of pannel
    Rectangle panel_content_rec = { 0, 0, panel_width - 15, total_len_height };
    Rectangle panel_view = { 0 };

    static Vector2 panel_scroll = { 0, 0 };
    GuiScrollPanel(panel_rec, "CHAT", panel_content_rec, &panel_scroll, &panel_view);

    float cumulative_height = 10; // Top padding
    // BEGIN SCISSOR MODE
    BeginScissorMode(panel_view.x, panel_view.y, panel_view.width, panel_view.height);
    for (int i = 0; i < g_mq.count; i++) {
        const char* sender = g_mq.messages[i].sender;
        const char* msg = g_mq.messages[i].text;

        int pos_x_sender_label = panel_view.x + 10 - panel_scroll.x; // Position x for sender label

        char sender_label[258];
        snprintf(sender_label, sizeof(sender_label), "%s:", sender);
        Vector2 sender_size = MeasureTextEx(custom_font, sender_label, font_size, spacing);
        float avail_width = panel_view.width - sender_size.x - gap;
        // Position x for message
        float pos_x_msg = pos_x_sender_label + sender_size.x + gap;

        int lines_count = calculate_wrapped_lines(custom_font, msg, font_size, spacing, avail_width);
        float message_height = lines_count * (font_size + spacing);

        int y_pos_msg = panel_view.y + cumulative_height + panel_scroll.y;

        if (!strcmp(sender_label, "SYSTEM:")) {
            DrawTextEx(custom_font, sender_label, (Vector2) { pos_x_sender_label, y_pos_msg }, font_size, spacing, RED);
        } else if (!strcmp(sender_label, "me:")) {
            DrawTextEx(custom_font, sender_label, (Vector2) { pos_x_sender_label, y_pos_msg }, font_size, spacing, MAROON);
        } else {
            DrawTextEx(custom_font, sender_label, (Vector2) { pos_x_sender_label, y_pos_msg }, font_size, spacing, GREEN);
        }

        draw_wrapped_text(custom_font, msg, (Vector2) { pos_x_msg, y_pos_msg }, font_size, spacing, avail_width, BLACK);

        cumulative_height += message_height + 15; // 15px gap between messages
    }
    EndScissorMode();
}

void show_fps()
{
    DrawText(TextFormat("FPS: %d", GetFPS()), 50, 50, 30, RED);
}

void introduction_window(Font custom_font)
{
    center_text_horizontally("Overview of this application!", 50, 100, RED, custom_font);
    center_text_horizontally("Hi, thank you for using this application. You are a peer in a LAN", 20, 150, RED, custom_font);
    center_text_horizontally("I made this application just for fun, so I hope you don't expect much from it.", 20, 230, RED, custom_font);
    center_text_horizontally("This app lets you chat with other peers in a LAN.", 20, 270, RED, custom_font);
    center_text_horizontally("There is nothing much to say; happy coding, and good luck!!!", 20, 310, RED, custom_font);
}

void connection_screen(int* port, char* server_ip, char* port_str, char* username, bool* is_connected, ClientConnection* conn)
{
    static bool ip_edit_mode = false;
    static bool port_edit_mode = false;
    static bool username_edit_mode = false;

    int label_width_host = MeasureText("Server IP:", 20);
    int label_width_port = MeasureText("Port number:", 20);
    int label_width_username = MeasureText("Your username:", 20);

    int textbox_width = 100;
    int spacing = 10;

    int group_width_host = label_width_host + spacing + textbox_width;
    int group_width_port = label_width_port + spacing + textbox_width;
    int group_width_username = label_width_username + spacing + textbox_width;

    int group_center_x = WINDOW_WIDTH / 2;

    int label_x_host = group_center_x - group_width_host / 2;
    int label_x_port = group_center_x - group_width_port / 2;
    int label_x_username = group_center_x - group_width_username / 2;

    int textbox_x_host = label_x_host + label_width_host + spacing;
    int textbox_x_port = label_x_port + label_width_port + spacing + 10;
    int textbox_x_username = label_x_username + label_width_username + spacing + 10;

    DrawText("Your username:", label_x_username, 405, 20, DARKGRAY);

    if (GuiTextBox((Rectangle) { textbox_x_username, 400, textbox_width, 30 }, username, USERNAME_BUFFER, username_edit_mode)) {
        TraceLog(LOG_INFO, "username set to: %s", username);
        username_edit_mode = !username_edit_mode;
    }

    DrawText("Server IP:", label_x_host, 455, 20, DARKGRAY);
    if (GuiTextBox((Rectangle) { textbox_x_host, 450, textbox_width, 30 }, server_ip, 30, ip_edit_mode)) {
        TraceLog(LOG_INFO, "Port set to: %s", server_ip);
        ip_edit_mode = !ip_edit_mode;
    }

    DrawText("Port number:", label_x_port, 495, 20, DARKGRAY);
    if (GuiTextBox((Rectangle) { textbox_x_port, 490, 100, 30 }, port_str, 30, port_edit_mode)) {
        port_edit_mode = !port_edit_mode;
    }

    if (GuiButton((Rectangle) { (WINDOW_WIDTH - 100) / 2.0, 550, 100, 50 }, "Connect now")) {
        *port = atoi(port_str);

        if (*port < 1 || *port > 65535) {
            TraceLog(LOG_WARNING, "Invalid port number: %d", *port);
            show_error("Invalid port number");
        } else {
            char tmp[USERNAME_BUFFER];
            strncpy(tmp, username, USERNAME_BUFFER - 1);
            tmp[USERNAME_BUFFER - 1] = '\0';
            char *username_trimmed = tmp, *e = tmp + strlen(tmp);

            while (*username_trimmed && (*username_trimmed == ' ' || *username_trimmed == '\t' || *username_trimmed == '\n' || *username_trimmed == '\r'))
                username_trimmed++;
            while (e > username_trimmed && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
                *--e = '\0';

            if (strcmp(username_trimmed, "") == 0) {
                TraceLog(LOG_WARNING, "Invalid username");
                show_error("Invalid username");
            } else if (connect_to_server(conn, server_ip, port_str, username_trimmed) == 0) {
                *is_connected = true;
            } else {
                show_error("Connection failed");
                TraceLog(LOG_ERROR, "Connection failed %s:%s", server_ip, port_str);
            }
        }

        TraceLog(LOG_INFO, "Port set to: %d", *port);
        TraceLog(LOG_INFO, "Server host set to: %s", server_ip);
    }
}

void debug_mq()
{
    for (int i = 0; i < g_mq.count; i++) {
        TraceLog(LOG_INFO, "%s: %s (%d)", g_mq.messages[i].sender, g_mq.messages[i].text, i + 1);
    }
}

static OutgoingTransfer* get_free_outgoing(void)
{
    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        if (!outgoing_transfers[i].active)
            return &outgoing_transfers[i];
    }
    return NULL;
}

static IncomingTransfer* get_free_incoming(void)
{
    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        if (!incoming_transfers[i].active)
            return &incoming_transfers[i];
    }
    return NULL;
}

static IncomingTransfer* get_incoming_transfer(const char* file_id)
{
    if (!file_id)
        return NULL;
    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        if (incoming_transfers[i].active && strcmp(incoming_transfers[i].file_id, file_id) == 0)
            return &incoming_transfers[i];
    }
    return NULL;
}

static void close_outgoing_transfer(ClientConnection* conn, OutgoingTransfer* transfer, const char* error_msg)
{
    if (!transfer)
        return;

    char filename_copy[FILE_NAME_MAX_LEN];
    strncpy(filename_copy, transfer->filename, sizeof(filename_copy) - 1);
    filename_copy[sizeof(filename_copy) - 1] = '\0';

    if (transfer->fp) {
        fclose(transfer->fp);
        transfer->fp = NULL;
    }

    if (error_msg && *error_msg) {
        if (conn && transfer->file_id[0] != '\0') {
            char abort_msg[MSG_BUFFER];
            snprintf(abort_msg, sizeof(abort_msg), "FILE_ABORT|%s|%s|%s", transfer->sender, transfer->file_id, error_msg);
            send_msg(conn, abort_msg);
        }
        char buf[512];
        snprintf(buf, sizeof(buf), "SYSTEM: File transfer error for %s (%s)", filename_copy[0] ? filename_copy : "file", error_msg);
        add_message(&g_mq, "SYSTEM", buf);
    }

    memset(transfer, 0, sizeof(*transfer));
}

static void finalize_incoming_transfer(IncomingTransfer* transfer, bool success, const char* reason)
{
    if (!transfer)
        return;

    char filename_copy[FILE_NAME_MAX_LEN];
    strncpy(filename_copy, transfer->filename, sizeof(filename_copy) - 1);
    filename_copy[sizeof(filename_copy) - 1] = '\0';
    char sender_copy[256];
    strncpy(sender_copy, transfer->sender, sizeof(sender_copy) - 1);
    sender_copy[sizeof(sender_copy) - 1] = '\0';
    char save_path_copy[FILE_PATH_MAX_LEN];
    strncpy(save_path_copy, transfer->save_path, sizeof(save_path_copy) - 1);
    save_path_copy[sizeof(save_path_copy) - 1] = '\0';
    size_t total_bytes_copy = transfer->total_bytes;

    if (transfer->fp) {
        fclose(transfer->fp);
        transfer->fp = NULL;
    }

    if (!success && save_path_copy[0] != '\0') {
        remove(save_path_copy);
    }

    if (success) {
        char buf[512];
        snprintf(buf, sizeof(buf), "SYSTEM: Received file %.200s from %.120s (%zu bytes)",
            filename_copy[0] ? filename_copy : "file",
            sender_copy[0] ? sender_copy : "peer",
            total_bytes_copy);
        add_message(&g_mq, "SYSTEM", buf);
        // Rescan received folder
        scan_received_folder(); 
    } else if (reason) {
        char buf[512];
        snprintf(buf, sizeof(buf), "SYSTEM: Failed to receive %.200s (%s)",
            filename_copy[0] ? filename_copy : "file",
            reason);
        add_message(&g_mq, "SYSTEM", buf);

    }

    memset(transfer, 0, sizeof(*transfer));
}

static void parse_and_add_chat_message(const char* incoming)
{
    if (!incoming || *incoming == '\0')
        return;

    char buffer[MSG_BUFFER];
    strncpy(buffer, incoming, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

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
}

static void handle_text_payload(char* message)
{
    parse_and_add_chat_message(message);
}

static void ensure_receive_directory(void)
{
    struct stat st = { 0 };
    if (stat("received", &st) == -1) {
#ifdef _WIN32
        _mkdir("received"); // Windows: no mode parameter
#else
        mkdir("received", 0755); // Linux: with permissions
#endif
    }
}

// State machine variables
typedef enum {
    STATE_HEADER,
    STATE_BODY
} StreamState;

static StreamState stream_state = STATE_HEADER;
static PacketHeader current_header;
static size_t bytes_needed = sizeof(PacketHeader);

static void handle_packet(uint8_t type, const char* data, size_t len);

static void handle_file_packet(int type, const char* data, size_t len)
{
    if (type == PACKET_TYPE_FILE_START) {
        // Payload: sender|file_id|filename|total_bytes|chunk_size
        char buffer[1024];
        if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
        memcpy(buffer, data, len);
        buffer[len] = '\0';

        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        const char* filename = strtok_r(NULL, "|", &save_ptr);
        const char* total_bytes_str = strtok_r(NULL, "|", &save_ptr);
        const char* chunk_size_str = strtok_r(NULL, "|", &save_ptr);

        TraceLog(LOG_INFO, "FILE_START received: sender=%s, file_id=%s, filename=%s, total=%s",
            sender ? sender : "NULL",
            file_id ? file_id : "NULL",
            filename ? filename : "NULL",
            total_bytes_str ? total_bytes_str : "NULL");

        if (!sender || !file_id || !filename || !total_bytes_str) {
            TraceLog(LOG_ERROR, "FILE_START validation failed - missing fields");
            return;
        }

        unsigned long long total_bytes = strtoull(total_bytes_str, NULL, 10);
        // chunk_size is not strictly needed for receiver but good for info
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

        // Avoid overwriting existing files
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
        // Payload: [FileID][Data]
        if (len <= FILE_ID_LEN) return;
        
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
        // Payload: sender|file_id
        char buffer[512];
        if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
        memcpy(buffer, data, len);
        buffer[len] = '\0';

        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        (void)sender;

        if (!file_id) return;

        IncomingTransfer* slot = get_incoming_transfer(file_id);
        if (!slot) return;

        if (slot->received_bytes == slot->total_bytes) {
            finalize_incoming_transfer(slot, true, NULL);
        } else {
            finalize_incoming_transfer(slot, false, "incomplete file");
        }

    } else if (type == PACKET_TYPE_FILE_ABORT) {
        // Payload: sender|file_id|reason
        char buffer[512];
        if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
        memcpy(buffer, data, len);
        buffer[len] = '\0';

        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        const char* reason = strtok_r(NULL, "|", &save_ptr);
        (void)sender;

        if (!file_id) return;
        IncomingTransfer* slot = get_incoming_transfer(file_id);
        if (slot)
            finalize_incoming_transfer(slot, false, reason ? reason : "Aborted by sender");
    }
}

static void handle_packet(uint8_t type, const char* data, size_t len) {
    if (type == PACKET_TYPE_TEXT) {
        // Ensure null termination for text
        if (len >= MSG_BUFFER) len = MSG_BUFFER - 1;
        char buffer[MSG_BUFFER];
        if (len > 0) memcpy(buffer, data, len);
        buffer[len] = '\0';
        handle_text_payload(buffer);
    } else if (type >= PACKET_TYPE_FILE_START && type <= PACKET_TYPE_FILE_ABORT) {
        handle_file_packet(type, data, len);
    } else {
        TraceLog(LOG_WARNING, "Unknown packet type: %d", type);
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
            processed += sizeof(PacketHeader);
            
            if (current_header.length > 0) {
                stream_state = STATE_BODY;
                bytes_needed = current_header.length;
            } else {
                // Empty packet, handle immediately
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

// Setup and getting information of a file
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

    // Setup outgoing transfer
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->fp = fp;
    slot->total_bytes = (size_t)st.st_size; // how many bytes of the file
    slot->chunk_size = FILE_CHUNK_SIZE;
    slot->sent_bytes = 0;
    slot->next_chunk_index = 0;
    // How many chunks are needed to complete the transfer process?
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

static void process_file_drop(ClientConnection* conn)
{
    if (!IsFileDropped())
        return;

    FilePathList dropped_files = LoadDroppedFiles();
    TraceLog(LOG_INFO, "File path entries count: %d", dropped_files.count);

    for (unsigned int i = 0; i < dropped_files.count; ++i) {
        start_outgoing_transfer(conn, dropped_files.paths[i]);
    }
    draw_transfer_status(GetFontDefault());
    UnloadDroppedFiles(dropped_files);
}

static bool has_active_transfer(void)
{
    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; i++) {
        if (outgoing_transfers[i].active) {
            return true;
        }
        if (incoming_transfers[i].active) {
            return true;
        }
    }
    return false;
}

// Send chunks every frame
// Send chunks every frame
// Send chunks every frame
static void pump_outgoing_transfers(ClientConnection* conn)
{
    // Use static buffers to avoid stack overflow with large chunk sizes (e.g. 1MB)
    // Total stack usage would be > 2MB otherwise, which crashes on Windows (default 1MB stack)
    static unsigned char chunk_buffer[FILE_CHUNK_SIZE];
    static unsigned char payload_buffer[FILE_ID_LEN + FILE_CHUNK_SIZE];
    char message_buffer[MSG_BUFFER];

    if (!conn->connected)
        return;

    // Throttle based on queue size (e.g., max 10MB pending)
    // This prevents memory exhaustion if disk read > network send
    const size_t MAX_QUEUE_SIZE = 10 * 1024 * 1024; 
    if (pq_get_data_size(&conn->queue) > MAX_QUEUE_SIZE) {
        return; // Wait for queue to drain
    }

    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        OutgoingTransfer* t = &outgoing_transfers[i];
        if (!t->active)
            continue;

        // Send metadata if not sent yet
        if (!t->meta_sent) {
            char meta[1024];
            // Format: sender|file_id|filename|total_bytes|chunk_size
            snprintf(meta, sizeof(meta), "%s|%s|%s|%zu|%zu",
                t->sender, t->file_id, t->filename, t->total_bytes, t->chunk_size);
            
            send_packet(conn, PACKET_TYPE_FILE_START, meta, (uint32_t)strlen(meta));
            t->meta_sent = true;
        }

        // Send chunks
        // We can send multiple chunks per frame since we are just pushing to memory now
        // But let's limit it to avoid spiking memory usage too fast in one frame
        int chunks_sent_this_frame = 0;
        const int MAX_CHUNKS_PER_FRAME = 5; 

        while (t->sent_bytes < t->total_bytes && chunks_sent_this_frame < MAX_CHUNKS_PER_FRAME) {
            // Check queue size again inside loop
            if (pq_get_data_size(&conn->queue) > MAX_QUEUE_SIZE)
                break;

            size_t remaining = t->total_bytes - t->sent_bytes;
            size_t to_read = (remaining > t->chunk_size) ? t->chunk_size : remaining;

            size_t read_count = fread(chunk_buffer, 1, to_read, t->fp);
            if (read_count > 0) {
                // Construct payload: [FileID][Data]
                memcpy(payload_buffer, t->file_id, FILE_ID_LEN);
                memcpy(payload_buffer + FILE_ID_LEN, chunk_buffer, read_count);

                send_packet(conn, PACKET_TYPE_FILE_CHUNK, payload_buffer, (uint32_t)(FILE_ID_LEN + read_count));

                t->sent_bytes += read_count;
                t->next_chunk_index++;
                chunks_sent_this_frame++;
            }

            if (read_count < to_read) {
                if (ferror(t->fp)) {
                    close_outgoing_transfer(conn, t, "File read error");
                }
                break;
            }
        }

        if (t->sent_bytes >= t->total_bytes) {
            // Send END packet
            snprintf(message_buffer, sizeof(message_buffer), "%s|%s", t->sender, t->file_id);
            send_packet(conn, PACKET_TYPE_FILE_END, message_buffer, (uint32_t)strlen(message_buffer));

            char buf[512];
            snprintf(buf, sizeof(buf), "SYSTEM: Finished sending %s", t->filename);
            add_message(&g_mq, "SYSTEM", buf);

            close_outgoing_transfer(conn, t, NULL);
            scan_received_folder(); // Moved here to be inside the function and after transfer completion
        }
    }
}

static void draw_transfer_status(Font custom_font)
{
    if (custom_font.baseSize == 0) {
        custom_font = GetFontDefault();
    }

    (void)custom_font;
    int x = 40;
    int y = WINDOW_HEIGHT - 180;
    DrawText("Outgoing transfers", x, y, 18, DARKGRAY);
    y += 20;
    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        OutgoingTransfer* t = &outgoing_transfers[i];
        if (!t->active)
            continue;
        float progress = t->total_bytes == 0 ? 0.0f : (float)t->sent_bytes / (float)t->total_bytes;
        if (progress > 1.0f)
            progress = 1.0f;
        DrawRectangle(x, y, 300, 18, LIGHTGRAY);
        DrawRectangle(x, y, (int)(300 * progress), 18, SKYBLUE);
        char label[256];
        snprintf(label, sizeof(label), "%.40s (%.1f%%)", t->filename, progress * 100.0f);
        DrawText(label, x + 5, y + 2, 14, BLACK);
        y += 26;
    }

    x = WINDOW_WIDTH - 360;
    y = WINDOW_HEIGHT - 180;
    DrawText("Incoming transfers", 40, y + 50, 18, DARKGRAY);
    y += 20;
    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        IncomingTransfer* t = &incoming_transfers[i];
        if (!t->active)
            continue;
        float progress = t->total_bytes == 0 ? 0.0f : (float)t->received_bytes / (float)t->total_bytes;
        if (progress > 1.0f)
            progress = 1.0f;
        DrawRectangle(x, y, 300, 18, LIGHTGRAY);
        DrawRectangle(x, y, (int)(300 * progress), 18, LIME);
        char label[256];
        snprintf(label, sizeof(label), "%.40s (%.1f%%)", t->filename, progress * 100.0f);
        DrawText(label, x + 5, y + 2, 14, BLACK);
        y += 26;
    }
}

static void abort_all_transfers(void)
{
    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        close_outgoing_transfer(NULL, &outgoing_transfers[i], "connection closed");
        finalize_incoming_transfer(&incoming_transfers[i], false, "connection closed");
    }
    incoming_stream_len = 0;
}

static void scan_received_folder(void)
{
    received_files_count = 0;
    DIR* dir = opendir("received");
    if (!dir) {
        TraceLog(LOG_ERROR, "Failed to open 'received' folder: %s", strerror(errno));
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && received_files_count < 100) {
        // Skip "." and ".." directories
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "received/%s", entry->d_name);

        struct stat st;
        if (stat(filepath, &st) == 0 && S_ISREG(st.st_mode)) {
            strncpy(received_files[received_files_count].filename, entry->d_name, sizeof(received_files[received_files_count].filename) - 1);
            received_files[received_files_count].size = st.st_size;
            received_files_count++;
        }
    }
    closedir(dir);
}

static char* format_file_size(long file_size)
{
    static char file_size_str[64];

    if (file_size < 1024) {
        snprintf(file_size_str, sizeof(file_size_str), "%ld B", file_size);
        return file_size_str;
    } else if (file_size < 1024 * 1024) {
        snprintf(file_size_str, sizeof(file_size_str), "%.2f KB", file_size / 1024.0);
        return file_size_str;
    } else {
        snprintf(file_size_str, sizeof(file_size_str), "%.2f MB", file_size / (1024.0 * 1024.0));
        return file_size_str;
    }
    return file_size_str;
}

// Display the files next to the main chat interface -> retrieve the files fromm the ./received folder
static void files_displaying(Font font)
{
    int panel_width = 300;
    int panel_height = 400;
    int y_panel_pos = 700; // Position `y` for panel
    int x_panel_pos = 320; // Position `x` for panel

    Rectangle panel_rec = { WINDOW_WIDTH - x_panel_pos, (WINDOW_HEIGHT - y_panel_pos) / 2.f, panel_width, panel_height };
    // Rectangle panel_content_rec = { 0, 0, 400, 640 };
    Rectangle panel_view = { 0 };
    float spacing = 1.f;
    float line_height = 25.f;
    float font_size = 13.f;

    float total_len_height = 10.f; // for margin at the top
    for (int i = 0; i < received_files_count; i++) {
        int lines_count = 1; // Always 1
        float message_height = lines_count * (font_size + spacing);
        total_len_height += message_height + 15;
    }
    total_len_height += 20.f; // extra margin at the bottom
    Rectangle panel_content_rec = { 0, 0, panel_width + 30, total_len_height };

    static Vector2 panel_scroll = { 0, 0 };
    GuiScrollPanel(panel_rec, NULL, panel_content_rec, &panel_scroll, &panel_view);

    // Rectangle panel_content_rec = { 0, 0, panel_width - 15, total_len_height };
    BeginScissorMode(panel_view.x, panel_view.y, panel_view.width, panel_view.height);
    {
        float y_offset = 5.f;
        if (received_files_count == 0) {
            DrawTextEx(font, "No files received yet",
                (Vector2) { panel_view.x + 10, panel_view.y + y_offset + panel_scroll.y },
                12.f, spacing, DARKGRAY);
        } else {
            for (int i = 0; i < received_files_count; i++) {
                float x_pos = panel_view.x + 10 + panel_scroll.x;
                float y_pos = panel_view.y + y_offset + panel_scroll.y;

                float btn_x = panel_view.x + panel_view.width - 30; 
                float btn_y = panel_view.y + y_offset + panel_scroll.y;
                // float btn_y = panel_view.y + y_offset + panel_scroll.y;
                // Delete a file button
                if (GuiButton((Rectangle) { btn_x, btn_y, 20, 20 }, "#9#")) {
                    remove_selected_file(received_files[i].filename);
                }

                char display_text[128];
                char size_str[32];
                snprintf(size_str, sizeof(size_str), "%s", format_file_size(received_files[i].size));

                if (strlen(received_files[i].filename) > FILENAME_TRUNCATE_THRESHOLD) {
                    snprintf(display_text, sizeof(display_text), "%d. %.22s...(%s)", i + 1, received_files[i].filename, size_str);
                } else {
                    snprintf(display_text, sizeof(display_text), "%d. %.25s (%s)", i + 1, received_files[i].filename, size_str);
                }


                DrawTextEx(font, display_text, (Vector2) { x_pos, y_pos }, font_size, spacing, BLACK);
                y_offset += line_height;
            }
        }
    }
    EndScissorMode();

    // 1. Save previous style values so we don't affect other buttons
    int prevBaseColor = GuiGetStyle(BUTTON, BASE_COLOR_NORMAL);
    int prevTextColor = GuiGetStyle(BUTTON, TEXT_COLOR_NORMAL);
    int prevBorderColor = GuiGetStyle(BUTTON, BORDER_COLOR_NORMAL);
    // 2. Set custom styles for this button (Red Theme)
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, ColorToInt(RED));
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, ColorToInt(WHITE));
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, ColorToInt(MAROON));
    // 3. Draw the button
    if (received_files_count > 0 && GuiButton((Rectangle) { WINDOW_WIDTH - panel_width, ((WINDOW_HEIGHT - panel_height) / 2.f) + 270, 120, 30}, "#9# Remove all files")) {
        remove_all_files();
        scan_received_folder();
    }
    // 4. Restore previous styles
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, prevBaseColor);
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, prevTextColor);
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, prevBorderColor);
}

int main()
{
    if (init_network() != 0) {
        fprintf(stderr, "Failed to initialize network\n");
        return 1;
    }

    const char* window_title = "C&F";
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, window_title);
    SetTargetFPS(FPS);
    Font comic_font = LoadFont("resources/fonts/ComicMono.ttf");
    Font comic_font_bold = LoadFont("resources/fonts/ComicMono-Bold.ttf");

    char server_ip[16] = "127.0.0.1";
    int server_port = 8898;
    char port_str[6] = "8898";
    char username[USERNAME_BUFFER];
    memset(username, 0, sizeof(username));

    char message_recv[MSG_BUFFER];

    bool is_connected = false;
    bool debugging = false;

    // Init client connection
    ClientConnection conn;
    init_client_connection(&conn);

    init_message_queue(&g_mq);

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
            panel_scroll_msg(comic_font);
            text_input(&conn, username);

            /*-------------------------- RECEIVE --------------------------*/
            int recv_status;
            do {
                recv_status = recv_msg(&conn, message_recv, MSG_BUFFER - 1);
                if (recv_status > 0) {
                    process_incoming_stream(message_recv, (size_t)recv_status);
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
        welcome_msg(comic_font_bold);

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
        }

        draw_warning_dialog();

        EndDrawing();
    }

    cleanup_network();
    CloseWindow();
    return 0;
}
