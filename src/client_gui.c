// platform.h must precede Windows and raylib headers.
#include "platform.h"

#include <raylib.h>

#include "client_network.h"
#include "file_transfer.h"
#include "message.h"
#include "ui_components.h"
#include "warning_dialog.h"
#include "window.h"

#include <stdio.h>
#include <string.h>

#define FPS 60
#define UI_FONT_BASE_SIZE 64
#define USERNAME_BUFFER 64

typedef struct {
    MessageQueue* messages;
    FileTransferModule* transfers;
    const RelayTransport* transport;
    bool* should_scroll;
} ClientUiContext;

static MessageQueue message_queue;
static bool should_scroll_to_bottom;

static void ensure_asset_workdir(void)
{
    if (FileExists("resources/fonts/Inter-VariableFont_opsz,wght.ttf"))
        return;
#ifdef _WIN32
    _chdir("..");
#else
    (void)chdir("..");
#endif
    if (!FileExists("resources/fonts/Inter-VariableFont_opsz,wght.ttf"))
        TraceLog(LOG_ERROR, "Asset directory not found: expected resources/fonts near executable");
}

static void transfer_notice(void* context, const char* message)
{
    MessageQueue* messages = context;
    add_message(messages, "SYSTEM", message);
    should_scroll_to_bottom = true;
}

static bool is_file_message(RelayMessageType type)
{
    return type >= RELAY_MESSAGE_FILE_OFFER_CREATED
        && type <= RELAY_MESSAGE_FILE_TRANSFER_CANCEL;
}

static void handle_server_message(void* context, const RelayMessage* message)
{
    ClientUiContext* ui = context;
    if (message->type == RELAY_MESSAGE_CHAT_DELIVER) {
        add_message(ui->messages, message->as.chat_deliver.display_name,
            message->as.chat_deliver.text);
        *ui->should_scroll = true;
        return;
    }
    if (message->type == RELAY_MESSAGE_WELCOME) {
        add_message(ui->messages, "SYSTEM", "Connected to the Relay Workspace");
        *ui->should_scroll = true;
        return;
    }
    if (is_file_message(message->type) || message->type == RELAY_MESSAGE_ACTION_REJECTED)
        file_transfer_handle_message(ui->transfers, ui->transport, message);
    if (message->type == RELAY_MESSAGE_ACTION_REJECTED) {
        add_message(ui->messages, "SYSTEM", message->as.action_rejected.reason);
        *ui->should_scroll = true;
    }
}

static void process_file_drop(FileTransferModule* transfers,
    const RelayTransport* transport)
{
    if (!IsFileDropped())
        return;
    FilePathList dropped = LoadDroppedFiles();
    for (unsigned i = 0; i < dropped.count; ++i) {
        if (!file_transfer_offer_file(transfers, transport, dropped.paths[i]))
            show_error("The file could not be offered");
    }
    UnloadDroppedFiles(dropped);
}

int main(void)
{
    if (init_network() != 0) {
        fprintf(stderr, "Failed to initialize network\n");
        return 1;
    }
    ClientConnection* connection = client_connection_create();
    if (!connection) {
        cleanup_network();
        return 1;
    }
    init_message_queue(&message_queue);
    FileTransferModule* transfers = file_transfer_create("received", transfer_notice,
        &message_queue);
    if (!transfers) {
        destroy_message_queue(&message_queue);
        client_connection_destroy(connection);
        cleanup_network();
        return 1;
    }
    RelayTransport transport = client_connection_transport(connection);
    ClientUiContext ui = {
        .messages = &message_queue,
        .transfers = transfers,
        .transport = &transport,
        .should_scroll = &should_scroll_to_bottom
    };

    ensure_asset_workdir();
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Relay - private LAN chat and file transfer");
    SetTargetFPS(FPS);
    Font font = LoadFontEx("resources/fonts/Inter-VariableFont_opsz,wght.ttf",
        UI_FONT_BASE_SIZE, NULL, 0);
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    init_ui_theme();
    GuiSetFont(font);

    char server_ip[256] = "127.0.0.1";
    int server_port = 8898;
    char port_text[6] = "8898";
    char display_name[USERNAME_BUFFER] = { 0 };
    bool connected = false;
    bool debugging = false;

    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground((Color) { 245, 247, 251, 255 });

        if (!connected) {
            introduction_window(font);
            connection_screen(&server_port, server_ip, port_text, display_name,
                &connected, connection);
        } else {
            welcome_msg(font);
            panel_scroll_msg(font, &message_queue, &should_scroll_to_bottom);
            text_input(connection, display_name, &message_queue, &should_scroll_to_bottom);

            if (client_connection_poll(connection, handle_server_message, &ui) < 0) {
                connected = false;
                file_transfer_abort_all(transfers, "Connection lost; active File Transfers stopped");
                disconnect_from_server(connection);
                show_error("Connection lost");
            } else {
                file_transfer_pump(transfers, &transport);
                process_file_drop(transfers, &transport);
                files_displaying(font, transfers);
                if (file_transfer_active_count(transfers) > 0)
                    draw_transfer_status(font, transfers);
                if (file_transfer_pending_count(transfers) > 0)
                    draw_pending_transfers(font, transfers, &transport);
            }
        }

        debugging_button(&debugging);
        if (debugging)
            show_fps();
        draw_warning_dialog();
        EndDrawing();
    }

    UnloadFont(font);
    file_transfer_destroy(transfers);
    client_connection_destroy(connection);
    destroy_message_queue(&message_queue);
    cleanup_network();
    CloseWindow();
    return 0;
}
