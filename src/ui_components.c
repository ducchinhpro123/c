#include "ui_components.h"
#include "client_network.h"
#include "file_transfer_state.h"
#include "message.h"
#include "warning_dialog.h"
#include "window.h" // For WINDOW_WIDTH/HEIGHT
// #include "platform.h"
#include "../thirdparty/tinyfiledialogs.h"
#include <errno.h>
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAYGUI_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "raygui.h"
#pragma GCC diagnostic pop

#define MAX_USERNAME_LENGTH 24
#define USERNAME_BUFFER 64

#define FILENAME_TRUNCATE_THRESHOLD 25

static bool edit_mode = false;
static char text_buffer[PROTOCOL_CHAT_CONTENT_MAX_LEN + 1] = "";

static const Color UI_NAVY = { 15, 23, 42, 255 };
static const Color UI_SLATE = { 71, 85, 105, 255 };
static const Color UI_MUTED = { 100, 116, 139, 255 };
static const Color UI_BORDER = { 226, 232, 240, 255 };
static const Color UI_SURFACE = { 255, 255, 255, 255 };
static const Color UI_ACCENT = { 79, 70, 229, 255 };
static const Color UI_ACCENT_SOFT = { 238, 242, 255, 255 };
static const Color UI_SUCCESS = { 16, 185, 129, 255 };
static const Color UI_DANGER = { 225, 29, 72, 255 };

static void draw_card(Rectangle bounds, float radius)
{
    Rectangle shadow = { bounds.x, bounds.y + 4, bounds.width, bounds.height };
    DrawRectangleRounded(shadow, radius, 12, (Color) { 15, 23, 42, 18 });
    DrawRectangleRounded(bounds, radius, 12, UI_SURFACE);
    DrawRectangleRoundedLinesEx(bounds, radius, 12, 1.0f, UI_BORDER);
}

void init_ui_theme(void)
{
    GuiSetStyle(DEFAULT, TEXT_SIZE, 17);
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(UI_SLATE));
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt(UI_BORDER));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt(UI_SURFACE));
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, ColorToInt(UI_ACCENT));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, ColorToInt(UI_ACCENT_SOFT));
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, ColorToInt(UI_ACCENT));
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, ColorToInt(WHITE));
    GuiSetStyle(BUTTON, TEXT_COLOR_FOCUSED, ColorToInt(WHITE));
    GuiSetStyle(BUTTON, TEXT_COLOR_PRESSED, ColorToInt(WHITE));
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, ColorToInt(UI_ACCENT));
    GuiSetStyle(BUTTON, BASE_COLOR_FOCUSED, ColorToInt((Color) { 67, 56, 202, 255 }));
    GuiSetStyle(BUTTON, BASE_COLOR_PRESSED, ColorToInt((Color) { 55, 48, 163, 255 }));
    GuiSetStyle(TEXTBOX, TEXT_PADDING, 12);
}

void debugging_button(bool* debugging)
{
    if (debugging && IsKeyPressed(KEY_F3))
        *debugging = !*debugging;
}

void text_input(struct ClientConnection* conn, const char* username, MessageQueue* mq, bool* should_scroll)
{
    (void)username;
    Rectangle composer = { 32, 706, 856, 70 };
    Rectangle input_bounds = { 48, 720, 704, 42 };
    Rectangle send_bounds = { 764, 720, 108, 42 };
    bool was_sent = false;
    bool shift_enter_pressed = false;

    draw_card(composer, 0.18f);

    // Handle shift-enter for new lines before GuiTextBox
    if (edit_mode && IsKeyPressed(KEY_ENTER)) {
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
            size_t len = strlen(text_buffer);
            if (len < PROTOCOL_CHAT_CONTENT_MAX_LEN - 1) {
                text_buffer[len] = '\n';
                text_buffer[len + 1] = '\0';
                shift_enter_pressed = true;
                edit_mode = true;
            }
        } else {
            was_sent = true;
        }
    }

    if (GuiTextBox(input_bounds, text_buffer, PROTOCOL_CHAT_CONTENT_MAX_LEN + 1, edit_mode)) {
        if (!shift_enter_pressed) {
            edit_mode = !edit_mode;
        } else {
            shift_enter_pressed = false;
        }
    }

    // Button to send text
    if (GuiButton(send_bounds, "Send")) {
        was_sent = true;
    }

    if (was_sent && strlen(text_buffer) > 0) {
        size_t raw_len = strnlen(text_buffer, PROTOCOL_CHAT_CONTENT_MAX_LEN);
        char* tmp = (char*)malloc(raw_len + 1);
        if (!tmp)
            return;
        memcpy(tmp, text_buffer, raw_len);
        tmp[raw_len] = '\0';

        // simple trim
        char *s = tmp, *e = tmp + strlen(tmp);
        while (*s && (*s == ' ' || *s == '\t' || *s == '\r'))
            s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
            *--e = '\0';

        if (*s) {
            int bytes_sent = send_msg((ClientConnection*)conn, s);
            if (bytes_sent == 0) {
                add_message(mq, "me", s);
                text_buffer[0] = '\0';
                edit_mode = true;
                if (should_scroll)
                    *should_scroll = true;
            } else {
                show_error("Message could not be queued");
            }
        } else {
            show_error("Type a message before sending");
        }
        free(tmp);
    }
}

