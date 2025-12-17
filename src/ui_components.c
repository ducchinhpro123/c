#include "ui_components.h"
#include "client_network.h"
#include "file_transfer_state.h"
#include "message.h"
#include "warning_dialog.h"
#include "window.h" // For WINDOW_WIDTH/HEIGHT
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAYGUI_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "raygui.h"
#pragma GCC diagnostic pop

#define MAX_USERNAME_LENGTH 24
#define USERNAME_BUFFER 64

#define FILENAME_TRUNCATE_THRESHOLD 25

static bool edit_mode = false;
static char text_buffer[MSG_BUFFER] = "";

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
        Vector2 text_pos = MeasureTextEx(custom_font, text, (float)font_size, 1);
        int x = (WINDOW_WIDTH - (int)text_pos.x) / 2;
        DrawTextEx(custom_font, text, (Vector2) { (float)x, (float)y }, (float)font_size, 1, color);
    }
}

void text_input(struct ClientConnection* conn, const char* username, MessageQueue* mq, bool* should_scroll)
{
    float box_width = 500;
    float box_height = 50;
    float x_pos = (WINDOW_WIDTH - box_width) / 2;
    float y_pos = (WINDOW_HEIGHT - box_height) - 10;
    bool was_sent = false;
    bool shift_enter_pressed = false;

    // Handle shift-enter for new lines before GuiTextBox
    if (edit_mode && IsKeyPressed(KEY_ENTER)) {
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
            size_t len = strlen(text_buffer);
            if (len < MSG_BUFFER - 2) {
                text_buffer[len] = '\n';
                text_buffer[len + 1] = '\0';
                shift_enter_pressed = true;
                edit_mode = true;
            }
        } else {
            was_sent = true;
        }
    }

    if (GuiTextBox((Rectangle) { (x_pos - ((box_width / 6) / 2)) - 150, y_pos, box_width, box_height }, text_buffer, MSG_BUFFER, edit_mode)) {
        if (!shift_enter_pressed) {
            edit_mode = !edit_mode;
        } else {
            shift_enter_pressed = false;
        }
    }

    // Button to send text
    if (GuiButton((Rectangle) { (x_pos + box_width - ((box_width / 6) / 2)) - 150, y_pos, box_width / 6, box_height }, "SEND")) {
        was_sent = true;
    }

    if (was_sent && strlen(text_buffer) > 0) {
        size_t raw_len = strnlen(text_buffer, MSG_BUFFER - 1);
        char* tmp = (char*)malloc(raw_len + 1);
        if (!tmp) return;
        memcpy(tmp, text_buffer, raw_len);
        tmp[raw_len] = '\0';
        
        // simple trim
        char *s = tmp, *e = tmp + strlen(tmp);
        while (*s && (*s == ' ' || *s == '\t' || *s == '\r')) s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = '\0';

        if (*s) {
            size_t fmt_len = strlen(username) + strlen(s) + 3; // "u: m" + null
            char* formatted_msg = (char*)malloc(fmt_len);
            if (formatted_msg) {
                snprintf(formatted_msg, fmt_len, "%s: %s", username, s);
                int bytes_sent = send_msg((ClientConnection*)conn, formatted_msg);
                if (bytes_sent == 0) {
                    add_message(mq, "me", s);
                    text_buffer[0] = '\0'; // reset
                    edit_mode = true;
                    if (should_scroll) *should_scroll = true;
                    // TraceLog(LOG_INFO, "Sent: %s", formatted_msg);
                }
                free(formatted_msg);
            }
        } else {
            show_error("error sending message");
        }
        free(tmp);
    }
}

void welcome_msg(Font custom_font)
{
    center_text_horizontally("NetApp Client v1.0!", 50, 20, RED, custom_font);
}

static int calculate_wrapped_lines(Font font, const char* text, float font_size, float spacing, float max_width)
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

