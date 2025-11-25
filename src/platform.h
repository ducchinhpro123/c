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

    #define closesocket close

    static inline int init_network(void) { return 0; }
    static inline void cleanup_network(void) { }
#endif

#endif // PLATFORM_H