void welcome_msg(Font custom_font)
{
    DrawRectangle(0, 0, WINDOW_WIDTH, 72, UI_NAVY);
    DrawCircle(38, 36, 17, UI_ACCENT);
    DrawCircle(38, 36, 6, (Color) { 199, 210, 254, 255 });
    DrawTextEx(custom_font, "Relay", (Vector2) { 68, 17 }, 27, 0.2f, WHITE);
    DrawTextEx(custom_font, "LAN workspace", (Vector2) { 68, 45 }, 12, 0.2f,
        (Color) { 148, 163, 184, 255 });

    DrawCircle(WINDOW_WIDTH - 272, 36, 4, UI_SUCCESS);
    DrawTextEx(custom_font, "Connected", (Vector2) { WINDOW_WIDTH - 260, 27 }, 14, 0.2f,
        (Color) { 203, 213, 225, 255 });
}

static int calculate_wrapped_lines(Font font, const char* text, float font_size, float spacing, float max_width)
{
    if (!text || max_width <= 1.0f)
        return 1;
    int total_lines = 0;
    char* text_cpy = strdup(text);
    if (!text_cpy)
        return 1;
    char* line = strtok(text_cpy, "\n");

    while (line != NULL) {
        Vector2 line_size = MeasureTextEx(font, line, font_size, spacing);
        int wrap_lines = (int)(line_size.x / max_width) + 1;
        total_lines += wrap_lines;
        line = strtok(NULL, "\n");
    }

    free(text_cpy);
    return total_lines > 0 ? total_lines : 1;
}

