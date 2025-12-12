#include "platform.h"
#include "server.h"
#include "file_transfer.h"
#include <errno.h>
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
    #include <fcntl.h>
#endif
static int server_fd = -1;
static Client clients[MAX_CLIENTS];
static int client_count = 0;
static bool server_running = false;
static server_msg_cb g_on_msg_cb = NULL;

// State machine variables per client
typedef enum {
    STATE_HEADER,
    STATE_BODY
} StreamState;

typedef struct {
    StreamState state;
    PacketHeader header;
    size_t bytes_needed;
} ClientState;

static ClientState client_states[MAX_CLIENTS];


typedef struct {
    bool active;
    char file_id[FILE_ID_LEN];
    int sender_fd;
    size_t total_bytes;
    size_t forwarded_bytes;
    size_t chunk_size;
} FileTransferSession;

#define MAX_FILE_SESSIONS 32
static FileTransferSession file_sessions[MAX_FILE_SESSIONS];

static void remove_client(int index);
static bool process_file_message(int client_index, uint8_t type, const char* data, size_t len);
static void send_abort_to_sender(int fd, const char* file_id, const char* reason);
static void broadcast_file_abort(const char* file_id, const char* reason, int skip_fd);
static FileTransferSession* file_session_find(const char* file_id);
static FileTransferSession* file_session_create(const char* file_id, int sender_fd, size_t total_bytes, size_t chunk_size);
static void file_session_remove(FileTransferSession* session);
static void cleanup_sessions_for_fd(int fd);
static ssize_t send_all_to_client(int socket_fd, const char* data, size_t len);

void server_set_msg_cb(server_msg_cb cb)
{
    g_on_msg_cb = cb;
}

int get_client_count()
{
    return client_count;
}
Client* get_clients()
{
    return clients;
}

int server_accept_client()
{
    if (!server_running || server_fd == -1) {
        return -1;
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

    if (client_fd == -1) {
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return -1;
        }
        TraceLog(LOG_ERROR, "Accept failed. WSA error %d", err);
#else
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return -1;
        }
        TraceLog(LOG_ERROR, "Accept failed. %s", strerror(errno));
#endif
        return -1;
    }

    // Optimize socket for LAN speed
#ifdef _WIN32
    char nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    char sendbuf[4]; *(int*)sendbuf = 8 * 1024 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, sendbuf, sizeof(sendbuf));
    char recvbuf[4]; *(int*)recvbuf = 8 * 1024 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, recvbuf, sizeof(recvbuf));
    // Non-block mode
    u_long iMode = 1;
    ioctlsocket(client_fd, FIONBIO, &iMode);
#else
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int sendbuf = 8 * 1024 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));
    int recvbuf = 8 * 1024 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &recvbuf, sizeof(recvbuf));
    // Non-block mode
    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags == -1) {
        TraceLog(LOG_ERROR, "fcntl failed: (%s)", strerror(errno));
    } else {
        if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            TraceLog(LOG_WARNING, "fcntl(F_SETFL) O_NONBLOCK failed: %s", strerror(errno));
        }
    }
#endif

    if (client_count >= MAX_CLIENTS) {
        TraceLog(LOG_WARNING, "Max clients reached, rejecting");
        closesocket(client_fd);
        return -1;
    }

    clients[client_count].sock_fd = client_fd;
    strcpy(clients[client_count].username, "Unknown");
    strcpy(clients[client_count].ip_addr, inet_ntoa(client_addr.sin_addr));
    clients[client_count].recv_len = 0;
    memset(clients[client_count].recv_buffer, 0, sizeof(clients[client_count].recv_buffer));

    // Initialize client state
    memset(&client_states[client_count], 0, sizeof(ClientState));
    client_states[client_count].state = STATE_HEADER;
    client_states[client_count].bytes_needed = sizeof(PacketHeader);

    client_count++;
    char join_msg[512];
    snprintf(join_msg, sizeof(join_msg), "SYSTEM: A new user has joined with ip: %s", clients[client_count - 1].ip_addr);

    server_broadcast_msg(join_msg, client_fd);

    // TraceLog(LOG_WARNING, "Client connected from: %s (%d/%d)", clients[client_count - 1].ip_addr, client_count, MAX_CLIENTS);

    return client_fd;
}

