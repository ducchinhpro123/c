#include "server.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
    #include <unistd.h>
#endif

static volatile sig_atomic_t g_running = 1;


static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

int main(void)
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // No message callback - server runs silently, just relays messages
    server_set_msg_cb(NULL);

    if (!init_server()) {
        fprintf(stderr, "Failed to initialize server on port %d\n", PORT);
        return EXIT_FAILURE;
    }

    printf("Server running on port %d. Press Ctrl+C to stop.\n", PORT);
    fflush(stdout);

    int prev_client_count = get_client_count();

    while (g_running) {
        server_accept_client();
        server_recv_msgs();

        int cur_client_count = get_client_count();
        if (cur_client_count != prev_client_count) {
            printf("Connected clients: %d\n", cur_client_count);
            fflush(stdout);

            prev_client_count = cur_client_count;
        }

        // Small sleep to avoid busy loop
        usleep(20 * 1000); // 20ms
    }

    printf("Shutting down server...\n");
    fflush(stdout);
    cleanup_server();
    return EXIT_SUCCESS;
}