static void draw_wrapped_text(Font font, const char* text, Vector2 pos, float font_size, float spacing, float max_width, Color color)
{
    char* text_cpy = strdup(text);
    if (!text_cpy)
        return;
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
    Rectangle card = { 32, 92, 856, 598 };
    Rectangle panel_rec = { 44, 152, 832, 524 };
    float font_size = 16.0f;
    float spacing = 0.3f;
    float bubble_width = panel_rec.width - 32.0f;

    draw_card(card, 0.025f);
    DrawTextEx(custom_font, "Conversation", (Vector2) { 54, 112 }, 21, 0.1f, UI_NAVY);
    char message_count[48];
    snprintf(message_count, sizeof(message_count), "%d message%s", mq->count,
        mq->count == 1 ? "" : "s");
    Vector2 count_size = MeasureTextEx(custom_font, message_count, 13, 0.1f);
    DrawTextEx(custom_font, message_count,
        (Vector2) { card.x + card.width - count_size.x - 24, 118 }, 13, 0.1f, UI_MUTED);

    float total_len_height = 12.f;
    for (int i = 0; i < mq->count; i++) {
        const char* msg = mq->messages[i].text;
        int lines_count = calculate_wrapped_lines(custom_font, msg, font_size, spacing,
            bubble_width - 28.0f);
        float message_height = lines_count * (font_size + spacing);
        total_len_height += message_height + 48.0f;
    }
    total_len_height += 12.f;

    Rectangle panel_content_rec = { 0, 0, panel_rec.width - 16, total_len_height };
    Rectangle panel_view = { 0 };

    static Vector2 panel_scroll = { 0, 0 };
    GuiScrollPanel(panel_rec, NULL, panel_content_rec, &panel_scroll, &panel_view);

    float cumulative_height = 10.0f;
    BeginScissorMode((int)panel_view.x, (int)panel_view.y, (int)panel_view.width, (int)panel_view.height);
    {
        if (mq->count == 0) {
            DrawCircle((int)(panel_view.x + panel_view.width / 2), (int)(panel_view.y + 175), 34,
                UI_ACCENT_SOFT);
            float icon_x = panel_view.x + panel_view.width / 2;
            DrawRectangleRounded((Rectangle) { icon_x - 15, panel_view.y + 165, 30, 20 },
                0.25f, 6, UI_ACCENT);
            DrawCircle((int)icon_x - 7, (int)panel_view.y + 175, 2, WHITE);
            DrawCircle((int)icon_x, (int)panel_view.y + 175, 2, WHITE);
            DrawCircle((int)icon_x + 7, (int)panel_view.y + 175, 2, WHITE);
            const char* empty_title = "No messages yet";
            Vector2 title_size = MeasureTextEx(custom_font, empty_title, 19, 0.1f);
            DrawTextEx(custom_font, empty_title,
                (Vector2) { panel_view.x + (panel_view.width - title_size.x) / 2, panel_view.y + 224 },
                19, 0.1f, UI_NAVY);
            const char* empty_hint = "Start the conversation below or drop a file into the window.";
            Vector2 hint_size = MeasureTextEx(custom_font, empty_hint, 14, 0.1f);
            DrawTextEx(custom_font, empty_hint,
                (Vector2) { panel_view.x + (panel_view.width - hint_size.x) / 2, panel_view.y + 257 },
                14, 0.1f, UI_MUTED);
        }

        for (int i = 0; i < mq->count; i++) {
            const char* sender = mq->messages[i].sender;
            const char* msg = mq->messages[i].text;
            bool is_system = strcmp(sender, "SYSTEM") == 0;
            bool is_me = strcmp(sender, "me") == 0;
            Color bubble = is_system ? (Color) { 245, 243, 255, 255 }
                                     : (is_me ? (Color) { 238, 242, 255, 255 }
                                              : (Color) { 248, 250, 252, 255 });
            Color sender_color = is_system ? (Color) { 124, 58, 237, 255 }
                                           : (is_me ? UI_ACCENT : UI_SUCCESS);

            int lines_count = calculate_wrapped_lines(custom_font, msg, font_size, spacing,
                bubble_width - 28.0f);
            float message_height = lines_count * (font_size + spacing);
            float bubble_y = panel_view.y + cumulative_height + panel_scroll.y;
            Rectangle bubble_bounds = { panel_view.x + 8, bubble_y, bubble_width,
                message_height + 38.0f };
            DrawRectangleRounded(bubble_bounds, 0.12f, 8, bubble);
            DrawTextEx(custom_font, is_me ? "You" : sender,
                (Vector2) { bubble_bounds.x + 14, bubble_bounds.y + 8 }, 13, 0.2f, sender_color);
            draw_wrapped_text(custom_font, msg,
                (Vector2) { bubble_bounds.x + 14, bubble_bounds.y + 25 }, font_size, spacing,
                bubble_bounds.width - 28.0f, UI_NAVY);

            cumulative_height += message_height + 48.0f;
        }

        if (should_scroll && *should_scroll) {
            float overflow = total_len_height - panel_rec.height;
            panel_scroll.y = overflow > 0 ? -overflow : 0;
            *should_scroll = false;
        }
    }
    EndScissorMode();
}

void show_fps()
{
    DrawRectangleRounded((Rectangle) { 16, 16, 92, 34 }, 0.25f, 8, (Color) { 15, 23, 42, 230 });
    DrawText(TextFormat("%d FPS", GetFPS()), 31, 25, 16, (Color) { 167, 243, 208, 255 });
}