bool is_server_running()
{
    return server_running;
}

void cleanup_server()
{
    if (server_fd != -1) {
        TraceLog(LOG_INFO, "Cleaning the server");
        closesocket(server_fd);
        server_fd = -1;
    }

    TraceLog(LOG_INFO, "Finished clean up the server");
    server_running = false;

    cleanup_network();
}

bool init_server()
{

    init_network();
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("socket");
        TraceLog(LOG_ERROR, "Socket creation failed.");
        return false;
    }
#ifdef _WIN32
    char opt = 1;
#else
    int opt = 1;
#endif
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        TraceLog(LOG_ERROR, "Bind failed: %s", strerror(errno));
        closesocket(server_fd);
        return false;
    }
    if (listen(server_fd, 10) == -1) {
        perror("listen");
        TraceLog(LOG_ERROR, "Listen failed: %s", strerror(errno));
        closesocket(server_fd);
        return false;
    }

#ifdef _WIN32
    u_long iMode = 1;
    ioctlsocket(server_fd, FIONBIO, &iMode);
#else
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
#endif

    server_running = true;
    TraceLog(LOG_INFO, "Server is running on port %d, ready to accept connections", PORT);
    return true;
}



static void handle_client_packet(int client_index, uint8_t type, const char* data, size_t len);

void server_recv_msgs()
{
    for (int i = 0; i < client_count; ++i) {
        int fd = clients[i].sock_fd;
        
        // Loop to drain socket buffer
        while (true) {
            char buffer[65536]; // 64KB stack buffer
            int bytes_recv = recv(fd, buffer, sizeof(buffer), 0);

            if (bytes_recv > 0) {
                size_t available = sizeof(clients[i].recv_buffer) - clients[i].recv_len;
                if ((size_t)bytes_recv > available) {
                    TraceLog(LOG_ERROR, "Client %s overflowed recv buffer", clients[i].ip_addr);
                    remove_client(i);
                    i--;
                    break; // Break inner loop, outer loop continues
                }

                memcpy(clients[i].recv_buffer + clients[i].recv_len, buffer, bytes_recv);
                clients[i].recv_len += bytes_recv;
                
                // Process data immediately to free up buffer space if possible
                // (Existing processing logic here)
                // Actually, we should just append to buffer and process after loop?
                // No, if we fill up CLIENT_STREAM_BUFFER (10MB), we must process.
                // So let's keep processing logic inside the loop.


            // Initialize state if needed (hacky but works for static array)
            if (client_states[i].bytes_needed == 0) {
                client_states[i].state = STATE_HEADER;
                client_states[i].bytes_needed = sizeof(PacketHeader);
            }

            size_t processed = 0;
            while (clients[i].recv_len - processed >= client_states[i].bytes_needed) {
                if (client_states[i].state == STATE_HEADER) {
                    memcpy(&client_states[i].header, clients[i].recv_buffer + processed, sizeof(PacketHeader));
                    client_states[i].header.length = ntohl(client_states[i].header.length);
                    processed += sizeof(PacketHeader);
                    
                    if (client_states[i].header.length > 0) {
                        client_states[i].state = STATE_BODY;
                        client_states[i].bytes_needed = client_states[i].header.length;
                    } else {
                        // Empty packet
                        handle_client_packet(i, client_states[i].header.type, NULL, 0);
                        client_states[i].state = STATE_HEADER;
                        client_states[i].bytes_needed = sizeof(PacketHeader);
                    }
                } else if (client_states[i].state == STATE_BODY) {
                    handle_client_packet(i, client_states[i].header.type, clients[i].recv_buffer + processed, client_states[i].header.length);
                    processed += client_states[i].header.length;
                    client_states[i].state = STATE_HEADER;
                    client_states[i].bytes_needed = sizeof(PacketHeader);
                }
            }

            if (processed > 0) {
                size_t remaining = clients[i].recv_len - processed;
                memmove(clients[i].recv_buffer, clients[i].recv_buffer + processed, remaining);
                clients[i].recv_len = remaining;
            }
            } else if (bytes_recv == 0) {
                TraceLog(LOG_INFO, "Client disconnected (%s)", clients[i].ip_addr);
                char leave_msg[512];
                snprintf(leave_msg, sizeof(leave_msg), "SYSTEM: User %s (%s) has left",
                    clients[i].username, clients[i].ip_addr);
                remove_client(i);
                i--;
                if (client_count > 0) {
                    server_broadcast_msg(leave_msg, -1);
                }
                break; // Break inner loop
            } else {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK)
                    break; // No more data, break inner loop
                TraceLog(LOG_WARNING, "Recv failed for %s: WSA error %d", clients[i].ip_addr, err);
#else
                if (errno == EWOULDBLOCK || errno == EAGAIN)
                    break; // No more data, break inner loop
                TraceLog(LOG_WARNING, "Recv failed for %s: %s", clients[i].ip_addr, strerror(errno));
#endif
                remove_client(i);
                i--;
                break; // Break inner loop
            }
        } // End while(true)
    }
}