static void draw_wrapped_text(Font font, const char* text, Vector2 pos, float font_size, float spacing, float max_width, Color color)
{
    char* text_cpy = strdup(text);
    char* word = strtok(text_cpy, " ");
    float cur_x = pos.x;
    float cur_y = pos.y;

    while (word != NULL) {
        char* next_word = strtok(NULL, " ");
        char word_buffer[256]; 
        
        size_t word_len = strlen(word);
        if (word_len >= sizeof(word_buffer) - 1) {
             word_len = sizeof(word_buffer) - 2;
        }

        if (next_word != NULL) {
            snprintf(word_buffer, sizeof(word_buffer), "%.*s ", (int)word_len, word);
        } else {
            snprintf(word_buffer, sizeof(word_buffer), "%.*s", (int)word_len, word);
        }

        Vector2 word_size = MeasureTextEx(font, word_buffer, font_size, spacing);

        if (cur_x + word_size.x > pos.x + max_width) {
            cur_x = pos.x; 
            cur_y += word_size.y + spacing;
        }

        DrawTextEx(font, word_buffer, (Vector2) { cur_x, cur_y }, font_size, spacing, color);

        cur_x += word_size.x;
        word = next_word;
    }

    free(text_cpy);
}

void panel_scroll_msg(Font custom_font, MessageQueue* mq, bool* should_scroll)
{
    int panel_width = 800;
    int panel_height = 700;

    float font_size = 21.0f;
    float spacing = 1.0f;
    float gap = 6.0f;

    int x_pos = ((WINDOW_WIDTH - panel_width) / 2) - 150;
    int y_pos = (WINDOW_HEIGHT - panel_height) / 2;

    float total_len_height = 10.f; 
    for (int i = 0; i < mq->count; i++) {
        const char* msg = mq->messages[i].text;
        const char* sender = mq->messages[i].sender;
        char sender_label[258];
        snprintf(sender_label, sizeof(sender_label), "%s:", sender);

        Vector2 sender_size = MeasureTextEx(custom_font, sender_label, font_size, spacing);
        float avail_width = (panel_width - 15) - sender_size.x - gap;

        int lines_count = calculate_wrapped_lines(custom_font, msg, font_size, spacing, avail_width);
        float message_height = lines_count * (font_size + spacing);
        total_len_height += message_height + 15;
    }
    total_len_height += 20.f; 

    Rectangle panel_rec = { (float)x_pos, (float)y_pos, (float)panel_width, (float)panel_height };
    Rectangle panel_content_rec = { 0, 0, (float)panel_width - 15, total_len_height };
    Rectangle panel_view = { 0 };

    static Vector2 panel_scroll = { 0, 0 };
    GuiScrollPanel(panel_rec, "CHAT", panel_content_rec, &panel_scroll, &panel_view);

    float cumulative_height = 10; 
    BeginScissorMode((int)panel_view.x, (int)panel_view.y, (int)panel_view.width, (int)panel_view.height);
    {
        for (int i = 0; i < mq->count; i++) {
            const char* sender = mq->messages[i].sender;
            const char* msg = mq->messages[i].text;

            float pos_x_sender_label = panel_view.x + 10 - panel_scroll.x; 

            char sender_label[258];
            snprintf(sender_label, sizeof(sender_label), "%s:", sender);
            Vector2 sender_size = MeasureTextEx(custom_font, sender_label, font_size, spacing);
            float avail_width = panel_view.width - sender_size.x - gap;
            
            float pos_x_msg = pos_x_sender_label + sender_size.x + gap;

            int lines_count = calculate_wrapped_lines(custom_font, msg, font_size, spacing, avail_width);
            float message_height = lines_count * (font_size + spacing);

            float y_pos_msg = panel_view.y + cumulative_height + panel_scroll.y;

            if (!strcmp(sender_label, "SYSTEM:")) {
                DrawTextEx(custom_font, sender_label, (Vector2) { pos_x_sender_label, y_pos_msg }, font_size, spacing, RED);
            } else if (!strcmp(sender_label, "me:")) {
                DrawTextEx(custom_font, sender_label, (Vector2) { pos_x_sender_label, y_pos_msg }, font_size, spacing, MAROON);
            } else {
                DrawTextEx(custom_font, sender_label, (Vector2) { pos_x_sender_label, y_pos_msg }, font_size, spacing, GREEN);
            }

            draw_wrapped_text(custom_font, msg, (Vector2) { pos_x_msg, y_pos_msg }, font_size, spacing, avail_width, BLACK);

            cumulative_height += message_height + 15; 
        }

        if (should_scroll && *should_scroll) {
            panel_scroll.y = -(total_len_height - panel_height);
            *should_scroll = false;
        }
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
    center_text_horizontally("This app lets you chat along with sharing files with other peers in a LAN.", 20, 270, RED, custom_font);
    center_text_horizontally("There is nothing much to say; happy coding, and good luck!!!", 20, 310, RED, custom_font);
    center_text_horizontally("Author: Vo Duc Chinh - ST22B - UDA", 30, 650, RED, custom_font);
}

void connection_screen(int* port, char* server_ip, char* port_str, char* username, bool* is_connected, struct ClientConnection* conn)
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

    if (GuiTextBox((Rectangle) { (float)textbox_x_username, 400, (float)textbox_width, 30 }, username, MAX_USERNAME_LENGTH + 1, username_edit_mode)) {
        TraceLog(LOG_INFO, "username set to: %s", username);
        username_edit_mode = !username_edit_mode;
    }

    DrawText("Server IP:", label_x_host, 455, 20, DARKGRAY);
    if (GuiTextBox((Rectangle) { (float)textbox_x_host, 450, (float)textbox_width, 30 }, server_ip, 30, ip_edit_mode)) {
        TraceLog(LOG_INFO, "Port set to: %s", server_ip);
        ip_edit_mode = !ip_edit_mode;
    }

    DrawText("Port number:", label_x_port, 495, 20, DARKGRAY);
    if (GuiTextBox((Rectangle) { (float)textbox_x_port, 490, 100, 30 }, port_str, 30, port_edit_mode)) {
        port_edit_mode = !port_edit_mode;
    }

    if (GuiButton((Rectangle) { (WINDOW_WIDTH - 100) / 2.0f, 550, 100, 50 }, "Connect now")) {
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
            } else if (connect_to_server((ClientConnection*)conn, server_ip, port_str, username_trimmed) == 0) {
                *is_connected = true;
            } else {
                show_error("Connection failed");
                TraceLog(LOG_ERROR, "Connection failed %s:%s", server_ip, port_str);
            }
        }
    }
}

