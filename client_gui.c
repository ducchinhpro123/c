#include "client_network.h"
#include "file_transfer.h"
#include "message.h"
#include "warning_dialog.h"
#include "window.h"
#include <stdint.h>
#include <stdio.h>
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
#define MAX_CONCURRENT_TRANSFERS 10

Message messages[MAX_MESSAGES];

static bool edit_mode = false;
static char text_buffer[MSG_BUFFER] = "";
static MessageQueue g_mq = { 0 }; // Init message queue
static FileTransfer* active_transfers[MAX_CONCURRENT_TRANSFERS] = { NULL };
static int transfer_count = 0;

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
//
// void send_msg(char msg[MSG_BUFFER])
// {
//     DrawText(msg, WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2, 30, BLACK);
// }

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

/**
 * @brief Display a Welcome message in the center horizontally of the screen
 * width
 */
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
        char word_buffer[8192];  // Increased from 256 to handle larger words

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
    /* bool showContentArea = true; */
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

        // int y_pos_msg = panel_view.y + 10 + i * 40 + panel_scroll.y;
        int y_pos_msg = panel_view.y + cumulative_height + panel_scroll.y;

        // Append sender label
        if (!strcmp(sender_label, "SYSTEM:")) {
            DrawTextEx(custom_font, sender_label, (Vector2) { pos_x_sender_label, y_pos_msg }, font_size, spacing, RED);
        } else if (!strcmp(sender_label, "me:")) {
            DrawTextEx(custom_font, sender_label, (Vector2) { pos_x_sender_label, y_pos_msg }, font_size, spacing, MAROON);
        } else {
            DrawTextEx(custom_font, sender_label, (Vector2) { pos_x_sender_label, y_pos_msg }, font_size, spacing, GREEN);
        }

        // DrawTextEx(custom_font, msg, (Vector2){pos_x_msg, y_pos_msg}, font_size, spacing, BLACK);
        draw_wrapped_text(custom_font, msg, (Vector2) { pos_x_msg, y_pos_msg }, font_size, spacing, avail_width, BLACK);

        cumulative_height += message_height + 15; // 15px gap between messages

        // EndScissorMode();
        // DrawText(TextFormat("%d", lines_count), 200, 200, 30, RED);
        // BeginScissorMode(panel_view.x, panel_view.y, panel_view.width, panel_view.height);
    }
    EndScissorMode();
    // END SCISSOR MODE
}

void show_fps()
{
    DrawText(TextFormat("FPS: %d", GetFPS()), 50, 50, 30, RED);
}

/**
 * @brief Display an introduction or overview (purpose) of the application
 */
void introduction_window(Font custom_font)
{
    center_text_horizontally("Overview of this application!", 50, 100, RED, custom_font);
    center_text_horizontally("Hi, thank you for using this application. You are a peer in a LAN", 20, 150, RED, custom_font);
    center_text_horizontally("I made this application just for fun, so I hope you don't expect much from it.", 20, 230, RED, custom_font);
    center_text_horizontally("This app lets you chat and share files with other peers in a LAN.", 20, 270, RED, custom_font);
    center_text_horizontally("There is nothing much to say; happy coding, and good luck!!!", 20, 310, RED, custom_font);
}

