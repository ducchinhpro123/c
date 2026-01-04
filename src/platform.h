#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    
    // IMPORTANT: These must be defined BEFORE including any Windows headers
    // to prevent conflicts with raylib types (Rectangle, CloseWindow, ShowCursor, etc.)
    #define NOGDI
    #define NOUSER
    #define NOMCX
    #define NOIME
    
    // Include winsock2.h BEFORE windows.h to avoid conflicts
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <direct.h>
    #include <sys/types.h>
    #include <sys/stat.h>

    // Compatibility for S_ISREG on Windows
    #ifndef S_ISREG
        #define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
    #endif

    typedef int socklen_t;

    // Windows doesn't have usleep, use Sleep (milliseconds)
    #define usleep(us) Sleep((us) / 1000)

    static inline int init_network(void) {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2,2), &wsa);
    }

    static inline void cleanup_network(void) {
        WSACleanup();
    }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <poll.h>

    #define closesocket close

    static inline int init_network(void) { return 0; }
    static inline void cleanup_network(void) { }
#endif

// Cross-platform wait for socket writable (returns 1 if ready, 0 if timeout, -1 on error)
static inline int wait_socket_writable(int socket_fd, int timeout_ms) {
#ifdef _WIN32
    fd_set write_fds;
    struct timeval tv;
    FD_ZERO(&write_fds);
    FD_SET(socket_fd, &write_fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(socket_fd + 1, NULL, &write_fds, NULL, &tv);
#else
    struct pollfd pfd;
    pfd.fd = socket_fd;
    pfd.events = POLLOUT;
    return poll(&pfd, 1, timeout_ms);
#endif
}

#endif // PLATFORM_H