static void handle_client_packet(int client_index, uint8_t type, const char* data, size_t len)
{
    if (type == PACKET_TYPE_TEXT) {
        // Ensure null termination
        char* msg = malloc(BUFFER_SIZE);
        if (!msg) {
            TraceLog(LOG_ERROR, "OOM in handle_client_packet");
            return;
        }
        
        if (len >= BUFFER_SIZE) len = BUFFER_SIZE - 1;
        if (len > 0) memcpy(msg, data, len);
        msg[len] = '\0';

        const char* prefix = "USERNAME:";
        size_t prefix_len = strlen(prefix);
        if (strncmp(msg, prefix, prefix_len) == 0) {
            const char* name = msg + prefix_len;
            if (*name) {
                snprintf(clients[client_index].username, sizeof(clients[client_index].username), "%s", name);
                TraceLog(LOG_INFO, "Client %s set username to '%s'", clients[client_index].ip_addr, clients[client_index].username);
            }
            return;
        }

        // Broadcast text message
        // We need to wrap it in a text packet for other clients
        // But server_broadcast_msg handles that now (we need to update it)
        server_broadcast_msg(msg, clients[client_index].sock_fd);

        if (g_on_msg_cb) {
            g_on_msg_cb(msg, clients[client_index].username);
        }
        free(msg);
    } else if (type >= PACKET_TYPE_FILE_START && type <= PACKET_TYPE_FILE_ABORT) {
        // Forward file packets
        // We need to update process_file_message to take raw data
        if (process_file_message(client_index, type, data, len)) {
             // Broadcast the packet as-is to other clients
             // We need a new broadcast function for raw packets
             // For now, let's update server_broadcast_msg to handle types or create a new one
             // Let's create a helper to broadcast raw packet
             
             PacketHeader header;
             header.type = type;
             header.length = htonl(len);
             
             // We need to iterate clients and send header + body
             for (int i = 0; i < client_count; ++i) {
                 if (clients[i].sock_fd == clients[client_index].sock_fd) continue;
                 
                 send_all_to_client(clients[i].sock_fd, (const char*)&header, sizeof(header));
                 if (len > 0)
                    send_all_to_client(clients[i].sock_fd, data, len);
             }
        }
    }
}

static void remove_client(int index)
{
    if (index < 0 || index >= client_count)
        return;

    int fd = clients[index].sock_fd;
    cleanup_sessions_for_fd(fd);

    closesocket(fd);
    clients[index].sock_fd = -1;
    clients[index].recv_len = 0;

    for (int j = index + 1; j < client_count; ++j) {
        clients[j - 1] = clients[j];
        client_states[j - 1] = client_states[j];
    }
    client_count--;
}

