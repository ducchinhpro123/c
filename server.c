#include "platform.h"
#include "server.h"
#include "server.h"
#include "file_transfer.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
static int server_fd = -1;
static Client clients[MAX_CLIENTS];
static int client_count = 0;
static bool server_running = false;
static server_msg_cb g_on_msg_cb = NULL;


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
static void handle_client_message(int client_index, const char* msg);
static void remove_client(int index);
static bool process_file_message(int client_index, const char* msg);
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
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return -1;
        }
        TraceLog(LOG_ERROR, "Accept failed. %s", strerror(errno));
        return -1;
    }

    // Optimize socket for LAN speed
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int sendbuf = 2 * 1024 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));
    int recvbuf = 2 * 1024 * 1024;
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
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // Accept connection on any interface
    addr.sin_port = htons(PORT); // Convert to network byte order, listen on port PORT

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        TraceLog(LOG_ERROR, "Bind failed: %s", strerror(errno));
        // close(server_fd);
        closesocket(server_fd);
        return false;
    }
    if (listen(server_fd, 10) == -1) {
        perror("listen");
        TraceLog(LOG_ERROR, "Listen failed: %s", strerror(errno));
        closesocket(server_fd);
        return false;
    }

    int flags = fcntl(server_fd, F_GETFL, 0); // Current status of the socket
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK); // Modifty it

    server_running = true;
    TraceLog(LOG_INFO, "Server is running on port %d, ready to accept connections", PORT);
    return true;
}