void connection_screen(int* port, char* server_ip, char* port_str, char* username, bool* is_connected, ClientConnection* conn)
{
    static bool ip_edit_mode = false;
    static bool port_edit_mode = false;
    static bool username_edit_mode = false;

    // Input server ip
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

    // Input username
    DrawText("Your username:", label_x_username, 405, 20, DARKGRAY);

    // int GuiTextBox(Rectangle bounds, char *text, int textSize, bool editMode)
    if (GuiTextBox((Rectangle) { textbox_x_username, 400, textbox_width, 30 }, username, USERNAME_BUFFER, username_edit_mode)) {
        TraceLog(LOG_INFO, "username set to: %s", username);
        username_edit_mode = !username_edit_mode;
    }

    // Input host
    DrawText("Server IP:", label_x_host, 455, 20, DARKGRAY);
    if (GuiTextBox((Rectangle) { textbox_x_host, 450, textbox_width, 30 }, server_ip, 30, ip_edit_mode)) {
        TraceLog(LOG_INFO, "Port set to: %s", server_ip);
        ip_edit_mode = !ip_edit_mode;
    }

    // Input port
    DrawText("Port number:", label_x_port, 495, 20, DARKGRAY);
    if (GuiTextBox((Rectangle) { textbox_x_port, 490, 100, 30 }, port_str, 30, port_edit_mode)) {
        port_edit_mode = !port_edit_mode;
    }

    if (GuiButton((Rectangle) { (WINDOW_WIDTH - 100) / 2.0, 550, 100, 50 }, "Connect now")) {
        // Convert port string as a input to port number
        *port = atoi(port_str);

        if (*port < 1 || *port > 65535) {
            TraceLog(LOG_WARNING, "Invalid port number: %d", *port);
            show_error("Invalid port number");
        } else {
            // Trimmed username
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

void handle_file_drop()
{
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

    /* char *username = malloc(256); */
    bool is_connected = false;
    bool debugging = false;

    // Init client connection
    ClientConnection conn;
    init_client_connection(&conn);

    init_message_queue(&g_mq);

    while (!WindowShouldClose()) {
        /*-------------------------- SENDING FILES --------------------------*/
        if (IsFileDropped()) {
            FilePathList dropped_files = LoadDroppedFiles();
            if (dropped_files.count > 0) {
                const char* file_path = dropped_files.paths[0];
                TraceLog(LOG_INFO, "File dropped: %s", file_path);

                // Get file size without loading entire file
                FILE* file = fopen(file_path, "rb");
                if (!file) {
                    show_error("Failed to open file.");
                    UnloadDroppedFiles(dropped_files);
                    continue;
                }

                fseek(file, 0, SEEK_END);
                size_t file_size = ftell(file);
                fclose(file);

                if (file_size > MAX_FILE_SIZE) {
                    char err_msg[128];
                    snprintf(err_msg, sizeof(err_msg), "File too large: %zu MB (max: %d MB)",
                        file_size / (1024 * 1024), MAX_FILE_SIZE / (1024 * 1024));
                    show_error(err_msg);
                    TraceLog(LOG_ERROR, "%s", err_msg);
                    UnloadDroppedFiles(dropped_files);
                    continue;
                }

                // Extract filename from path
                const char* filename = strrchr(file_path, '/');
                filename = filename ? filename + 1 : file_path;
                TraceLog(LOG_INFO, "Streaming file: %s (%zu bytes)", filename, file_size);

                int total_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
                unsigned char chunk_buffer[CHUNK_SIZE];

                // Stream and send chunks one at a time
                bool send_success = true;
                for (int i = 0; i < total_chunks; i++) {
                    size_t offset = i * CHUNK_SIZE;
                    size_t chunk_size = CHUNK_SIZE;

                    // Read chunk from disk
                    if (!stream_file_chunk(file_path, offset, chunk_buffer, &chunk_size)) {
                        TraceLog(LOG_ERROR, "Failed to read chunk %d from file", i);
                        show_error("Failed to read file chunk.");
                        send_success = false;
                        break;
                    }

                    // Create and send packet
                    char* packet = create_file_packet(filename, file_size,
                        i, total_chunks,
                        chunk_buffer, chunk_size);

                    if (packet) {
                        int bytes_sent = send_msg(&conn, packet);
                        free(packet);

                        if (bytes_sent <= 0) {
                            TraceLog(LOG_ERROR, "Failed to send chunk %d", i);
                            show_error("Failed to send file chunk.");
                            send_success = false;
                            break;
                        }

                        // No artificial delay - let send_all() handle flow control via EAGAIN
                    } else {
                        TraceLog(LOG_ERROR, "Failed to create packet for chunk %d", i);
                        send_success = false;
                        break;
                    }
                }

                if (send_success) {
                    // Add system message
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Sent file: %s (%zu bytes)", filename, file_size);
                    add_message(&g_mq, "SYSTEM", msg);
                    TraceLog(LOG_INFO, "Successfully sent file: %s", filename);
                } else {
                    TraceLog(LOG_ERROR, "File send incomplete: %s", filename);
                }

                UnloadDroppedFiles(dropped_files);
            }
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
            // Large buffer for accumulating FILE packets (256KB to handle multiple large packets)
            // Each file packet can be ~50KB (32KB data + base64 overhead + headers)
            #define FILE_RECV_BUFFER_SIZE (262144)  // 256KB
            static char recv_buffer[FILE_RECV_BUFFER_SIZE] = { 0 };
            static size_t buffer_len = 0;

            ssize_t bytes_recv = recv_msg(&conn, message_recv, MSG_BUFFER);
            if (bytes_recv > 0) {
                // Null-terminate for string operations
                message_recv[bytes_recv] = '\0';

                // Check if it's a FILE packet OR we're accumulating file data (buffer_len > 0)
                // This ensures continuation data from multi-packet files is not lost
                if (strncmp(message_recv, "FILE:", 5) == 0 || buffer_len > 0) {
                    // Handle FILE packet with accumulation buffer
                    // Check if we have enough space (leave 10% margin for safety)
                    if (buffer_len + bytes_recv < FILE_RECV_BUFFER_SIZE * 0.9) {
                        memcpy(recv_buffer + buffer_len, message_recv, bytes_recv);
                        buffer_len += bytes_recv;
                    } else {
                        // Buffer getting full - try to process what we have first
                        TraceLog(LOG_WARNING, "Receive buffer at %zu/%d bytes, processing before adding more",
                                 buffer_len, FILE_RECV_BUFFER_SIZE);
                        
                        // If buffer is dangerously full and we can't process, reset
                        if (buffer_len + bytes_recv >= FILE_RECV_BUFFER_SIZE) {
                            TraceLog(LOG_ERROR, "Receive buffer overflow! Dropping %zu bytes. This may cause transfer failure.",
                                     buffer_len);
                            buffer_len = 0;
                            // Try to add new data to empty buffer
                            if (bytes_recv < FILE_RECV_BUFFER_SIZE) {
                                memcpy(recv_buffer, message_recv, bytes_recv);
                                buffer_len = bytes_recv;
                            }
                        } else {
                            // Still have room, add the data
                            memcpy(recv_buffer + buffer_len, message_recv, bytes_recv);
                            buffer_len += bytes_recv;
                        }
                    }

                    // Process all complete FILE packets in the buffer
                    char* packet_start = recv_buffer;
                    char* buffer_end = recv_buffer + buffer_len;
                    int packets_processed = 0;

                    while (packet_start < buffer_end) {
                        // Find next FILE packet
                        char* file_marker = strstr(packet_start, "FILE:");
                        if (!file_marker)
                            break;

                        // Check if file_marker is within bounds
                        if (file_marker >= buffer_end) {
                            TraceLog(LOG_WARNING, "FILE marker beyond buffer bounds, waiting for more data");
                            break;
                        }

                        // Check for complete packet (must have newline delimiter)
                        char* packet_end = strchr(file_marker, '\n');
                        if (!packet_end || packet_end >= buffer_end) {
                            TraceLog(LOG_INFO, "Incomplete packet at buffer end, waiting for more data");
                            break; // Incomplete packet, will be completed in next recv
                        }

                        packets_processed++;
                        TraceLog(LOG_INFO, "Processing packet %d from received buffer", packets_processed);

                        // Process this complete packet
                        if (!strncmp(file_marker, "FILE:", 5)) {
                            TraceLog(LOG_INFO, "Received a file");
                            FileTransfer* ft = NULL;

                            char temp_filename[MAX_FILENAME];
                            sscanf(file_marker + 5, "%[^:]", temp_filename);
                            for (int i = 0; i < transfer_count; i++) {
                                if (active_transfers[i] && !strcmp(active_transfers[i]->filename, temp_filename)) {
                                    ft = active_transfers[i];
                                    break;
                                }
                            }

                            if (!ft && transfer_count < 10) {
                                ft = malloc(sizeof(FileTransfer));
                                memset(ft, 0, sizeof(FileTransfer));
                                active_transfers[transfer_count++] = ft;
                            }

                            // Parse and add chunk
                            unsigned char* chunk_data;
                            size_t chunk_size;

                            TraceLog(LOG_INFO, "Parsing a file packet.");
                            if (parse_file_packet(file_marker, ft, &chunk_data, &chunk_size)) {
                                TraceLog(LOG_INFO, "parse_file_packet returned successfully, chunk_size=%zu", chunk_size);

                                // Extract chunk index from packet
                                const char* parse_start = file_marker + 5; // Skip "FILE:"
                                const char* pos = parse_start;

                                // Skip filename (first field)
                                pos = strchr(pos, ':');
                                if (!pos) {
                                    TraceLog(LOG_ERROR, "Invalid packet format - no first colon");
                                    free(chunk_data);
                                    continue;
                                }
                                pos++; // Move past the colon

                                // Skip filesize (second field)
                                pos = strchr(pos, ':');
                                if (!pos) {
                                    TraceLog(LOG_ERROR, "Invalid packet format - no second colon");
                                    free(chunk_data);
                                    continue;
                                }
                                pos++; // Move past the colon

                                // Now pos points to the chunk_index string
                                const char* chunk_index_str = pos;
                                pos = strchr(pos, ':');
                                if (!pos) {
                                    TraceLog(LOG_ERROR, "Invalid packet format - no third colon");
                                    free(chunk_data);
                                    continue;
                                }

                                char temp[16];
                                int len = pos - chunk_index_str;
                                if (len >= (int)sizeof(temp))
                                    len = (int)sizeof(temp) - 1;
                                strncpy(temp, chunk_index_str, len);
                                temp[len] = '\0';

                                int chunk_index = atoi(temp);
                                TraceLog(LOG_INFO, "Extracted chunk_index=%d", chunk_index);

                                // Validate file handle
                                if (ft->file_handle == NULL) {
                                    TraceLog(LOG_ERROR, "File handle not opened!");
                                    free(chunk_data);
                                    continue;
                                }

                                // Write chunk directly to disk
                                if (!add_chunk_to_streaming_transfer(ft, chunk_index, chunk_data, chunk_size)) {
                                    TraceLog(LOG_ERROR, "Failed to write chunk %d to disk", chunk_index);
                                    free(chunk_data);

                                    // Cleanup failed transfer
                                    for (int i = 0; i < transfer_count; i++) {
                                        if (active_transfers[i] == ft) {
                                            close_streaming_transfer(ft, false);
                                            for (int j = i; j < transfer_count - 1; j++) {
                                                active_transfers[j] = active_transfers[j + 1];
                                            }
                                            active_transfers[transfer_count - 1] = NULL;
                                            transfer_count--;
                                            break;
                                        }
                                    }
                                    continue;
                                }

                                free(chunk_data);

                                // Check if complete
                                if (ft->complete) {
                                    char msg[512];
                                    snprintf(msg, sizeof(msg), "Received file: %s (%zu bytes)",
                                        ft->filename, ft->total_size);
                                    add_message(&g_mq, "SYSTEM", msg);

                                    // Close and finalize the transfer
                                    for (int i = 0; i < transfer_count; i++) {
                                        if (active_transfers[i] == ft) {
                                            close_streaming_transfer(ft, true);

                                            // Shift remaining transfers left
                                            for (int j = i; j < transfer_count - 1; j++) {
                                                active_transfers[j] = active_transfers[j + 1];
                                            }
                                            active_transfers[transfer_count - 1] = NULL;
                                            transfer_count--;
                                            break;
                                        }
                                    }
                                }
                            }
                        }

                        // Move to next packet (after newline) - we already validated packet_end exists
                        packet_start = packet_end + 1;
                    }

                    TraceLog(LOG_INFO, "Processed %d FILE packets from received buffer", packets_processed);

                    // Move any remaining incomplete data to start of buffer
                    size_t remaining = buffer_end - packet_start;
                    if (remaining > 0 && packet_start != recv_buffer) {
                        memmove(recv_buffer, packet_start, remaining);
                        buffer_len = remaining;
                        TraceLog(LOG_INFO, "Kept %zu bytes of incomplete packet for next iteration", remaining);
                    } else if (remaining == 0) {
                        buffer_len = 0;
                    } else {
                        // packet_start == recv_buffer, no packets were processed
                        buffer_len = remaining;
                    }
                } else {
                    // Regular text message - parse "sender: text" format
                    // BUT: Skip if it looks like leftover FILE packet data (contains base64/binary)
                    bool is_likely_binary = false;
                    size_t msg_len = strlen(message_recv);
                    
                    // Heuristic: if message is very long (>1KB) or has no spaces, it's probably binary data
                    if (msg_len > 1024) {
                        is_likely_binary = true;
                        TraceLog(LOG_WARNING, "Skipping display of likely binary/base64 data (%zu bytes)", msg_len);
                    }
                    
                    if (!is_likely_binary) {
                        char* colon_pos = strchr(message_recv, ':');
                        if (colon_pos && colon_pos > message_recv) {
                            // Extract sender
                            size_t sender_len = colon_pos - message_recv;
                            char sender[256];
                            if (sender_len < sizeof(sender)) {
                                strncpy(sender, message_recv, sender_len);
                                sender[sender_len] = '\0';

                                // Extract message text (skip ": " after sender)
                                char* text_start = colon_pos + 1;
                                while (*text_start == ' ' || *text_start == '\t') {
                                    text_start++;
                                }

                                // Add to message queue
                                add_message(&g_mq, sender, text_start);
                                TraceLog(LOG_INFO, "Received message from %s: %s", sender, text_start);
                            } else {
                                TraceLog(LOG_WARNING, "Sender name too long, ignoring message");
                            }
                        } else {
                            // No colon found, might be a system message (but check length first)
                            if (msg_len < 512) {  // Only show short system messages
                                add_message(&g_mq, "SYSTEM", message_recv);
                            } else {
                                TraceLog(LOG_WARNING, "Skipping long non-text data (%zu bytes)", msg_len);
                            }
                        }
                    }
                }

            } else if (bytes_recv < 0) {
                is_connected = false;
                show_error("Connection lost");
            }
        }
        /* init_server_button(&is_connected); */

        /* if (is_connected) { // Only display chat interface once user is connected */
        /*     panel_scroll_msg(inter_font); */
        /* } */

        // Display welcome mesasge at the center horizontally
        welcome_msg(comic_font_bold);

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