void introduction_window(Font custom_font)
{
    DrawRectangle(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, (Color) { 248, 250, 252, 255 });
    DrawRectangle(0, 0, 650, WINDOW_HEIGHT, UI_NAVY);
    DrawCircleGradient(90, 720, 300, (Color) { 79, 70, 229, 76 }, (Color) { 15, 23, 42, 0 });
    DrawCircleGradient(560, 80, 240, (Color) { 14, 165, 233, 50 }, (Color) { 15, 23, 42, 0 });

    DrawCircle(58, 54, 18, UI_ACCENT);
    DrawCircle(58, 54, 6, (Color) { 199, 210, 254, 255 });
    DrawTextEx(custom_font, "Relay", (Vector2) { 88, 36 }, 29, 0.2f, WHITE);

    DrawTextEx(custom_font, "Your LAN,", (Vector2) { 58, 180 }, 54, -0.5f, WHITE);
    DrawTextEx(custom_font, "in one calm place.", (Vector2) { 58, 238 }, 54, -0.5f,
        (Color) { 165, 180, 252, 255 });
    DrawTextEx(custom_font,
        "Chat and move large files directly across your local network.\nNo cloud account. No upload detour.",
        (Vector2) { 60, 326 }, 19, 0.3f, (Color) { 203, 213, 225, 255 });

    const char* features[] = {
        "Peer-approved file transfers",
        "Bounded, validated network packets",
        "Files stay on your local network"
    };
    for (int i = 0; i < 3; ++i) {
        float y = 438.0f + i * 58.0f;
        DrawCircle(71, (int)y + 8, 11, (Color) { 30, 41, 59, 255 });
        DrawLineEx((Vector2) { 66, y + 8 }, (Vector2) { 70, y + 12 }, 2,
            (Color) { 110, 231, 183, 255 });
        DrawLineEx((Vector2) { 70, y + 12 }, (Vector2) { 77, y + 4 }, 2,
            (Color) { 110, 231, 183, 255 });
        DrawTextEx(custom_font, features[i], (Vector2) { 98, y }, 17, 0.2f,
            (Color) { 226, 232, 240, 255 });
    }

    DrawTextEx(custom_font, "Designed for trusted local networks", (Vector2) { 58, 742 }, 13, 0.2f,
        (Color) { 100, 116, 139, 255 });
}