void server_recv_msgs()
{
    for (int i = 0; i < client_count; ++i) {
        char buffer[BUFFER_SIZE];
        int fd = clients[i].sock_fd;
        int bytes_recv = recv(fd, buffer, sizeof(buffer), 0);

        if (bytes_recv > 0) {
            size_t available = sizeof(clients[i].recv_buffer) - clients[i].recv_len;
            if ((size_t)bytes_recv > available) {
                TraceLog(LOG_ERROR, "Client %s overflowed recv buffer", clients[i].ip_addr);
                remove_client(i);
                i--;
                continue;
            }

            memcpy(clients[i].recv_buffer + clients[i].recv_len, buffer, bytes_recv);
            clients[i].recv_len += bytes_recv;

            size_t processed = 0;
            while (processed < clients[i].recv_len) {
                void* newline_ptr = memchr(clients[i].recv_buffer + processed, '\n', clients[i].recv_len - processed);
                if (!newline_ptr)
                    break;

                size_t msg_len = (size_t)((char*)newline_ptr - (clients[i].recv_buffer + processed));
                if (msg_len >= BUFFER_SIZE) {
                    TraceLog(LOG_WARNING, "Message too large from %s, dropping", clients[i].ip_addr);
                    processed = clients[i].recv_len; // drop buffered data
                    break;
                }

                char message[BUFFER_SIZE];
                memcpy(message, clients[i].recv_buffer + processed, msg_len);
                message[msg_len] = '\0';
                handle_client_message(i, message);

                processed = (size_t)((char*)newline_ptr - clients[i].recv_buffer) + 1;
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
        } else {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                continue;
            TraceLog(LOG_WARNING, "Recv failed for %s: %s", clients[i].ip_addr, strerror(errno));
            remove_client(i);
            i--;
        }
    }
}

static void handle_client_message(int client_index, const char* msg)
{
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

    bool is_file_packet = strncmp(msg, "FILE_", 5) == 0;
    bool allow_broadcast = true;

    if (is_file_packet) {
        allow_broadcast = process_file_message(client_index, msg);
    }

    if (allow_broadcast) {
        server_broadcast_msg(msg, clients[client_index].sock_fd);
    }

    if (!is_file_packet && allow_broadcast && g_on_msg_cb) {
        g_on_msg_cb(msg, clients[client_index].username);
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
        ssize_t bytes_sent = send(socket_fd, data + total_sent, len - total_sent, MSG_NOSIGNAL);
        
        if (bytes_sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
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
            } else if (errno == EPIPE || errno == ECONNRESET) {
                // Connection closed
                TraceLog(LOG_WARNING, "Connection closed while sending after %zu/%zu bytes", total_sent, len);
                return -1;
            } else {
                // Other error
                TraceLog(LOG_ERROR, "Send error: %s (sent %zu/%zu bytes)", strerror(errno), total_sent, len);
                return -1;
            }
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
        if (total_sent > 0 && (total_sent % 32768) == 0 && len > 10000) {
            // Log progress for large transfers
            TraceLog(LOG_INFO, "Transfer progress: %zu/%zu bytes (%.1f%%)",
                     total_sent, len, (float)total_sent / len * 100);
            usleep(1000); // Very brief yield, just 1ms
        }
        
        retry_count = 0; // Reset retry counter on successful send
    }
    
    // TraceLog(LOG_INFO, "Successfully sent %zu bytes", total_sent);
    return (ssize_t)total_sent;
}

static void send_abort_to_sender(int fd, const char* file_id, const char* reason)
{
    if (fd < 0 || !file_id || !reason)
        return;

    char msg[BUFFER_SIZE];
    snprintf(msg, sizeof(msg), "FILE_ABORT|SYSTEM|%s|%s", file_id, reason);
    send_all_to_client(fd, msg, strlen(msg));
    send_all_to_client(fd, "\n", 1);
}

static void broadcast_file_abort(const char* file_id, const char* reason, int skip_fd)
{
    if (!file_id || !reason)
        return;

    char msg[BUFFER_SIZE];
    snprintf(msg, sizeof(msg), "FILE_ABORT|SYSTEM|%s|%s", file_id, reason);
    server_broadcast_msg(msg, skip_fd);
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

static bool process_file_message(int client_index, const char* msg)
{
    if (!msg)
        return false;

    char buffer[BUFFER_SIZE];
    strncpy(buffer, msg, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char* save_ptr = NULL;
    char* type = strtok_r(buffer, "|", &save_ptr);
    if (!type)
        return false;

    int sender_fd = clients[client_index].sock_fd;

    if (strcmp(type, "FILE_META") == 0) {
        const char* sender = strtok_r(NULL, "|", &save_ptr);
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
            send_abort_to_sender(sender_fd, file_id, "Invalid file metadata");
            return false;
        }

        FileTransferSession* session = file_session_create(file_id, sender_fd, (size_t)total_bytes, (size_t)chunk_size);
        if (!session) {
            send_abort_to_sender(sender_fd, file_id, "Server busy");
            return false;
        }
        session->forwarded_bytes = 0;
        return true;
    }

    if (strcmp(type, "FILE_CHUNK") == 0) {
        const char* sender = strtok_r(NULL, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        const char* chunk_idx = strtok_r(NULL, "|", &save_ptr);
        const char* payload = strtok_r(NULL, "|", &save_ptr);
        if (!sender || !file_id || !chunk_idx || !payload)
            return false;

        (void)sender;
        (void)chunk_idx;

        FileTransferSession* session = file_session_find(file_id);
        if (!session || session->sender_fd != sender_fd) {
            send_abort_to_sender(sender_fd, file_id, "Unknown file session");
            return false;
        }

        size_t payload_len = strlen(payload);
        if (payload_len > FILE_CHUNK_ENCODED_SIZE + 8) {
            broadcast_file_abort(file_id, "Chunk payload too large", -1);
            file_session_remove(session);
            return false;
        }

        size_t approx_bytes = (payload_len / 4) * 3;
        if (payload_len >= 1 && payload[payload_len - 1] == '=')
            approx_bytes--;
        if (payload_len >= 2 && payload[payload_len - 2] == '=')
            approx_bytes--;

        if (approx_bytes == 0 || approx_bytes > session->chunk_size + 1024) {
            broadcast_file_abort(file_id, "Chunk size mismatch", -1);
            file_session_remove(session);
            return false;
        }

        if (session->forwarded_bytes + approx_bytes > session->total_bytes + 2048) {
            broadcast_file_abort(file_id, "Transfer exceeded size", -1);
            file_session_remove(session);
            return false;
        }

        session->forwarded_bytes += approx_bytes;
        return true;
    }

    if (strcmp(type, "FILE_END") == 0) {
        const char* sender = strtok_r(NULL, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        if (!sender || !file_id)
            return false;

        (void)sender;

        FileTransferSession* session = file_session_find(file_id);
        if (!session)
            return false;

        size_t tolerance = 2048;
        if (session->forwarded_bytes + tolerance < session->total_bytes) {
            broadcast_file_abort(file_id, "Transfer incomplete", -1);
            file_session_remove(session);
            return false;
        }

        file_session_remove(session);
        return true;
    }

    if (strcmp(type, "FILE_ABORT") == 0) {
        const char* sender = strtok_r(NULL, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        if (!sender || !file_id)
            return true;

        FileTransferSession* session = file_session_find(file_id);
        if (session)
            file_session_remove(session);
        return true;
    }

    return true;
}

void server_broadcast_msg(const char* msg, int sender_fd)
{
    size_t len = strlen(msg);
    
    // Check if message already has a newline
    bool has_newline = (len > 0 && msg[len - 1] == '\n');
    
    // Track failed clients to remove after iteration
    int failed_clients[MAX_CLIENTS];
    int failed_count = 0;
    
    // Log large packet broadcasts
    // if (len > 10000) {
    //     TraceLog(LOG_INFO, "Broadcasting large packet: %zu bytes to %d clients", len, client_count - 1);
    // }
    
    for (int i = 0; i < client_count; ++i) {
        int fd = clients[i].sock_fd;
        if (fd == sender_fd)
            continue;

        ssize_t bytes_sent = send_all_to_client(fd, msg, len);
        
        // If no newline, send one
        if (bytes_sent == (ssize_t)len && !has_newline) {
            send_all_to_client(fd, "\n", 1);
        }
        
        if (bytes_sent < 0 || (size_t)bytes_sent < len) {
            TraceLog(LOG_WARNING, "send failed to %s (%s), sent %zd/%zu bytes",
                     clients[i].username, clients[i].ip_addr, 
                     bytes_sent < 0 ? 0 : bytes_sent, len);
            failed_clients[failed_count++] = i;
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
