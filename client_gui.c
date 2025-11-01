#include "client_network.h"
#include "message.h"
#include "warning_dialog.h"
#include "window.h"

#include <raygui.h>
#include <raylib.h>
#include <stdlib.h>
#include <string.h>

/* #include "server_gui.h" */

#define FPS 60

Message messages[MAX_MESSAGES];
int message_count = 0;
static bool edit_mode = false;
static char text_buffer[MSG_BUFFER] = "";

bool debugging = false;

void debugging_button()
{
    if (GuiButton((Rectangle){WINDOW_WIDTH - 80, 40, 80, 20}, "#152#Debugging"))
    {
        debugging = !debugging;
    }
}

void setting_button()
{
    if (GuiButton((Rectangle){WINDOW_WIDTH - 70, 10, 70, 20}, "#142#Settings"))
    {
        // TODO: handle setting button
    }
}

void center_text_horizontally(const char *text, int font_size, int y, Color color, Font custom_font)
{
    if (custom_font.baseSize == 0)
    {
        int text_width = MeasureText(text, font_size);
        int x = (WINDOW_WIDTH - text_width) / 2;
        DrawText(text, x, y, font_size, color);
    }
    else
    {
        Vector2 text_pos = MeasureTextEx(custom_font, text, font_size, 1);
        int x = (WINDOW_WIDTH - text_pos.x) / 2;
        DrawTextEx(custom_font, text, (Vector2){x, y}, font_size, 1, color);
    }
}
//
// void send_msg(char msg[MSG_BUFFER])
// {
//     DrawText(msg, WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2, 30, BLACK);
// }