void connection_screen(int* port, char* server_ip, char* port_str, char* username, bool* is_connected, struct ClientConnection* conn)
{
    static bool ip_edit_mode = false;
    static bool port_edit_mode = false;
    static bool username_edit_mode = false;

    Rectangle card = { 720, 88, 480, 624 };
    draw_card(card, 0.045f);

    Font font = GuiGetFont();
    DrawTextEx(font, "Join a workspace", (Vector2) { 766, 132 }, 29, 0.5f, UI_NAVY);
    DrawTextEx(font, "Enter the address shared by your server host.",
        (Vector2) { 766, 172 }, 16, 0.3f, UI_MUTED);

    DrawTextEx(font, "DISPLAY NAME", (Vector2) { 766, 226 }, 12, 1.1f, UI_MUTED);
    if (GuiTextBox((Rectangle) { 766, 248, 388, 46 }, username,
            PROTOCOL_USERNAME_MAX_LEN + 1, username_edit_mode)) {
        username_edit_mode = !username_edit_mode;
    }

    DrawTextEx(font, "SERVER ADDRESS", (Vector2) { 766, 320 }, 12, 1.1f, UI_MUTED);
    if (GuiTextBox((Rectangle) { 766, 342, 388, 46 }, server_ip, 256, ip_edit_mode)) {
        ip_edit_mode = !ip_edit_mode;
    }

    DrawTextEx(font, "PORT", (Vector2) { 766, 414 }, 12, 1.1f, UI_MUTED);
    if (GuiTextBox((Rectangle) { 766, 436, 388, 46 }, port_str, 6, port_edit_mode)) {
        port_edit_mode = !port_edit_mode;
    }

    if (GuiButton((Rectangle) { 766, 526, 388, 48 }, "Connect to workspace")) {
        char* port_end = NULL;
        errno = 0;
        long parsed_port = strtol(port_str, &port_end, 10);
        if (errno != 0 || port_end == port_str || *port_end != '\0' || parsed_port < 1 || parsed_port > 65535) {
            show_error("Enter a port between 1 and 65535");
        } else {
            char tmp[USERNAME_BUFFER];
            strncpy(tmp, username, USERNAME_BUFFER - 1);
            tmp[USERNAME_BUFFER - 1] = '\0';
            char *username_trimmed = tmp, *e = tmp + strlen(tmp);

            // Santize username
            while (*username_trimmed && (*username_trimmed == ' ' || *username_trimmed == '\t' || *username_trimmed == '\n' || *username_trimmed == '\r'))
                username_trimmed++;
            while (e > username_trimmed && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
                *--e = '\0';

            if (!protocol_username_is_valid(username_trimmed)) {
                show_error("Use 1-24 characters; ':' and '|' are not allowed");
            } else if (server_ip[0] == '\0') {
                show_error("Enter a server address");
            } else if (connect_to_server((ClientConnection*)conn, server_ip, port_str, username_trimmed) == 0) {
                *port = (int)parsed_port;
                *is_connected = true;
            } else {
                show_error("Could not reach that server");
            }
        }
    }

    DrawTextEx(font, "Relay traffic is not encrypted. Use only on a network you trust.",
        (Vector2) { 766, 614 }, 13, 0.1f, UI_MUTED);
    DrawTextEx(font, "Tip: the server defaults to port 8898", (Vector2) { 766, 660 }, 13, 0.1f,
        (Color) { 129, 140, 248, 255 });
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
    Rectangle card = { 912, 92, 336, 598 };
    Rectangle drop_zone = { 928, 146, 304, 82 };
    Rectangle panel_rec = { 928, 276, 304, 340 };
    Rectangle panel_view = { 0 };
    float spacing = 0.2f;
    float line_height = 52.f;
    float font_size = 14.f;

    draw_card(card, 0.06f);
    DrawTextEx(font, "Files", (Vector2) { 932, 112 }, 21, 0.1f, UI_NAVY);

    DrawRectangleRounded(drop_zone, 0.12f, 10, UI_ACCENT_SOFT);
    DrawRectangleRoundedLinesEx(drop_zone, 0.12f, 10, 1.0f, (Color) { 199, 210, 254, 255 });
    DrawCircle(956, 174, 14, (Color) { 224, 231, 255, 255 });
    DrawLineEx((Vector2) { 950, 174 }, (Vector2) { 962, 174 }, 2, UI_ACCENT);
    DrawLineEx((Vector2) { 956, 168 }, (Vector2) { 956, 180 }, 2, UI_ACCENT);
    DrawTextEx(font, "Drop files anywhere", (Vector2) { 980, 159 }, 15, 0.1f, UI_NAVY);
    DrawTextEx(font, "Up to 500 MB per file", (Vector2) { 980, 185 }, 12, 0.1f, UI_MUTED);

    DrawTextEx(font, "RECEIVED", (Vector2) { 928, 249 }, 12, 1.0f, UI_MUTED);
    char file_count[24];
    snprintf(file_count, sizeof(file_count), "%d", received_files_count);
    DrawTextEx(font, file_count, (Vector2) { 1214, 249 }, 12, 0.2f, UI_MUTED);

    float total_len_height = received_files_count > 0
        ? received_files_count * line_height + 8.0f
        : panel_rec.height - 8.0f;
    Rectangle panel_content_rec = { 0, 0, panel_rec.width - 14, total_len_height };

    static Vector2 panel_scroll = { 0, 0 };
    GuiScrollPanel(panel_rec, NULL, panel_content_rec, &panel_scroll, &panel_view);

    BeginScissorMode((int)panel_view.x, (int)panel_view.y, (int)panel_view.width, (int)panel_view.height);
    {
        float y_offset = 8.f;
        if (received_files_count == 0) {
            const char* empty = "Received files appear here";
            Vector2 empty_size = MeasureTextEx(font, empty, 13, spacing);
            DrawTextEx(font, empty,
                (Vector2) { panel_view.x + (panel_view.width - empty_size.x) / 2,
                    panel_view.y + 120 },
                13, spacing, UI_MUTED);
        } else {
            for (int i = 0; i < received_files_count; i++) {
                float y_pos = panel_view.y + y_offset + panel_scroll.y;
                DrawRectangleRounded((Rectangle) { panel_view.x + 2, y_pos, panel_view.width - 8, 44 },
                    0.12f, 8, (Color) { 248, 250, 252, 255 });

                int previous_base = GuiGetStyle(BUTTON, BASE_COLOR_NORMAL);
                int previous_text = GuiGetStyle(BUTTON, TEXT_COLOR_NORMAL);
                int previous_border = GuiGetStyle(BUTTON, BORDER_COLOR_NORMAL);
                GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, ColorToInt((Color) { 248, 250, 252, 255 }));
                GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, ColorToInt(UI_DANGER));
                GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, ColorToInt((Color) { 248, 250, 252, 255 }));
                if (GuiButton((Rectangle) { panel_view.x + panel_view.width - 40, y_pos + 7, 28, 28 }, "x")) {
                    remove_selected_file(received_files[i].filename);
                    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, previous_base);
                    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, previous_text);
                    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, previous_border);
                    break;
                }
                GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, previous_base);
                GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, previous_text);
                GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, previous_border);

                char size_str[32];
                snprintf(size_str, sizeof(size_str), "%s", format_file_size(received_files[i].size));
                char display_name[40];
                snprintf(display_name, sizeof(display_name), strlen(received_files[i].filename) > 28 ? "%.25s..." : "%s", received_files[i].filename);
                DrawTextEx(font, display_name, (Vector2) { panel_view.x + 12, y_pos + 6 },
                    font_size, spacing, UI_NAVY);
                DrawTextEx(font, size_str, (Vector2) { panel_view.x + 12, y_pos + 25 },
                    11, spacing, UI_MUTED);
                y_offset += line_height;
            }
        }
    }
    EndScissorMode();

    int prev_base = GuiGetStyle(BUTTON, BASE_COLOR_NORMAL);
    int prev_text = GuiGetStyle(BUTTON, TEXT_COLOR_NORMAL);
    int prev_border = GuiGetStyle(BUTTON, BORDER_COLOR_NORMAL);
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, ColorToInt((Color) { 255, 241, 242, 255 }));
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, ColorToInt(UI_DANGER));
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, ColorToInt((Color) { 254, 205, 211, 255 }));
    if (received_files_count > 0 && GuiButton((Rectangle) { 928, 638, 304, 36 }, "Clear received files")) {
        remove_all_files();
        scan_received_folder();
    }
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, prev_base);
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, prev_text);
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, prev_border);
}