// Helper: Send all data, handling partial sends and flow control
static ssize_t send_all_to_client(int socket_fd, const char* data, size_t len)
{
    size_t total_sent = 0;
    int retry_count = 0;
    
    // Increase max retries for large packets
    const int MAX_RETRIES = (len > 10000) ? 10000 : 100; // Much more retries for large packets
    
    // Calculate progressive timeout based on data size
    int base_wait_ms = 10; // Start with 10ms
    if (len > 100000) base_wait_ms = 50; // 50ms for >100KB
    if (len > 1000000) base_wait_ms = 100; // 100ms for >1MB
    
    while (total_sent < len) {
#ifdef _WIN32
        ssize_t bytes_sent = send(socket_fd, data + total_sent, (int)(len - total_sent), 0);
#else
        ssize_t bytes_sent = send(socket_fd, data + total_sent, len - total_sent, MSG_NOSIGNAL);
#endif
        if (bytes_sent < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
#endif
                // Socket buffer full, wait and retry
                retry_count++;
                if (retry_count > MAX_RETRIES) {
                    TraceLog(LOG_ERROR, "Max retries reached after %d attempts, sent %zu/%zu bytes (%.1f%%)",
                             retry_count, total_sent, len, (float)total_sent / len * 100);
                    return -1; // Return error, let caller handle it
                }
                
                // Progressive backoff with longer waits for larger data
                int wait_ms = base_wait_ms;
                
                // Increase wait time as we retry more
                if (retry_count > 100) {
                    wait_ms = base_wait_ms * 2; // Double after 100 retries
                }
                if (retry_count > 500) {
                    wait_ms = base_wait_ms * 5; // 5x after 500 retries
                }
                if (retry_count > 1000) {
                    wait_ms = base_wait_ms * 10; // 10x after 1000 retries
                }
                
                // Cap max wait to prevent excessive delays
                if (wait_ms > 1000) wait_ms = 1000; // Max 1 second
                
                TraceLog(LOG_DEBUG, "Socket buffer full, retry %d/%d, waiting %dms, sent %zu/%zu bytes",
                         retry_count, MAX_RETRIES, wait_ms, total_sent, len);
                
                usleep(wait_ms * 1000);
                continue;
            }
#ifdef _WIN32
            else if (err == WSAECONNRESET || err == WSAECONNABORTED) {
                TraceLog(LOG_WARNING, "Connection closed while sending after %zu/%zu bytes", total_sent, len);
                return -1;
            } else {
                TraceLog(LOG_ERROR, "Send error: %d (sent %zu/%zu bytes)", err, total_sent, len);
                return -1;
            }
#else
            else if (errno == EPIPE || errno == ECONNRESET) {
                TraceLog(LOG_WARNING, "Connection closed while sending after %zu/%zu bytes", total_sent, len);
                return -1;
            } else {
                TraceLog(LOG_ERROR, "Send error: %s (sent %zu/%zu bytes)", strerror(errno), total_sent, len);
                return -1;
            }
#endif
        } else if (bytes_sent == 0) {
            // This shouldn't happen with blocking send, but handle it
            retry_count++;
            if (retry_count > MAX_RETRIES) {
                TraceLog(LOG_WARNING, "Send returned 0 after %d retries, sent %zu/%zu bytes", retry_count, total_sent, len);
                return total_sent > 0 ? (ssize_t)total_sent : -1;
            }
            int wait_ms = (retry_count < 10) ? 10 : 100; // Short wait for this case
            usleep(wait_ms * 1000);
            continue;
        }
        
        total_sent += bytes_sent;
        
        // For large transfers, yield CPU occasionally but don't reset progress
        // if (total_sent > 0 && (total_sent % 32768) == 0 && len > 10000) {
        //     // Log progress for large transfers
        //     TraceLog(LOG_INFO, "Transfer progress: %zu/%zu bytes (%.1f%%)",
        //              total_sent, len, (float)total_sent / len * 100);
        //     usleep(1000); // Very brief yield, just 1ms
        // }
        
        retry_count = 0; // Reset retry counter on successful send
    }
    
    // TraceLog(LOG_INFO, "Successfully sent %zu bytes", total_sent);
    return (ssize_t)total_sent;
}

// static int send_packet_to_client(int fd, uint8_t type, const void* data, uint32_t length)
// {
//     PacketHeader header;
//     header.type = type;
//     header.length = htonl(length);
//
//     if (send_all_to_client(fd, (const char*)&header, sizeof(header)) != sizeof(header)) {
//         return -1;
//     }
//     if (length > 0 && data != NULL) {
//         if (send_all_to_client(fd, (const char*)data, length) != (ssize_t)length) {
//             return -1;
//         }
//     }
//     return 0;
// }

