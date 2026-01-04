#ifndef UI_COMPONENTS_H
#define UI_COMPONENTS_H

#include <raylib.h>
#include "packet_queue.h"
#include "message.h"

struct ClientConnection;

// Font helper
// extern Font custom_font; // Passed as arg usually

// UI functions
void welcome_msg(Font custom_font);
void introduction_window(Font custom_font);
void connection_screen(int* port, char* server_ip, char* port_str, char* username, bool* is_connected, struct ClientConnection* conn);
void panel_scroll_msg(Font custom_font, MessageQueue* mq, bool* should_scroll);
void text_input(struct ClientConnection* conn, const char* username, MessageQueue* mq, bool* should_scroll);
void files_displaying(Font font);
void draw_transfer_status(Font custom_font);
void draw_pending_transfers(Font custom_font, struct ClientConnection* conn);  // New: pending transfer dialog
void center_text_horizontally(const char* text, int font_size, int y, Color color, Font custom_font);

// Buttons/Debug
void setting_button(void);
void debugging_button(bool* debugging);
void show_fps(void);
void debug_mq(MessageQueue* mq);

#endif // UI_COMPONENTS_H
