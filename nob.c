#define NOB_IMPLEMENTATION
#include "./thirdparty/nob.h"
#include <string.h>

static bool nob_cstr_eq(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);

    if (!nob_mkdir_if_not_exists("build")) return 1;

    // Declaration of variables used across blocks
    const char *cc = "gcc";
    Nob_Cmd cmd = {0};

    // Argument handling for subcommands
    if (argc >= 2) {
        if (nob_cstr_eq(argv[1], "test")) {
            nob_log(NOB_INFO, "Building tests...");
            
            // Linux only for now or MinGW
#ifdef _WIN32
            const char *raylib_include = "./thirdparty/raylib-windows/include";
            const char *raylib_lib = "./thirdparty/raylib-windows/lib";
#else
            const char *raylib_include = "./thirdparty/raylib-5.5_linux_amd64/include";
            const char *raylib_lib = "./thirdparty/raylib-5.5_linux_amd64/lib";
#endif

            nob_cmd_append(&cmd, cc);
            nob_cmd_append(&cmd, "-Wall", "-Wextra", "-g");
            nob_cmd_append(&cmd, "-I./src", "-I./src/test");
            nob_cmd_append(&cmd, "-I", raylib_include);
            nob_cmd_append(&cmd, "-o", "build/test_client_logic");
            nob_cmd_append(&cmd,
                "src/test/test_client_logic.c",
                "src/client_logic.c",
                "src/test/unity.c");
            nob_cmd_append(&cmd, "-L", raylib_lib);
#ifdef _WIN32
            nob_cmd_append(&cmd, "-lraylib", "-lopengl32", "-lgdi32", "-lwinmm", "-lws2_32", "-lpthread");
#else
            nob_cmd_append(&cmd, "-Wl,-rpath,$ORIGIN/../thirdparty/raylib-5.5_linux_amd64/lib");
            nob_cmd_append(&cmd, "-lraylib", "-lpthread", "-ldl", "-lrt", "-lX11", "-lm");
#endif
            if (!nob_cmd_run_sync(cmd)) return 1;

            nob_log(NOB_INFO, "Building test_file_transfer...");
            cmd.count = 0;
            nob_cmd_append(&cmd, cc);
            nob_cmd_append(&cmd, "-Wall", "-Wextra", "-g");
            nob_cmd_append(&cmd, "-I./src", "-I./src/test");
            nob_cmd_append(&cmd, "-I", raylib_include);
            nob_cmd_append(&cmd, "-o", "build/test_file_transfer");
            nob_cmd_append(&cmd,
                "src/test/test_file_transfer.c",
                "src/client_logic.c",
                "src/file_transfer.c",
                "src/packet_queue.c",
                "src/test/unity.c");
            nob_cmd_append(&cmd, "-L", raylib_lib);
#ifdef _WIN32
            nob_cmd_append(&cmd, "-lraylib", "-lopengl32", "-lgdi32", "-lwinmm", "-lws2_32", "-lpthread");
#else
            nob_cmd_append(&cmd, "-Wl,-rpath,$ORIGIN/../thirdparty/raylib-5.5_linux_amd64/lib");
            nob_cmd_append(&cmd, "-lraylib", "-lpthread", "-ldl", "-lrt", "-lX11", "-lm");
#endif
            if (!nob_cmd_run_sync(cmd)) return 1;

            nob_log(NOB_INFO, "Building test_packet_queue...");
            cmd.count = 0;
            nob_cmd_append(&cmd, cc);
            nob_cmd_append(&cmd, "-Wall", "-Wextra", "-g");
            nob_cmd_append(&cmd, "-I./src", "-I./src/test");
            nob_cmd_append(&cmd, "-o", "build/test_packet_queue");
            nob_cmd_append(&cmd,
                "src/test/test_packet_queue.c",
                "src/packet_queue.c",
                "src/test/unity.c");
            nob_cmd_append(&cmd, "-lpthread");
            if (!nob_cmd_run_sync(cmd)) return 1;

            // Run the tests
            nob_log(NOB_INFO, "Running client logic tests...");
            cmd.count = 0;
            nob_cmd_append(&cmd, "./build/test_client_logic");
            if (!nob_cmd_run_sync(cmd)) return 1;

            nob_log(NOB_INFO, "Running file transfer tests...");
            cmd.count = 0;
            nob_cmd_append(&cmd, "./build/test_file_transfer");
            if (!nob_cmd_run_sync(cmd)) return 1;

            nob_log(NOB_INFO, "Running packet queue tests...");
            cmd.count = 0;
            nob_cmd_append(&cmd, "./build/test_packet_queue");
            if (!nob_cmd_run_sync(cmd)) return 1;

            nob_cmd_free(cmd);
            return 0;
        } else if (nob_cstr_eq(argv[1], "benchmark")) {
            nob_log(NOB_INFO, "Building benchmark...");
            
#ifdef _WIN32
            const char *raylib_include = "./thirdparty/raylib-windows/include";
            const char *raylib_lib = "./thirdparty/raylib-windows/lib";
#else
            const char *raylib_include = "./thirdparty/raylib-5.5_linux_amd64/include";
            const char *raylib_lib = "./thirdparty/raylib-5.5_linux_amd64/lib";
#endif

            nob_cmd_append(&cmd, cc);
            nob_cmd_append(&cmd, "-O3", "-Wall", "-Wextra"); // Optimize for benchmark
            nob_cmd_append(&cmd, "-I./src", "-I./src/test");
            nob_cmd_append(&cmd, "-I", raylib_include);
            nob_cmd_append(&cmd, "-o", "build/benchmark");
            nob_cmd_append(&cmd,
                "src/test/benchmark_throughput.c",
                "src/client_logic.c",
                "src/test/unity.c");
            
            // Link Raylib for TraceLog
            nob_cmd_append(&cmd, "-L", raylib_lib);
#ifdef _WIN32
            nob_cmd_append(&cmd, "-lraylib", "-lopengl32", "-lgdi32", "-lwinmm", "-lws2_32", "-lpthread");
            // Keep raylib.dll
            if (!nob_copy_file("thirdparty/raylib-windows/lib/raylib.dll", "build/raylib.dll")) {}
#else
            nob_cmd_append(&cmd, "-Wl,-rpath,$ORIGIN/../thirdparty/raylib-5.5_linux_amd64/lib");
            nob_cmd_append(&cmd, "-lraylib", "-lpthread", "-ldl", "-lrt", "-lX11", "-lm");
#endif

            if (!nob_cmd_run_sync(cmd)) return 1;

            // Run the benchmark
            nob_log(NOB_INFO, "Running benchmark...");
            cmd.count = 0;
            nob_cmd_append(&cmd, "./build/benchmark");
            if (!nob_cmd_run_sync(cmd)) return 1;

            nob_cmd_free(cmd);
            return 0;
        } else if (nob_cstr_eq(argv[1], "run")) {
            nob_cmd_append(&cmd, "./build/client_gui");
            if (!nob_cmd_run_sync(cmd)) return 1;
            nob_cmd_free(cmd);
            return 0;
        } else if (nob_cstr_eq(argv[1], "server")) {
            nob_cmd_append(&cmd, "./build/server");
            if (!nob_cmd_run_sync(cmd)) return 1;
            nob_cmd_free(cmd);
            return 0;
        }
    }

    bool target_windows = false;
