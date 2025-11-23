#define NOB_IMPLEMENTATION
#include "./thirdparty/nob.h"

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);

    // Create build directory if it doesn't exist
    if (!nob_mkdir_if_not_exists("build")) return 1;

    Nob_Cmd cmd = {0};

    // --- Build client_gui ---
    nob_log(NOB_INFO, "Building client_gui...");
    cmd.count = 0;
    nob_cmd_append(&cmd, "gcc");
    nob_cmd_append(&cmd, "-Wall", "-Wextra", "-I./src");
    nob_cmd_append(&cmd, "-I./thirdparty/raylib-5.5_linux_amd64/include"); // Local Raylib include
    nob_cmd_append(&cmd, "-o", "build/client_gui");
    nob_cmd_append(&cmd, "src/client_gui.c", "src/warning_dialog.c", "src/client_network.c", "src/message.c", "src/file_transfer.c");
    nob_cmd_append(&cmd, "-L./thirdparty/raylib-5.5_linux_amd64/lib"); // Local Raylib lib
    nob_cmd_append(&cmd, "-Wl,-rpath,$ORIGIN/../thirdparty/raylib-5.5_linux_amd64/lib"); // Add rpath
    nob_cmd_append(&cmd, "-lraylib", "-lpthread", "-ldl", "-lrt", "-lX11", "-lm");
    if (!nob_cmd_run_sync(cmd)) return 1;

    // --- Build server (CLI) ---
    nob_log(NOB_INFO, "Building server...");
    cmd.count = 0;
    nob_cmd_append(&cmd, "gcc");
    nob_cmd_append(&cmd, "-Wall", "-Wextra", "-I./src");
    nob_cmd_append(&cmd, "-I./thirdparty/raylib-5.5_linux_amd64/include"); // Local Raylib include
    nob_cmd_append(&cmd, "-o", "build/server");
    // Assuming server_cli.c contains the main function for the CLI server
    nob_cmd_append(&cmd, "src/server.c", "src/server_cli.c", "src/message.c", "src/file_transfer.c");
    nob_cmd_append(&cmd, "-L./thirdparty/raylib-5.5_linux_amd64/lib"); // Local Raylib lib
    nob_cmd_append(&cmd, "-Wl,-rpath,$ORIGIN/../thirdparty/raylib-5.5_linux_amd64/lib"); // Add rpath
    nob_cmd_append(&cmd, "-lraylib", "-lpthread", "-ldl", "-lrt", "-lX11", "-lm"); 
    if (!nob_cmd_run_sync(cmd)) return 1;

    nob_cmd_free(cmd);
    return 0;
}