void text_input(ClientConnection *conn)
{
    float box_width = 500;
    float box_height = 50;
    float x_pos = (WINDOW_WIDTH - box_width) / 2;
    float y_pos = (WINDOW_HEIGHT - box_height) - 10;
    bool was_sent = false;

    /* GuiWindowBox(Rectangle bounds, const char *title); */
    // Input text
    if (GuiTextBox((Rectangle){x_pos - ((box_width / 6) / 2), y_pos, box_width, box_height}, text_buffer, MSG_BUFFER, edit_mode))
    {
        edit_mode = !edit_mode;
    }

    // Button to send text
    if (GuiButton((Rectangle){x_pos + box_width - ((box_width / 6) / 2), y_pos, box_width / 6, box_height}, "SEND"))
    {
        was_sent = true;
    }
    if (IsKeyPressed(KEY_ENTER) && !IsKeyDown(KEY_LEFT_SHIFT) && !IsKeyDown(KEY_RIGHT_SHIFT))
    {
        was_sent = true;
    }

    if (was_sent)
    {
        char tmp[MSG_BUFFER];
        strncpy(tmp, text_buffer, MSG_BUFFER);
        tmp[MSG_BUFFER - 1] = '\0';
        // simple trim message
        char *s = tmp, *e = tmp + strlen(tmp);
        while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';

        if (*s)
        {
            int bytes_sent = send_msg(conn, s);
            if (bytes_sent > 0)
            {
                text_buffer[0] = '\0';
                was_sent = true;
                edit_mode = true;
            }
        }
        else
        {
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
    center_text_horizontally("Welcome to the CLIENT!", 70, 20, RED, custom_font);
}

void panel_scroll_msg(Font custom_font)
{
    /* bool showContentArea = true; */
    int panel_width = 800;
    int panel_height = 700;

    int x_pos = (WINDOW_WIDTH - panel_width) / 2;
    int y_pos = (WINDOW_HEIGHT - panel_height) / 2;

    Rectangle panel_rec = {x_pos, y_pos, panel_width, panel_height};  // The position of pannel
    Rectangle panel_content_rec = {0, 0, panel_width - 15, 1200};
    Rectangle panel_view = {0};

    static Vector2 panel_scroll = {0, 0};
    GuiScrollPanel(panel_rec, "CHAT", panel_content_rec, &panel_scroll, &panel_view);

    // BEGIN SCISSOR MODE
    BeginScissorMode(panel_view.x, panel_view.y, panel_view.width, panel_view.height);
    DrawCircle(panel_view.y + 400, panel_scroll.y + 500, 40, RED);
    /* for (int i = 0; i < 30; i++) { */
    /*     int y_pos_msg = panel_view.y + 10 + i * 40 + panel_scroll.y; */
    /*     int x_pos_msg = panel_view.x + 10 - panel_scroll.x; */
    /*     DrawTextEx(custom_font, TextFormat("Message %d: Hello from user", i), (Vector2)
     * {x_pos_msg, y_pos_msg}, 16, 1, BLACK); */
    /* } */
    EndScissorMode();
    // END SCISSOR MODE

    /* GuiSliderBar((Rectangle){ 590, 385, 145, 15}, "WIDTH", TextFormat("%i",
     * (int)panel_content_rec.width), &panel_content_rec.width, 1, 2000); */
    /* GuiSliderBar((Rectangle){ 590, 410, 145, 15 }, "HEIGHT", TextFormat("%i",
     * (int)panel_content_rec.height), &panel_content_rec.height, 1, 2000); */
}

void show_fps()
{
    DrawText(TextFormat("FPS: %d", GetFPS()), 50, 50, 30, RED);
}

// Enter username in here
void input_username()
{
}

/**
 * @brief Display an introduction or overview (purpose) of the application
 */
void introduction_window(Font custom_font)
{
    center_text_horizontally("Overview of this application!", 50, 100, RED, custom_font);
    center_text_horizontally("Hi, thank you for using this application. You are a peer in a LAN", 20, 150, RED, custom_font);
    center_text_horizontally("I made this application just for fun, so I hope you don't expect much from it.", 20, 230, RED, custom_font);
    center_text_horizontally("If you made it here, you should know this app lets you chat and "
                             "share files with other peers in a LAN.",
                             20, 270, RED, custom_font);
    center_text_horizontally("There is nothing much to say; happy coding, and good luck!!!", 30, 310, RED, custom_font);
}

void connection_screen(Font font, int *port, char *server_ip, char *port_str, bool *is_connected, ClientConnection *conn)
{
    static bool ip_edit_mode = false;
    static bool port_edit_mode = false;

    // Input server ip
    int label_width_host = MeasureText("Server IP:", 20);
    int label_width_port = MeasureText("Port number:", 20);

    int textbox_width = 100;
    int spacing = 10;

    int group_width_host = label_width_host + spacing + textbox_width;
    int group_width_port = label_width_port + spacing + textbox_width;

    int group_center_x = WINDOW_WIDTH / 2;

    int label_x_host = group_center_x - group_width_host / 2;
    int label_x_port = group_center_x - group_width_port / 2;

    int textbox_x_host = label_x_host + label_width_host + spacing;
    int textbox_x_port = label_x_port + label_width_port + spacing + 10;

    // Input host
    DrawText("Server IP:", label_x_host, 455, 20, DARKGRAY);
    if (GuiTextBox((Rectangle){textbox_x_host, 450, textbox_width, 30}, server_ip, 30, ip_edit_mode))
    {
        TraceLog(LOG_INFO, "Port set to: %s", server_ip);
        ip_edit_mode = !ip_edit_mode;
    }

    // Input port
    DrawText("Port number:", label_x_port, 495, 20, DARKGRAY);
    if (GuiTextBox((Rectangle){textbox_x_port, 490, 100, 30}, port_str, 30, port_edit_mode))
    {
        port_edit_mode = !port_edit_mode;
    }

    if (GuiButton((Rectangle){(WINDOW_WIDTH - 100) / 2.0, 550, 100, 50}, "Connect now"))
    {
        // Convert port string as a input to port number
        *port = atoi(port_str);

        if (*port < 1 || *port > 65535)
        {
            TraceLog(LOG_WARNING, "Invalid port number: %d", *port);
            show_error("Invalid port number");
        }
        else
        {
            if (connect_to_server(conn, server_ip, port_str) == 0)
            {
                *is_connected = true;
            }
            else
            {
                show_error("Connection failed");
                TraceLog(LOG_ERROR, "Connection failed %s:%s", server_ip, port_str);
            }
        }

        TraceLog(LOG_INFO, "Port set to: %d", *port);
        TraceLog(LOG_INFO, "Server host set to: %s", server_ip);
    }
}

int main()
{
    const char *window_title = "C&F"; InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, window_title);
    SetTargetFPS(FPS);
    // Font custom_font = LoadFont("resources/fonts/IosevkaNerdFontMono-Regular.ttf");  // Iosevka Nerd Font
    Font inter_font = LoadFont("resources/fonts/static/Inter_18pt-Medium.ttf");
    Font hack_font_bold = LoadFont("resources/fonts/Hack-Bold.ttf");

    char server_ip[16] = "127.0.0.1";
    int server_port = 8898;
    char port_str[6] = "8898";

    /* char *username = malloc(256); */
    bool is_connected = false;

    // Init client connection
    ClientConnection conn;
    init_client_connection(&conn);

    // Init message queue
    MessageQueue mq = { 0 };
    init_message_queue(&mq);

    while (!WindowShouldClose())
    {
        ClearBackground(RAYWHITE);

        BeginDrawing();

        if (!is_connected)
        {
            introduction_window(hack_font_bold);
            connection_screen(hack_font_bold, &server_port, server_ip, port_str, &is_connected, &conn);
        }

        if (is_connected)
        {
            panel_scroll_msg(inter_font);
            text_input(&conn);
        }
        /* init_server_button(&is_connected); */

        /* if (is_connected) { // Only display chat interface once user is connected */
        /*     panel_scroll_msg(inter_font); */
        /* } */

        // Display welcome mesasge at the center horizontally
        welcome_msg(hack_font_bold);

        setting_button();

        debugging_button();
        if (debugging)
        {
            show_fps();
        }

        draw_warning_dialog();

        EndDrawing();
    }

    CloseWindow();
    return 0;
}