#ifdef _WIN32
    target_windows = true;
#else
    if (argc >= 2 && (nob_cstr_eq(argv[1], "win") || nob_cstr_eq(argv[1], "windows"))) {
        target_windows = true;
    }
#endif

    cc = target_windows ? "x86_64-w64-mingw32-gcc" : "gcc";
    const char *raylib_include = target_windows
        ? "./thirdparty/raylib-windows/include"
        : "./thirdparty/raylib-5.5_linux_amd64/include";
    const char *raylib_lib = target_windows
        ? "./thirdparty/raylib-windows/lib"
        : "./thirdparty/raylib-5.5_linux_amd64/lib";

    // Re-init cmd for main build
    cmd.count = 0;

    if (target_windows) {
        // --- Windows Build ---
        nob_log(NOB_INFO, "Building client_gui (Windows)...");
        nob_cmd_append(&cmd, cc);
        nob_cmd_append(&cmd, "-Wall", "-Wextra", "-I./src");
        nob_cmd_append(&cmd, "-I", raylib_include);
        nob_cmd_append(&cmd, "-o", "build/client_gui.exe");
        nob_cmd_append(&cmd,
            "src/client_gui.c",
            "src/warning_dialog.c",
            "src/client_network.c",
            "src/message.c",
            "src/file_transfer.c",
            "src/packet_queue.c",
            "src/file_transfer_state.c",
            "src/ui_components.c",
            "src/client_logic.c",
            "thirdparty/tinyfiledialogs.c");
        nob_cmd_append(&cmd, "-L", raylib_lib);
        nob_cmd_append(&cmd,
            "-lraylib",
            "-lopengl32",
            "-lgdi32",
            "-lwinmm",
            "-lws2_32",
            "-lpthread",
            "-lcomdlg32",
            "-lole32");
        if (!nob_cmd_run_sync(cmd)) return 1;

        nob_log(NOB_INFO, "Building server (Windows)...");
        cmd.count = 0;
        nob_cmd_append(&cmd, cc);
        nob_cmd_append(&cmd, "-Wall", "-Wextra", "-I./src");
        nob_cmd_append(&cmd, "-I", raylib_include);
        nob_cmd_append(&cmd, "-o", "build/server.exe");
        nob_cmd_append(&cmd,
            "src/server.c",
            "src/server_cli.c",
            "src/message.c",
            "src/file_transfer.c",
            "src/packet_queue.c");
        nob_cmd_append(&cmd, "-L", raylib_lib);
        nob_cmd_append(&cmd,
            "-lraylib",
            "-lopengl32",
            "-lgdi32",
            "-lwinmm",
            "-lws2_32",
            "-lpthread");
        if (!nob_cmd_run_sync(cmd)) return 1;

        // Convenience: keep raylib.dll next to the exe for running on Windows
        if (!nob_copy_file("thirdparty/raylib-windows/lib/raylib.dll", "build/raylib.dll")) {
            nob_log(NOB_WARNING, "Could not copy raylib.dll to build/ (Windows runtime may need it next to the exe)");
        }
    } else {
        // --- Linux Build ---
        nob_log(NOB_INFO, "Building client_gui (Linux)...");
        cmd.count = 0;
        nob_cmd_append(&cmd, cc);
        nob_cmd_append(&cmd, "-Wall", "-Wextra", "-I./src");
        nob_cmd_append(&cmd, "-I", raylib_include);
        nob_cmd_append(&cmd, "-o", "build/client_gui");
        nob_cmd_append(&cmd,
            "src/client_gui.c",
            "src/warning_dialog.c",
            "src/client_network.c",
            "src/message.c",
            "src/file_transfer.c",
            "src/packet_queue.c",
            "src/file_transfer_state.c",
            "src/ui_components.c",
            "src/client_logic.c",
            "thirdparty/tinyfiledialogs.c");
        nob_cmd_append(&cmd, "-L", raylib_lib);
        nob_cmd_append(&cmd, "-Wl,-rpath,$ORIGIN/../thirdparty/raylib-5.5_linux_amd64/lib");
        nob_cmd_append(&cmd, "-lraylib", "-lpthread", "-ldl", "-lrt", "-lX11", "-lm");
        if (!nob_cmd_run_sync(cmd)) return 1;

        nob_log(NOB_INFO, "Building server (Linux)...");
        cmd.count = 0;
        nob_cmd_append(&cmd, cc);
        nob_cmd_append(&cmd, "-Wall", "-Wextra", "-I./src");
        nob_cmd_append(&cmd, "-I", raylib_include);
        nob_cmd_append(&cmd, "-o", "build/server");
        nob_cmd_append(&cmd,
            "src/server.c",
            "src/server_cli.c",
            "src/message.c",
            "src/file_transfer.c",
            "src/packet_queue.c");
        nob_cmd_append(&cmd, "-L", raylib_lib);
        nob_cmd_append(&cmd, "-Wl,-rpath,$ORIGIN/../thirdparty/raylib-5.5_linux_amd64/lib");
        nob_cmd_append(&cmd, "-lraylib", "-lpthread", "-ldl", "-lrt", "-lX11", "-lm");
        if (!nob_cmd_run_sync(cmd)) return 1;
    }

    nob_cmd_free(cmd);
    return 0;
}
