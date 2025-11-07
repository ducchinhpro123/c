/*********************************************
           Author: Autumn Leaves
**********************************************/

#include "client_network.h"
#include "message.h"
#include "warning_dialog.h"
#include "window.h"
#include <stdio.h>

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
int message_count = 0;
static bool edit_mode = false;
static char text_buffer[MSG_BUFFER] = "";

// Init message queue
static MessageQueue g_mq = { 0 };

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

void text_input(ClientConnection *conn, const char *username)
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
    if (IsKeyPressed(KEY_ENTER) && !IsKeyDown(KEY_LEFT_SHIFT) && !IsKeyDown(KEY_RIGHT_SHIFT) && strlen(text_buffer) > 0)
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
        while (*s && (*s == ' ' || *s == '\t' || *s == '\r')) s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = '\0';

        if (*s)
        {
            char formatted_msg[MSG_BUFFER + 4];
            snprintf(formatted_msg, sizeof(formatted_msg), "%s: %s", username, s);
            int bytes_sent = send_msg(conn, formatted_msg);
            if (bytes_sent > 0)
            {
                add_message(&g_mq, "me", s);
                text_buffer[0] = '\0'; // reset
                was_sent = true;
                edit_mode = true;
                TraceLog(LOG_INFO, "Sent: %s", formatted_msg);
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
    center_text_horizontally("NetApp Client v1.0!", 50, 20, RED, custom_font);
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
    for (int i = 0; i < g_mq.count; i++)
    {
        int y_pos_msg = panel_view.y + 10 + i * 40 + panel_scroll.y;
        int x_pos_msg = panel_view.x + 10 - panel_scroll.x;
        DrawTextEx(custom_font, TextFormat("%s: %s", g_mq.messages[i].sender, g_mq.messages[i].text), (Vector2){x_pos_msg, y_pos_msg}, 21, 1, BLACK);
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


void connection_screen(int *port, char *server_ip, char *port_str, char *username, bool *is_connected, ClientConnection *conn)
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
    if (GuiTextBox((Rectangle){textbox_x_username, 400, textbox_width, 30}, username, USERNAME_BUFFER, username_edit_mode))
    {
        TraceLog(LOG_INFO, "username set to: %s", username);
        username_edit_mode = !username_edit_mode;
    }

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
            // Trimmed username
            char tmp[USERNAME_BUFFER];
            strncpy(tmp, username, USERNAME_BUFFER);
            char *username_trimmed = tmp, *e = tmp + strlen(tmp);

            while (*username_trimmed && (*username_trimmed == ' ' || *username_trimmed == '\t' || *username_trimmed == '\n' || *username_trimmed == '\r'))
                username_trimmed++;
            while (e > username_trimmed && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
                *--e = '\0';

            if (strcmp(username_trimmed, "") == 0)
            {
                TraceLog(LOG_WARNING, "Invalid username");
                show_error("Invalid username");
            }
            else if (connect_to_server(conn, server_ip, port_str, username_trimmed) == 0)
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

void debug_mq()
{
    for (int i = 0; i < g_mq.count; i++)
    {
        TraceLog(LOG_INFO, "%s: %s (%d)", g_mq.messages[i].sender, g_mq.messages[i].text, i + 1);
    }
}

int main()
{
    const char *window_title = "C&F"; InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, window_title);
    SetTargetFPS(FPS);
    Font custom_font = LoadFont("resources/fonts/IosevkaNerdFontMono-Regular.ttf");
    // Font inter_font = LoadFont("resources/fonts/static/Inter_18pt-Medium.ttf");
    Font hack_font_bold = LoadFont("resources/fonts/BigBlueTerm437NerdFont-Regular.ttf");

    char server_ip[16] = "127.0.0.1";
    int server_port = 8898;
    char port_str[6] = "8898";
    char username[USERNAME_BUFFER];
    memset(username, 0, sizeof(username));

    char message_recv[MSG_BUFFER];

    /* char *username = malloc(256); */
    bool is_connected = false;

    // Init client connection
    ClientConnection conn;
    init_client_connection(&conn);

    init_message_queue(&g_mq);

    while (!WindowShouldClose())
    {
        ClearBackground(RAYWHITE);

        BeginDrawing();

        if (!is_connected)
        {
            introduction_window(hack_font_bold);
            connection_screen(&server_port, server_ip, port_str, username, &is_connected, &conn);
        }

        if (is_connected)
        {
            panel_scroll_msg(custom_font);
            text_input(&conn, username);

            ssize_t bytes_recv = recv_msg(&conn, message_recv, MSG_BUFFER);
            if (bytes_recv > 0)
            {
                TraceLog(LOG_INFO, "Message received: %s", message_recv);
                char *colon_pos = strchr(message_recv, ':');
                if (colon_pos != NULL)
                {
                    char sender[256];
                    size_t sender_len = colon_pos - message_recv;
                    if (sender_len >= sizeof(sender)) sender_len = sizeof(sender) - 1;
                    strncpy(sender, message_recv, sender_len);
                    sender[sender_len] = '\0';

                    char *msg_text = colon_pos + 1;
                    while (*msg_text == ' ') msg_text ++; // skip spaces
                    add_message(&g_mq, sender, msg_text);
                }
                else
                {
                    add_message(&g_mq, "unknown", message_recv);
                }
            }
            else if (bytes_recv < 0)
            {
                is_connected = false;
                show_error("Connection lost");
            }
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
