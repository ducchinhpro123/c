#include "client_network.h"
#include "file_transfer.h"
#include "message.h"
#include "warning_dialog.h"
#include "window.h"
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define RAYGUI_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "raygui.h"
#pragma GCC diagnostic pop
#include <raylib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define FPS 60
#define USERNAME_BUFFER 64

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

static OutgoingTransfer outgoing_transfers[MAX_ACTIVE_TRANSFERS];
static IncomingTransfer incoming_transfers[MAX_ACTIVE_TRANSFERS];
static char incoming_stream[INCOMING_STREAM_CAPACITY];
static size_t incoming_stream_len = 0;

static void process_file_drop(ClientConnection* conn);
static void start_outgoing_transfer(ClientConnection* conn, const char* file_path);
static void pump_outgoing_transfers(ClientConnection* conn);
static void draw_transfer_status(Font custom_font);
static void process_incoming_stream(const char* data, size_t len);
static void handle_file_packet(char* message);
static void handle_text_payload(char* message);
static void abort_all_transfers(void);
static void parse_and_add_chat_message(const char* incoming);
static OutgoingTransfer* get_free_outgoing(void);
static IncomingTransfer* get_incoming_transfer(const char* file_id);
static IncomingTransfer* get_free_incoming(void);
static void close_outgoing_transfer(ClientConnection* conn, OutgoingTransfer* transfer, const char* error_msg);
static void finalize_incoming_transfer(IncomingTransfer* transfer, bool success, const char* reason);

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

