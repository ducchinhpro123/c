#define NOB_IMPLEMENTATION
#include "./thirdparty/nob.h"

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);

    if (!nob_mkdir_if_not_exists("build")) return 1;

    Nob_Cmd cmd = {0};

#ifdef _WIN32
    // --- Windows Build ---
    
    // Build client_gui
    nob_log(NOB_INFO, "Building client_gui (Windows)...");
    cmd.count = 0;
    nob_cmd_append(&cmd, "gcc");
    nob_cmd_append(&cmd, "-Wall", "-Wextra", "-I./src");
    nob_cmd_append(&cmd, "-I./thirdparty/raylib-windows/include");
    nob_cmd_append(&cmd, "-o", "build/client_gui.exe");
    nob_cmd_append(&cmd, "src/client_gui.c", "src/warning_dialog.c", "src/client_network.c", "src/message.c", "src/file_transfer.c", "src/packet_queue.c");
    nob_cmd_append(&cmd, "-L./thirdparty/raylib-windows/lib");
    nob_cmd_append(&cmd, "-lraylib", "-lws2_32", "-lgdi32", "-lwinmm", "-lpthread");
    if (!nob_cmd_run_sync(cmd)) return 1;

    // Build server
    nob_log(NOB_INFO, "Building server (Windows)...");
    cmd.count = 0;
    nob_cmd_append(&cmd, "gcc");
    nob_cmd_append(&cmd, "-Wall", "-Wextra", "-I./src");
    nob_cmd_append(&cmd, "-I./thirdparty/raylib-windows/include");
    nob_cmd_append(&cmd, "-o", "build/server.exe");
    nob_cmd_append(&cmd, "src/server.c", "src/server_cli.c", "src/message.c", "src/file_transfer.c", "src/packet_queue.c");
    nob_cmd_append(&cmd, "-L./thirdparty/raylib-windows/lib");
    nob_cmd_append(&cmd, "-lraylib", "-lws2_32", "-lgdi32", "-lwinmm", "-lpthread");
    if (!nob_cmd_run_sync(cmd)) return 1;

#else
    // --- Linux Build ---
    
    // Build client_gui
    nob_log(NOB_INFO, "Building client_gui (Linux)...");
    cmd.count = 0;
    nob_cmd_append(&cmd, "gcc");
    nob_cmd_append(&cmd, "-Wall", "-Wextra", "-I./src");
    nob_cmd_append(&cmd, "-I./thirdparty/raylib-5.5_linux_amd64/include");
    nob_cmd_append(&cmd, "-o", "build/client_gui");
    nob_cmd_append(&cmd, "src/client_gui.c", "src/warning_dialog.c", "src/client_network.c", "src/message.c", "src/file_transfer.c", "src/packet_queue.c");
    nob_cmd_append(&cmd, "-L./thirdparty/raylib-5.5_linux_amd64/lib");
    nob_cmd_append(&cmd, "-Wl,-rpath,$ORIGIN/../thirdparty/raylib-5.5_linux_amd64/lib");
    nob_cmd_append(&cmd, "-lraylib", "-lpthread", "-ldl", "-lrt", "-lX11", "-lm");
    if (!nob_cmd_run_sync(cmd)) return 1;

    // Build server
    nob_log(NOB_INFO, "Building server (Linux)...");
    cmd.count = 0;
    nob_cmd_append(&cmd, "gcc");
    nob_cmd_append(&cmd, "-Wall", "-Wextra", "-I./src");
    nob_cmd_append(&cmd, "-I./thirdparty/raylib-5.5_linux_amd64/include");
    nob_cmd_append(&cmd, "-o", "build/server");
    nob_cmd_append(&cmd, "src/server.c", "src/server_cli.c", "src/message.c", "src/file_transfer.c", "src/packet_queue.c");
    nob_cmd_append(&cmd, "-L./thirdparty/raylib-5.5_linux_amd64/lib");
    nob_cmd_append(&cmd, "-Wl,-rpath,$ORIGIN/../thirdparty/raylib-5.5_linux_amd64/lib");
    nob_cmd_append(&cmd, "-lraylib", "-lpthread", "-ldl", "-lrt", "-lX11", "-lm");
    if (!nob_cmd_run_sync(cmd)) return 1;

#endif

    nob_cmd_free(cmd);
    return 0;
}