static void send_abort_to_sender(int fd, const char* file_id, const char* reason)
{
    if (fd < 0 || !file_id || !reason)
        return;

    char* msg = malloc(BUFFER_SIZE);
    if (!msg) return;

    snprintf(msg, BUFFER_SIZE, "FILE_ABORT|SYSTEM|%s|%s", file_id, reason);
    send_all_to_client(fd, msg, strlen(msg));
    send_all_to_client(fd, "\n", 1);
    free(msg);
}

static void broadcast_file_abort(const char* file_id, const char* reason, int skip_fd)
{
    if (!file_id || !reason)
        return;

    char* msg = malloc(BUFFER_SIZE);
    if (!msg) return;

    snprintf(msg, BUFFER_SIZE, "FILE_ABORT|SYSTEM|%s|%s", file_id, reason);
    server_broadcast_msg(msg, skip_fd);
    free(msg);
}

static FileTransferSession* file_session_find(const char* file_id)
{
    if (!file_id)
        return NULL;

    for (int i = 0; i < MAX_FILE_SESSIONS; ++i) {
        if (file_sessions[i].active && strcmp(file_sessions[i].file_id, file_id) == 0) {
            return &file_sessions[i];
        }
    }
    return NULL;
}

static FileTransferSession* file_session_create(const char* file_id, int sender_fd, size_t total_bytes, size_t chunk_size)
{
    if (!file_id)
        return NULL;

    FileTransferSession* existing = file_session_find(file_id);
    if (existing) {
        existing->sender_fd = sender_fd;
        existing->total_bytes = total_bytes;
        existing->chunk_size = chunk_size;
        existing->forwarded_bytes = 0;
        return existing;
    }

    for (int i = 0; i < MAX_FILE_SESSIONS; ++i) {
        if (!file_sessions[i].active) {
            memset(&file_sessions[i], 0, sizeof(file_sessions[i]));
            file_sessions[i].active = true;
            strncpy(file_sessions[i].file_id, file_id, sizeof(file_sessions[i].file_id) - 1);
            file_sessions[i].sender_fd = sender_fd;
            file_sessions[i].total_bytes = total_bytes;
            file_sessions[i].chunk_size = chunk_size;
            file_sessions[i].forwarded_bytes = 0;
            return &file_sessions[i];
        }
    }

    return NULL;
}

static void file_session_remove(FileTransferSession* session)
{
    if (!session)
        return;
    memset(session, 0, sizeof(*session));
}

static void cleanup_sessions_for_fd(int fd)
{
    if (fd < 0)
        return;

    for (int i = 0; i < MAX_FILE_SESSIONS; ++i) {
        if (file_sessions[i].active && file_sessions[i].sender_fd == fd) {
            broadcast_file_abort(file_sessions[i].file_id, "Sender disconnected", fd);
            file_session_remove(&file_sessions[i]);
        }
    }
}