void draw_transfer_status(Font custom_font)
{
    if (custom_font.baseSize == 0) {
        custom_font = GetFontDefault();
    }

    int x = 928;
    int y = 530;
    DrawRectangleRounded((Rectangle) { 928, 522, 304, 152 }, 0.08f, 8,
        (Color) { 248, 250, 252, 248 });
    DrawRectangleRoundedLinesEx((Rectangle) { 928, 522, 304, 152 }, 0.08f, 8, 1.0f, UI_BORDER);
    DrawTextEx(custom_font, "ACTIVE TRANSFERS", (Vector2) { x + 12, y }, 11, 1.0f, UI_MUTED);
    y += 25;
    int shown = 0;
    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        OutgoingTransfer* t = &outgoing_transfers[i];
        if (!t->active || shown >= 2)
            continue;
        float progress = t->total_bytes == 0 ? 0.0f : (float)t->sent_bytes / (float)t->total_bytes;
        if (progress > 1.0f)
            progress = 1.0f;
        DrawRectangleRounded((Rectangle) { x + 12, y + 22, 280, 5 }, 0.5f, 6, UI_BORDER);
        DrawRectangleRounded((Rectangle) { x + 12, y + 22, 280 * progress, 5 }, 0.5f, 6, UI_ACCENT);
        char label[256];
        snprintf(label, sizeof(label), "Sending  %.24s   %.0f%%", t->filename, progress * 100.0f);
        DrawTextEx(custom_font, label, (Vector2) { x + 12, y }, 12, 0.1f, UI_SLATE);
        y += 43;
        shown++;
    }

    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        IncomingTransfer* t = &incoming_transfers[i];
        if (t->state != TRANSFER_STATE_ACCEPTED || shown >= 2)
            continue;
        float progress = t->total_bytes == 0 ? 0.0f : (float)t->received_bytes / (float)t->total_bytes;
        if (progress > 1.0f)
            progress = 1.0f;
        DrawRectangleRounded((Rectangle) { x + 12, y + 22, 280, 5 }, 0.5f, 6, UI_BORDER);
        DrawRectangleRounded((Rectangle) { x + 12, y + 22, 280 * progress, 5 }, 0.5f, 6, UI_SUCCESS);
        char label[256];
        snprintf(label, sizeof(label), "Receiving  %.22s   %.0f%%", t->filename, progress * 100.0f);
        DrawTextEx(custom_font, label, (Vector2) { x + 12, y }, 12, 0.1f, UI_SLATE);
        y += 43;
        shown++;
    }
}

