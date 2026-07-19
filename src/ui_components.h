#ifndef UI_COMPONENTS_H
#define UI_COMPONENTS_H

#include "file_transfer.h"
#include "message.h"
#include <raylib.h>

struct ClientConnection;

// Font helper
// extern Font custom_font; // Passed as arg usually

// UI functions
void init_ui_theme(void);
void welcome_msg(Font custom_font);
void introduction_window(Font custom_font);
void connection_screen(int* port, char* server_ip, char* port_str, char* username, bool* is_connected, struct ClientConnection* conn);
void panel_scroll_msg(Font custom_font, MessageQueue* mq, bool* should_scroll);
void text_input(struct ClientConnection* conn, const char* username, MessageQueue* mq, bool* should_scroll);
void files_displaying(Font font, FileTransferModule* transfers);
void draw_transfer_status(Font custom_font, const FileTransferModule* transfers);
void draw_pending_transfers(Font custom_font, FileTransferModule* transfers,
    const RelayTransport* transport);
// Buttons/Debug
void debugging_button(bool* debugging);
void show_fps(void);

#endif // UI_COMPONENTS_H
