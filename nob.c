#define _POSIX_C_SOURCE 200809L
#define NOB_IMPLEMENTATION
#include "./thirdparty/nob.h"

#include <string.h>

static bool cstr_equal(const char* left, const char* right)
{
    return left && right && strcmp(left, right) == 0;
}

static void append_common_flags(Nob_Cmd* command)
{
    nob_cmd_append(command, "-std=c11", "-D_POSIX_C_SOURCE=200809L");
    nob_cmd_append(command, "-Wall", "-Wextra", "-Werror", "-I./src");
}

static bool build_and_run_tests(const char* compiler)
{
    const char* tests[][5] = {
        { "protocol", "src/test/test_protocol.c", "src/protocol.c", NULL, NULL },
        { "relay_policy", "src/test/test_relay_policy.c", "src/relay_policy.c",
            "src/protocol.c", NULL },
        { "file_transfer", "src/test/test_file_transfer.c", "src/file_transfer.c",
            "src/protocol.c", NULL },
        { "client_network", "src/test/test_client_network.c", "src/client_network.c",
            "src/protocol.c", NULL }
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        Nob_Cmd command = { 0 };
        nob_cmd_append(&command, compiler);
        append_common_flags(&command);
        nob_cmd_append(&command, "-I./src/test");
        const char* executable = nob_temp_sprintf("build/test_%s", tests[i][0]);
        nob_cmd_append(&command, "-o", executable, tests[i][1], tests[i][2]);
        if (tests[i][3])
            nob_cmd_append(&command, tests[i][3]);
        nob_cmd_append(&command, "src/test/unity.c");
#ifdef _WIN32
        if (cstr_equal(tests[i][0], "file_transfer"))
            nob_cmd_append(&command, "-lbcrypt");
        if (cstr_equal(tests[i][0], "client_network"))
            nob_cmd_append(&command, "-lws2_32", "-lpthread");
#else
        if (cstr_equal(tests[i][0], "client_network"))
            nob_cmd_append(&command, "-lpthread");
#endif
        if (!nob_cmd_run_sync(command))
            return false;
        command.count = 0;
        nob_cmd_append(&command, executable);
        if (!nob_cmd_run_sync(command))
            return false;
        nob_cmd_free(command);
    }
    return true;
}

int main(int argc, char** argv)
{
    NOB_GO_REBUILD_URSELF(argc, argv);
    if (!nob_mkdir_if_not_exists("build"))
        return 1;

    if (argc >= 2 && cstr_equal(argv[1], "test"))
        return build_and_run_tests("gcc") ? 0 : 1;
    if (argc >= 2 && cstr_equal(argv[1], "run")) {
        Nob_Cmd command = { 0 };
        nob_cmd_append(&command, "./build/client_gui");
        bool ok = nob_cmd_run_sync(command);
        nob_cmd_free(command);
        return ok ? 0 : 1;
    }
    if (argc >= 2 && cstr_equal(argv[1], "server")) {
        Nob_Cmd command = { 0 };
        nob_cmd_append(&command, "./build/server");
        bool ok = nob_cmd_run_sync(command);
        nob_cmd_free(command);
        return ok ? 0 : 1;
    }

    bool target_windows = false;
#ifdef _WIN32
    target_windows = true;
#else
    if (argc >= 2 && (cstr_equal(argv[1], "win") || cstr_equal(argv[1], "windows")))
        target_windows = true;
#endif
    const char* compiler = target_windows ? "x86_64-w64-mingw32-gcc" : "gcc";
    const char* raylib_include = target_windows
        ? "./thirdparty/raylib-windows/include"
        : "./thirdparty/raylib-5.5_linux_amd64/include";
    const char* raylib_library = target_windows
        ? "./thirdparty/raylib-windows/lib"
        : "./thirdparty/raylib-5.5_linux_amd64/lib";

    Nob_Cmd command = { 0 };
    nob_cmd_append(&command, compiler);
    append_common_flags(&command);
    nob_cmd_append(&command, "-I", raylib_include, "-o",
        target_windows ? "build/client_gui.exe" : "build/client_gui");
    nob_cmd_append(&command,
        "src/client_gui.c",
        "src/warning_dialog.c",
        "src/client_network.c",
        "src/message.c",
        "src/file_transfer.c",
        "src/protocol.c",
        "src/ui_components.c",
        "thirdparty/tinyfiledialogs.c",
        "-L", raylib_library);
    if (target_windows) {
        nob_cmd_append(&command, "-lraylib", "-lopengl32", "-lgdi32", "-lwinmm",
            "-lws2_32", "-lbcrypt", "-lpthread", "-lcomdlg32", "-lole32");
    } else {
        nob_cmd_append(&command,
            "-Wl,-rpath,$ORIGIN/../thirdparty/raylib-5.5_linux_amd64/lib",
            "-lraylib", "-lpthread", "-ldl", "-lrt", "-lX11", "-lm");
    }
    if (!nob_cmd_run_sync(command))
        return 1;

    command.count = 0;
    nob_cmd_append(&command, compiler);
    append_common_flags(&command);
    nob_cmd_append(&command, "-o", target_windows ? "build/server.exe" : "build/server",
        "src/server.c", "src/server_cli.c", "src/relay_policy.c", "src/protocol.c");
    if (target_windows)
        nob_cmd_append(&command, "-lws2_32");
    if (!nob_cmd_run_sync(command))
        return 1;

    if (target_windows
        && !nob_copy_file("thirdparty/raylib-windows/lib/raylib.dll", "build/raylib.dll"))
        nob_log(NOB_WARNING, "Could not copy raylib.dll beside the Windows executable");
    nob_cmd_free(command);
    return 0;
}