void draw_pending_transfers(Font custom_font, struct ClientConnection* conn)
{
    int pending_count = get_pending_transfer_count();
    if (pending_count == 0)
        return;

    if (custom_font.baseSize == 0) {
        custom_font = GetFontDefault();
    }

    // Draw a semi-transparent overlay
    DrawRectangle(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, (Color) { 15, 23, 42, 150 });

    // Dialog box dimensions
    int dialog_width = 580;
    int dialog_height = 286;
    int dialog_x = (WINDOW_WIDTH - dialog_width) / 2;
    int dialog_y = (WINDOW_HEIGHT - dialog_height) / 2;

    // Draw dialog background
    draw_card((Rectangle) { dialog_x, dialog_y, dialog_width, dialog_height }, 0.05f);

    // Get the first pending transfer
    IncomingTransfer* transfer = get_pending_transfer(0);
    if (!transfer)
        return;

    // Title
    DrawCircle(dialog_x + 40, dialog_y + 42, 18, UI_ACCENT_SOFT);
    DrawLineEx((Vector2) { dialog_x + 32, dialog_y + 42 },
        (Vector2) { dialog_x + 48, dialog_y + 42 }, 2, UI_ACCENT);
    DrawTextEx(custom_font, "Incoming file",
        (Vector2) { dialog_x + 72, dialog_y + 23 }, 23, 0.1f, UI_NAVY);
    DrawTextEx(custom_font, "Review the details before saving this file.",
        (Vector2) { dialog_x + 72, dialog_y + 52 }, 13, 0.1f, UI_MUTED);

    // File info
    char info_line1[512];
    DrawRectangleRounded((Rectangle) { dialog_x + 24, dialog_y + 86, dialog_width - 48, 92 },
        0.08f, 8, (Color) { 248, 250, 252, 255 });
    snprintf(info_line1, sizeof(info_line1), "%.60s", transfer->filename);
    DrawTextEx(custom_font, info_line1,
        (Vector2) { dialog_x + 42, dialog_y + 101 }, 16, 0.1f, UI_NAVY);

    char info_line2[256];
    snprintf(info_line2, sizeof(info_line2), "From %.40s", transfer->sender);
    DrawTextEx(custom_font, info_line2,
        (Vector2) { dialog_x + 42, dialog_y + 132 }, 13, 0.1f, UI_MUTED);

    char info_line3[128];
    snprintf(info_line3, sizeof(info_line3), "Size: %.2f MB",
        transfer->total_bytes / (1024.0 * 1024.0));
    DrawTextEx(custom_font, info_line3,
        (Vector2) { dialog_x + dialog_width - 145, dialog_y + 132 }, 13, 0.1f, UI_MUTED);

    // Pending count indicator
    if (pending_count > 1) {
        char pending_text[64];
        snprintf(pending_text, sizeof(pending_text), "+%d more pending", pending_count - 1);
        DrawTextEx(custom_font, pending_text,
            (Vector2) { dialog_x + dialog_width - 135, dialog_y + 30 }, 13, 0.1f, UI_MUTED);
    }

    // Buttons
    int btn_width = 164;
    int btn_height = 42;
    int btn_y = dialog_y + dialog_height - 66;
    int btn_spacing = 14;

    // Calculate button positions (centered)
    int total_btn_width = btn_width * 3 + btn_spacing * 2;
    int btn_start_x = dialog_x + (dialog_width - total_btn_width) / 2;

    // Accept (default folder) button - Green
    int prevBaseColor = GuiGetStyle(BUTTON, BASE_COLOR_NORMAL);
    int prevTextColor = GuiGetStyle(BUTTON, TEXT_COLOR_NORMAL);
    int prevBorderColor = GuiGetStyle(BUTTON, BORDER_COLOR_NORMAL);

    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, ColorToInt(UI_SUCCESS));
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, ColorToInt(WHITE));
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, ColorToInt(UI_SUCCESS));

    if (GuiButton((Rectangle) { btn_start_x, btn_y, btn_width, btn_height }, "Save to received")) {
        // Save file_id and sender before accept modifies transfer state
        char file_id_copy[FILE_ID_LEN];
        char sender_copy[256];
        strncpy(file_id_copy, transfer->file_id, sizeof(file_id_copy) - 1);
        file_id_copy[sizeof(file_id_copy) - 1] = '\0';
        strncpy(sender_copy, transfer->sender, sizeof(sender_copy) - 1);
        sender_copy[sizeof(sender_copy) - 1] = '\0';

        accept_incoming_transfer(transfer, "received");
        // Send FILE_ACCEPT to sender so they can start sending chunks
        if (conn && atomic_load(&conn->connected)) {
            char response[512];
            uint8_t response_type = transfer->state == TRANSFER_STATE_ACCEPTED
                ? PACKET_TYPE_FILE_ACCEPT
                : PACKET_TYPE_FILE_ABORT;
            if (response_type == PACKET_TYPE_FILE_ACCEPT)
                snprintf(response, sizeof(response), "%s|%s", sender_copy, file_id_copy);
            else
                snprintf(response, sizeof(response), "%s|%s|Could not create destination", sender_copy, file_id_copy);
            send_packet((ClientConnection*)conn, response_type, response, (uint32_t)strlen(response));
        }
    }

    // Choose folder button - Blue
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, ColorToInt(UI_ACCENT));
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, ColorToInt(WHITE));
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, ColorToInt(UI_ACCENT));

    if (GuiButton((Rectangle) { btn_start_x + btn_width + btn_spacing, btn_y, btn_width, btn_height }, "Choose folder")) {
        const char* folder = tinyfd_selectFolderDialog("Select Save Location", "received");
        if (folder) {
            // Save file_id and sender before accept modifies transfer state
            char file_id_copy[FILE_ID_LEN];
            char sender_copy[256];
            strncpy(file_id_copy, transfer->file_id, sizeof(file_id_copy) - 1);
            file_id_copy[sizeof(file_id_copy) - 1] = '\0';
            strncpy(sender_copy, transfer->sender, sizeof(sender_copy) - 1);
            sender_copy[sizeof(sender_copy) - 1] = '\0';

            accept_incoming_transfer(transfer, folder);

            // Send FILE_ACCEPT to sender so they can start sending chunks
            if (conn && atomic_load(&conn->connected)) {
                char response[512];
                uint8_t response_type = transfer->state == TRANSFER_STATE_ACCEPTED
                    ? PACKET_TYPE_FILE_ACCEPT
                    : PACKET_TYPE_FILE_ABORT;
                if (response_type == PACKET_TYPE_FILE_ACCEPT)
                    snprintf(response, sizeof(response), "%s|%s", sender_copy, file_id_copy);
                else
                    snprintf(response, sizeof(response), "%s|%s|Could not create destination", sender_copy, file_id_copy);
                send_packet((ClientConnection*)conn, response_type, response, (uint32_t)strlen(response));
            }
        }
        // If user cancels folder dialog, do nothing (stay pending)
    }

    // Reject button - Red
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, ColorToInt((Color) { 255, 241, 242, 255 }));
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, ColorToInt(UI_DANGER));
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, ColorToInt((Color) { 254, 205, 211, 255 }));

    if (GuiButton((Rectangle) { btn_start_x + (btn_width + btn_spacing) * 2, btn_y, btn_width, btn_height }, "Reject")) {
        reject_incoming_transfer(transfer);
        // Send FILE_ABORT to sender
        if (conn && atomic_load(&conn->connected)) {
            char abort_msg[512];
            snprintf(abort_msg, sizeof(abort_msg), "%s|%s|Rejected by recipient",
                transfer->sender, transfer->file_id);
            send_packet((ClientConnection*)conn, PACKET_TYPE_FILE_ABORT, abort_msg, (uint32_t)strlen(abort_msg));
        }
        // Clear the transfer slot
        memset(transfer, 0, sizeof(*transfer));
    }

    // Restore button styles
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, prevBaseColor);
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, prevTextColor);
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, prevBorderColor);
}