static bool process_file_message(int client_index, uint8_t type, const char* data, size_t len)
{
    if (type == PACKET_TYPE_FILE_START) {
        // Payload: sender|file_id|filename|total_bytes|chunk_size
        char* buffer = malloc(BUFFER_SIZE);
        if (!buffer) return false;

        if (len >= BUFFER_SIZE) len = BUFFER_SIZE - 1;
        memcpy(buffer, data, len);
        buffer[len] = '\0';
        
        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        const char* filename = strtok_r(NULL, "|", &save_ptr);
        const char* total_bytes_str = strtok_r(NULL, "|", &save_ptr);
        const char* chunk_size_str = strtok_r(NULL, "|", &save_ptr);
        
        if (!sender || !file_id || !filename || !total_bytes_str || !chunk_size_str)
            return false;

        (void)sender;

        unsigned long long total_bytes = strtoull(total_bytes_str, NULL, 10);
        unsigned long chunk_size = strtoul(chunk_size_str, NULL, 10);
        
        if (total_bytes == 0 || total_bytes > FILE_TRANSFER_MAX_SIZE || chunk_size == 0 || chunk_size > FILE_CHUNK_SIZE) {
            send_abort_to_sender(clients[client_index].sock_fd, file_id, "Invalid file metadata");
            return false;
        }

        FileTransferSession* session = file_session_create(file_id, clients[client_index].sock_fd, (size_t)total_bytes, (size_t)chunk_size);
        if (!session) {
            send_abort_to_sender(clients[client_index].sock_fd, file_id, "Server busy");
            return false;
        }
        session->forwarded_bytes = 0;
        free(buffer);
        return true;
    }
    else if (type == PACKET_TYPE_FILE_CHUNK) {
        // Payload: [FileID][Data]
        if (len <= FILE_ID_LEN) return false;
        
        char file_id[FILE_ID_LEN + 1];
        memcpy(file_id, data, FILE_ID_LEN);
        file_id[FILE_ID_LEN] = '\0';
        
        FileTransferSession* session = file_session_find(file_id);
        if (!session || session->sender_fd != clients[client_index].sock_fd) {
            send_abort_to_sender(clients[client_index].sock_fd, file_id, "Unknown file session");
            return false;
        }
        
        size_t chunk_len = len - FILE_ID_LEN;
        if (chunk_len > session->chunk_size + 1024) { // Tolerance
             broadcast_file_abort(file_id, "Chunk too large", -1);
             file_session_remove(session);
             return false;
        }
        
        session->forwarded_bytes += chunk_len;
        return true;
    }
    else if (type == PACKET_TYPE_FILE_END) {
        // Payload: sender|file_id
        char* buffer = malloc(BUFFER_SIZE);
        if (!buffer) return false;

        if (len >= BUFFER_SIZE) len = BUFFER_SIZE - 1;
        memcpy(buffer, data, len);
        buffer[len] = '\0';
        
        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        
        if (!sender || !file_id) {
            free(buffer);
            return false;
        }
        
        (void)sender;

        FileTransferSession* session = file_session_find(file_id);
        if (!session) {
            free(buffer);
            return false;
        }

        size_t tolerance = 2048;
        if (session->forwarded_bytes + tolerance < session->total_bytes) {
            broadcast_file_abort(file_id, "Transfer incomplete", -1);
            file_session_remove(session);
            free(buffer);
            return false;
        }

        file_session_remove(session);
        free(buffer);
        return true;
    }
    else if (type == PACKET_TYPE_FILE_ABORT) {
        // Payload: sender|file_id|reason
        char* buffer = malloc(BUFFER_SIZE);
        if (!buffer) return true;

        if (len >= BUFFER_SIZE) len = BUFFER_SIZE - 1;
        memcpy(buffer, data, len);
        buffer[len] = '\0';
        
        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        
        if (!sender || !file_id)
            return true;

        FileTransferSession* session = file_session_find(file_id);
        if (session)
            file_session_remove(session);
        free(buffer);
        return true;
    }

    return false;
}

void server_broadcast_msg(const char* msg, int sender_fd)
{
    size_t len = strlen(msg);
    
    // Construct packet header
    PacketHeader header;
    header.type = PACKET_TYPE_TEXT;
    header.length = htonl((uint32_t)len);

    // Track failed clients to remove after iteration
    int failed_clients[MAX_CLIENTS];
    int failed_count = 0;
    
    for (int i = 0; i < client_count; ++i) {
        int fd = clients[i].sock_fd;
        if (fd == sender_fd)
            continue;

        // Send Header
        ssize_t sent_header = send_all_to_client(fd, (const char*)&header, sizeof(header));
        if (sent_header != sizeof(header)) {
             TraceLog(LOG_WARNING, "Failed to send header to %s", clients[i].ip_addr);
             failed_clients[failed_count++] = i;
             continue;
        }

        // Send Body
        if (len > 0) {
            ssize_t sent_body = send_all_to_client(fd, msg, len);
            if (sent_body != (ssize_t)len) {
                TraceLog(LOG_WARNING, "Failed to send body to %s", clients[i].ip_addr);
                failed_clients[failed_count++] = i;
            }
        }
    }
    
    // Remove failed clients after iteration to avoid corruption
    for (int i = failed_count - 1; i >= 0; i--) {
        int idx = failed_clients[i];
        closesocket(clients[idx].sock_fd);
        
        // Shift remaining clients
        for (int j = idx + 1; j < client_count; j++) {
            clients[j - 1] = clients[j];
        }
        client_count--;
    }
}