void debug_mq(MessageQueue* mq)
{
    for (int i = 0; i < mq->count; i++) {
        TraceLog(LOG_INFO, "%s: %s (%d)", mq->messages[i].sender, mq->messages[i].text, i + 1);
    }
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
}

void files_displaying(Font font)
{
    int panel_width = 300;
    int panel_height = 400;
    int y_panel_pos = 700; 
    int x_panel_pos = 320; 

    Rectangle panel_rec = { (float)WINDOW_WIDTH - x_panel_pos, (WINDOW_HEIGHT - y_panel_pos) / 2.f, (float)panel_width, (float)panel_height };
    Rectangle panel_view = { 0 };
    float spacing = 1.f;
    float line_height = 25.f;
    float font_size = 13.f;

    float total_len_height = 10.f; 
    for (int i = 0; i < received_files_count; i++) {
        total_len_height += (font_size + spacing) + 15;
    }
    total_len_height += 20.f; 
    Rectangle panel_content_rec = { 0, 0, (float)panel_width + 30, total_len_height };

    static Vector2 panel_scroll = { 0, 0 };
    GuiScrollPanel(panel_rec, NULL, panel_content_rec, &panel_scroll, &panel_view);

    BeginScissorMode((int)panel_view.x, (int)panel_view.y, (int)panel_view.width, (int)panel_view.height);
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

    int prevBaseColor = GuiGetStyle(BUTTON, BASE_COLOR_NORMAL);
    int prevTextColor = GuiGetStyle(BUTTON, TEXT_COLOR_NORMAL);
    int prevBorderColor = GuiGetStyle(BUTTON, BORDER_COLOR_NORMAL);
    
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, ColorToInt(RED));
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, ColorToInt(WHITE));
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, ColorToInt(MAROON));
    
    if (received_files_count > 0 && GuiButton((Rectangle) { (float)WINDOW_WIDTH - panel_width, ((WINDOW_HEIGHT - panel_height) / 2.f) + 270, 120, 30 }, "#9# Remove all files")) {
        remove_all_files();
        scan_received_folder();
    }
    
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, prevBaseColor);
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, prevTextColor);
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, prevBorderColor);
}

void draw_transfer_status(Font custom_font)
{
    if (custom_font.baseSize == 0) {
        custom_font = GetFontDefault();
    }

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
