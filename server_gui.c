#include "message.h"
#include "server.h"
#include "window.h"
#include <stdio.h>
#define RAYGUI_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "raygui.h"
#pragma GCC diagnostic pop
#include <raylib.h>
/* #include "server_gui.h" */

#define FPS 60

Message messages[MAX_MESSAGES];
int message_count = 0;
static bool edit_mode = false;
static char text_buffer[MSG_BUFFER] = "";

// Init message queue
static MessageQueue g_mq = { 0 };

bool debugging = false;

void debugging_button()
{
    if (GuiButton((Rectangle) { WINDOW_WIDTH - 80, 40, 80, 20 }, "#152#Debugging")) {
        debugging = !debugging;
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

void text_input(bool is_connected)
{
    if (!is_connected) {
        return;
    }

    float box_width = 500;
    float box_height = 50;
    float x_pos = (WINDOW_WIDTH - box_width) / 2;
    float y_pos = (WINDOW_HEIGHT - box_height) - 10;
    bool was_sent = false;

    /* GuiWindowBox(Rectangle bounds, const char *title); */
    // Input text
    if (GuiTextBox((Rectangle) { x_pos - ((box_width / 6) / 2), y_pos, box_width, box_height }, text_buffer, MSG_BUFFER, edit_mode)) {
        edit_mode = !edit_mode;
    }

    // Button to send text
    if (GuiButton((Rectangle) { x_pos + box_width - ((box_width / 6) / 2), y_pos, box_width / 6, box_height }, "SEND")) {
        was_sent = true;
    }

    if (IsKeyPressed(KEY_ENTER) && !IsKeyDown(KEY_LEFT_SHIFT) && !IsKeyDown(KEY_RIGHT_SHIFT)) {
        was_sent = true;
    }

    if (was_sent) {
        char tmp[MSG_BUFFER];
        strncpy(tmp, text_buffer, MSG_BUFFER);
        tmp[MSG_BUFFER - 1] = '\0';
        // simple trim message
        char *s = tmp, *e = tmp + strlen(tmp);
        while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
            s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
            *--e = '\0';

        if (*s) {
            char formatted_msg[MSG_BUFFER + 9];
            // We actually send the prefix SERVER.
            snprintf(formatted_msg, sizeof(formatted_msg), "SERVER: %s", s);
            server_broadcast_msg(formatted_msg, -1);
            add_message(&g_mq, "SERVER", s);
            memset(text_buffer, 0, sizeof(text_buffer));
        }
        was_sent = false;
    }
}

/**
 * @brief Display a Welcome message in the center horizontally of the screen
 * width
 */
void welcome_msg(Font custom_font)
{
    center_text_horizontally("NetApp Server v1.0!", 50, 20, RED, custom_font);
}

void panel_scroll_msg(Font custom_font)
{
    /* bool showContentArea = true; */
    int panel_width = 800;
    int panel_height = 700;

    int font_size = 16;
    int gap = 10;

    int x_pos = (WINDOW_WIDTH - panel_width) / 2;
    int y_pos = (WINDOW_HEIGHT - panel_height) / 2;

    Rectangle panel_rec = { x_pos, y_pos, panel_width, panel_height }; // The position of pannel
    Rectangle panel_content_rec = { 0, 0, panel_width - 15, 1200 };
    Rectangle panel_view = { 0 };

    static Vector2 panel_scroll = { 0, 0 };
    GuiScrollPanel(panel_rec, "CHAT", panel_content_rec, &panel_scroll, &panel_view);

    // BEGIN SCISSOR MODE
    BeginScissorMode(panel_view.x, panel_view.y, panel_view.width, panel_view.height);
    for (int i = 0; i < g_mq.count; i++) {
        int x_pos_msg = panel_view.x + gap - panel_scroll.x;
        int y_pos_msg = panel_view.y + gap + i * 40 + panel_scroll.y;

        DrawTextEx(custom_font, TextFormat("%s: %s", g_mq.messages[i].sender, g_mq.messages[i].text), (Vector2) { x_pos_msg, y_pos_msg }, font_size, 1, BLACK);
    }
    EndScissorMode();
    // END SCISSOR MODE

    /* GuiSliderBar((Rectangle){ 590, 385, 145, 15}, "WIDTH", TextFormat("%i", (int)panel_content_rec.width),
     * &panel_content_rec.width, 1, 2000); */
    /* GuiSliderBar((Rectangle){ 590, 410, 145, 15 }, "HEIGHT", TextFormat("%i", (int)panel_content_rec.height),
     * &panel_content_rec.height, 1, 2000); */
}

void show_fps()
{
    DrawText(TextFormat("FPS: %d", GetFPS()), 50, 50, 30, RED);
}

void init_server_button(bool* is_connected)
{
    if (GuiButton((Rectangle) { (WINDOW_WIDTH - 130) / 2.0, WINDOW_HEIGHT / 2.0, 130, 50 }, "INIT THE SERVER")) {
        init_server();
        if (is_server_running()) {
            *is_connected = true;
        } else {
            *is_connected = false;
        }
    }
}

/**
 * @brief Display an introduction or overview (purpose) of the application
 */
void introduction_window(Font custom_font)
{
    center_text_horizontally("Overview of this application!", 50, 100, RED, custom_font);
    center_text_horizontally("Hi, thank you for using this application. You are running the server", 20, 150, RED, custom_font);
    center_text_horizontally(TextFormat("You are the host of this server and your port number is: %d", PORT), 20, 190, RED, custom_font);
    center_text_horizontally("I made this application just for fun, so I hope you don't expect much from it.", 20, 230, RED, custom_font);
    center_text_horizontally("If you made it here, you should know this app lets you chat and share files with other peers in a LAN.", 20, 270, RED, custom_font);
    center_text_horizontally("There is nothing much to say; happy coding, and good luck!!!", 30, 310, RED, custom_font);
}

static void on_server_msg(const char* msg, const char* username)
{
    TraceLog(LOG_INFO, "Add a message");
    add_message(&g_mq, username, msg);
}

int main()
{
    const char* window_title = "C&F";
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, window_title);
    SetTargetFPS(FPS);
    Font custom_font = LoadFont("resources/fonts/IosevkaNerdFontMono-Regular.ttf");
    // Font inter_font = LoadFont("resources/fonts/static/Inter_18pt-Medium.ttf");
    Font hack_font_bold = LoadFont("resources/fonts/Hack-Bold.ttf");

    init_message_queue(&g_mq);
    /* char *username = malloc(256); */
    bool is_connected = false;
    server_set_msg_cb(on_server_msg);

    while (!WindowShouldClose()) {
        ClearBackground(RAYWHITE);

        BeginDrawing();

        if (!is_connected) {
            init_server_button(&is_connected);
            introduction_window(hack_font_bold);
        }

        if (is_connected) {
            server_accept_client();
            server_recv_msgs();
            panel_scroll_msg(custom_font);
        }

        // Display welcome mesasge at the center horizontally
        // welcome_msg(hack_font_bold);
        text_input(is_connected);
        setting_button();
        debugging_button();

        if (debugging) {
            show_fps();
            DrawText(TextFormat("is server running = %s", is_server_running() == true ? "true" : "false"), 10, 10, 30, RED);
        }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}