void text_input(ClientConnection* conn, const char* username)
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

    if (GuiTextBox((Rectangle) { x_pos - ((box_width / 6) / 2), y_pos, box_width, box_height }, text_buffer, MSG_BUFFER, edit_mode)) {
        // Only toggle if shift-enter wasn't just processed
        if (!shift_enter_pressed) {
            edit_mode = !edit_mode;
        } else {
            // Reset the flag for next frame
            shift_enter_pressed = false;
        }
    }

    // Button to send text
    if (GuiButton((Rectangle) { x_pos + box_width - ((box_width / 6) / 2), y_pos, box_width / 6, box_height }, "SEND")) {
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
            if (bytes_sent > 0) {
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

void panel_scroll_msg(Font custom_font)
{
    int panel_width = 800;
    int panel_height = 700;

    int font_size = 21;
    float spacing = 1.0f;
    float gap = 6.0f;

    int x_pos = (WINDOW_WIDTH - panel_width) / 2;
    int y_pos = (WINDOW_HEIGHT - panel_height) / 2;

    Rectangle panel_rec = { x_pos, y_pos, panel_width, panel_height }; // The position of pannel
    Rectangle panel_content_rec = { 0, 0, panel_width - 15, 1200 };
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
            strncpy(tmp, username, USERNAME_BUFFER);
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
        mkdir("received", 0755);
    }
}

static void handle_file_packet(char* message)
{
    char* save_ptr = NULL;
    char* token = strtok_r(message, "|", &save_ptr);
    if (!token)
        return;

    const char* type = token;

    if (strcmp(type, "FILE_META") == 0) {
        const char* sender = strtok_r(NULL, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        const char* filename = strtok_r(NULL, "|", &save_ptr);
        const char* total_bytes_str = strtok_r(NULL, "|", &save_ptr);
        const char* chunk_size_str = strtok_r(NULL, "|", &save_ptr);

        TraceLog(LOG_INFO, "FILE_META received: sender=%s, file_id=%s, filename=%s, total=%s, chunk=%s",
            sender ? sender : "NULL",
            file_id ? file_id : "NULL",
            filename ? filename : "NULL",
            total_bytes_str ? total_bytes_str : "NULL",
            chunk_size_str ? chunk_size_str : "NULL");

        if (!sender || !file_id || !filename || !total_bytes_str || !chunk_size_str) {
            TraceLog(LOG_ERROR, "FILE_META validation failed - missing fields");
            return;
        }
        if (!file_id || !filename || !total_bytes_str || !chunk_size_str)
            return;

        unsigned long long total_bytes = strtoull(total_bytes_str, NULL, 10);
        unsigned long chunk_size = strtoul(chunk_size_str, NULL, 10);

        if (total_bytes > FILE_TRANSFER_MAX_SIZE || chunk_size == 0)
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
    } else if (strcmp(type, "FILE_CHUNK") == 0) {
        const char* sender = strtok_r(NULL, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        const char* chunk_idx_str = strtok_r(NULL, "|", &save_ptr);
        const char* payload = strtok_r(NULL, "|", &save_ptr);
        if (!sender || !file_id || !chunk_idx_str || !payload)
            return;

        (void)sender;
        if (!file_id || !chunk_idx_str || !payload)
            return;

        (void)chunk_idx_str;

        IncomingTransfer* slot = get_incoming_transfer(file_id);
        if (!slot || !slot->fp)
            return;

        unsigned char decoded[FILE_CHUNK_SIZE + 4];
        int decoded_len = base64_decode(payload, strlen(payload), decoded, sizeof(decoded));
        if (decoded_len <= 0)
            return;

        size_t written = fwrite(decoded, 1, (size_t)decoded_len, slot->fp);
        if (written != (size_t)decoded_len) {
            finalize_incoming_transfer(slot, false, "disk write error");
            return;
        }

        slot->received_bytes += written;
        if (slot->received_bytes > slot->total_bytes) {
            finalize_incoming_transfer(slot, false, "received more than expected");
        }
    } else if (strcmp(type, "FILE_END") == 0) {
        const char* sender = strtok_r(NULL, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        if (!sender || !file_id)
            return;

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
    } else if (strcmp(type, "FILE_ABORT") == 0) {
        const char* sender = strtok_r(NULL, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        const char* reason = strtok_r(NULL, "|", &save_ptr);

        (void)sender;
        IncomingTransfer* slot = get_incoming_transfer(file_id);
        if (slot)
            finalize_incoming_transfer(slot, false, reason ? reason : "Aborted by sender");
    }
}

static void process_incoming_stream(const char* data, size_t len)
{
    if (len == 0)
        return;

    if (len > INCOMING_STREAM_CAPACITY - incoming_stream_len) {
        TraceLog(LOG_WARNING, "Incoming stream overflow, dropping data");
        incoming_stream_len = 0;
    }

    memcpy(incoming_stream + incoming_stream_len, data, len);
    incoming_stream_len += len;

    size_t processed = 0;
    while (processed < incoming_stream_len) {
        char* newline = memchr(incoming_stream + processed, '\n', incoming_stream_len - processed);
        if (!newline)
            break;

        size_t msg_len = (size_t)(newline - (incoming_stream + processed));
        if (msg_len == 0) {
            processed = (size_t)(newline - incoming_stream) + 1;
            continue;
        }

        if (msg_len >= MSG_BUFFER) {
            TraceLog(LOG_WARNING, "Message too large, skipping");
            processed = (size_t)(newline - incoming_stream) + 1;
            continue;
        }

        char message[MSG_BUFFER];
        memcpy(message, incoming_stream + processed, msg_len);
        message[msg_len] = '\0';

        if (strncmp(message, "FILE_", 5) == 0) {
            handle_file_packet(message);
        } else {
            handle_text_payload(message);
        }

        processed = (size_t)(newline - incoming_stream) + 1;
    }

    if (processed > 0) {
        size_t remaining = incoming_stream_len - processed;
        memmove(incoming_stream, incoming_stream + processed, remaining);
        incoming_stream_len = remaining;
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
    slot->total_bytes = (size_t)st.st_size;
    slot->chunk_size = FILE_CHUNK_SIZE;
    slot->sent_bytes = 0;
    slot->next_chunk_index = 0;
    // How many chunks are needed to complete the transfer process?
    slot->chunks_total = (slot->total_bytes + slot->chunk_size - 1) / slot->chunk_size;
    if (conn->username[0] == '\0') {
        strncpy(slot->sender, "Unknown", sizeof(slot->sender) - 1);
    } else {
        strncpy(slot->sender, conn->username, sizeof(slot->sender) - 1);
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
    UnloadDroppedFiles(dropped_files);
}

// Send chunks every frame
static void pump_outgoing_transfers(ClientConnection* conn)
{
    unsigned char chunk_buffer[FILE_CHUNK_SIZE];
    char encoded_buffer[FILE_CHUNK_ENCODED_SIZE + 4];
    char message_buffer[MSG_BUFFER];
    const double pump_deadline = GetTime() + (TRANSFER_PUMP_BUDGET_MS / 1000.0);
    bool time_budget_exhausted = false;

    for (int i = 0; i < MAX_ACTIVE_TRANSFERS && !time_budget_exhausted; ++i) {
        OutgoingTransfer* transfer = &outgoing_transfers[i];
        if (!transfer->active)
            continue;

        if (!transfer->meta_sent) {
            int written = snprintf(message_buffer, sizeof(message_buffer), "FILE_META|%s|%s|%s|%zu|%zu",
                transfer->sender, transfer->file_id, transfer->filename, transfer->total_bytes, transfer->chunk_size);
            if (written <= 0 || written >= (int)sizeof(message_buffer)) {
                close_outgoing_transfer(conn, transfer, "metadata too large");
                continue;
            }

            TraceLog(LOG_INFO, "Sending FILE_META: %s", message_buffer);

            if (send_msg(conn, message_buffer) < 0) {
                close_outgoing_transfer(conn, transfer, "failed to send metadata");
                continue;
            }
            transfer->meta_sent = true;
            TraceLog(LOG_INFO, "FILE_META sent successfully");
        }

        size_t chunks_sent = 0;
        while (transfer->sent_bytes < transfer->total_bytes && chunks_sent < MAX_CHUNKS_PER_BATCH && !time_budget_exhausted) {
            size_t bytes_to_read = transfer->chunk_size;
            if (bytes_to_read > transfer->total_bytes - transfer->sent_bytes)
                bytes_to_read = transfer->total_bytes - transfer->sent_bytes;

            size_t read = fread(chunk_buffer, 1, bytes_to_read, transfer->fp);
            if (read == 0) {
                if (ferror(transfer->fp)) {
                    close_outgoing_transfer(conn, transfer, "read error");
                }
                break;
            }

            int encoded_len = base64_encode(chunk_buffer, read, encoded_buffer, sizeof(encoded_buffer));
            if (encoded_len <= 0) {
                close_outgoing_transfer(conn, transfer, "encoding error");
                break;
            }

            int written = snprintf(message_buffer, sizeof(message_buffer), "FILE_CHUNK|%s|%s|%zu|%s",
                transfer->sender, transfer->file_id, transfer->next_chunk_index, encoded_buffer);
            if (written <= 0 || written >= (int)sizeof(message_buffer)) {
                close_outgoing_transfer(conn, transfer, "chunk too large");
                break;
            }

            if (send_msg(conn, message_buffer) < 0) {
                close_outgoing_transfer(conn, transfer, "network error");
                break;
            }

            transfer->sent_bytes += read;
            transfer->next_chunk_index++;
            chunks_sent++;

            if (GetTime() >= pump_deadline) {
                time_budget_exhausted = true;
            }
        }

        if (!transfer->active)
            continue;

        if (transfer->sent_bytes >= transfer->total_bytes) {
            if (transfer->fp) {
                fclose(transfer->fp);
                transfer->fp = NULL;
            }

            int written = snprintf(message_buffer, sizeof(message_buffer), "FILE_END|%s|%s",
                transfer->sender, transfer->file_id);
            if (written > 0 && written < (int)sizeof(message_buffer)) {
                if (send_msg(conn, message_buffer) < 0) {
                    close_outgoing_transfer(conn, transfer, "failed to finalize");
                    continue;
                }
            }

            char buf[512];
            snprintf(buf, sizeof(buf), "Finished sending %s", transfer->filename);
            add_message(&g_mq, "SYSTEM", buf);
            memset(transfer, 0, sizeof(*transfer));
        }
    }
}

static void draw_transfer_status(Font custom_font)
{
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

int main()
{
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
            draw_transfer_status(comic_font);
        }

        setting_button();

        debugging_button(&debugging);
        if (debugging) {
            show_fps();
        }

        draw_warning_dialog();

        EndDrawing();
    }

    CloseWindow();
    return 0;
}
