#include "server.h"
#include "file_transfer.h"
#include "platform.h"
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
static int send_packet_to_client(int fd, uint8_t type, const void* data, uint32_t length);
static void broadcast_packet(uint8_t type, const void* data, uint32_t length, int skip_fd);

void server_set_msg_cb(server_msg_cb cb)
{
    g_on_msg_cb = cb;
}

int get_client_count(void)
{
    return client_count;
}
Client* get_clients(void)
{
    return clients;
}

int server_accept_client(void)
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
    int sendbuf = 8 * 1024 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, (char*)&sendbuf, sizeof(sendbuf));
    int recvbuf = 8 * 1024 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, (char*)&recvbuf, sizeof(recvbuf));
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

    char* recv_buffer = malloc(CLIENT_STREAM_BUFFER);
    if (!recv_buffer) {
        TraceLog(LOG_ERROR, "Could not allocate a receive buffer for a new client");
        closesocket(client_fd);
        return -1;
    }

    clients[client_count].sock_fd = client_fd;
    snprintf(clients[client_count].username, sizeof(clients[client_count].username), "Unknown");
    snprintf(clients[client_count].ip_addr, sizeof(clients[client_count].ip_addr), "%s", inet_ntoa(client_addr.sin_addr));
    clients[client_count].recv_buffer = recv_buffer;
    clients[client_count].recv_capacity = CLIENT_STREAM_BUFFER;
    clients[client_count].recv_len = 0;
    clients[client_count].authenticated = false;

    // Initialize client state
    memset(&client_states[client_count], 0, sizeof(ClientState));
    client_states[client_count].state = STATE_HEADER;
    client_states[client_count].bytes_needed = sizeof(PacketHeader);

    client_count++;

    return client_fd;
}

bool is_server_running(void)
{
    return server_running;
}

void cleanup_server(void)
{
    while (client_count > 0)
        remove_client(client_count - 1);

    if (server_fd != -1) {
        TraceLog(LOG_INFO, "Cleaning the server");
        closesocket(server_fd);
        server_fd = -1;
    }

    TraceLog(LOG_INFO, "Finished clean up the server");
    server_running = false;

    cleanup_network();
}

bool init_server(void)
{
    if (init_network() != 0) {
        TraceLog(LOG_ERROR, "Network initialization failed");
        return false;
    }
    memset(file_sessions, 0, sizeof(file_sessions));
    client_count = 0;
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("socket");
        TraceLog(LOG_ERROR, "Socket creation failed.");
        cleanup_network();
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
        server_fd = -1;
        cleanup_network();
        return false;
    }
    if (listen(server_fd, 10) == -1) {
        perror("listen");
        TraceLog(LOG_ERROR, "Listen failed: %s", strerror(errno));
        closesocket(server_fd);
        server_fd = -1;
        cleanup_network();
        return false;
    }

#ifdef _WIN32
    u_long iMode = 1;
    ioctlsocket(server_fd, FIONBIO, &iMode);
#else
    // Setup non-block mode
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
#endif

    server_running = true;
    TraceLog(LOG_INFO, "Server is running on port %d, ready to accept connections", PORT);
    return true;
}

static void handle_client_packet(int client_index, uint8_t type, const char* data, size_t len);

void server_recv_msgs(void)
{
    for (int i = 0; i < client_count; ++i) {
        int fd = clients[i].sock_fd;

        // Loop to drain socket buffer
        while (true) {
            char buffer[65536]; // 64KB stack buffer
            ssize_t bytes_recv = recv(fd, buffer, sizeof(buffer), 0);

            if (bytes_recv > 0) {
                size_t available = clients[i].recv_capacity - clients[i].recv_len;
                if ((size_t)bytes_recv > available) {
                    TraceLog(LOG_ERROR, "Client %s overflowed recv buffer", clients[i].ip_addr);
                    remove_client(i);
                    i--;
                    break; // Break inner loop, outer loop continues
                }

                memcpy(clients[i].recv_buffer + clients[i].recv_len, buffer, (size_t)bytes_recv);
                clients[i].recv_len += (size_t)bytes_recv;

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

                        if (!protocol_packet_is_valid(client_states[i].header.type,
                                client_states[i].header.length)) {
                            TraceLog(LOG_WARNING, "Disconnecting %s for invalid packet type=%u length=%u",
                                clients[i].ip_addr, client_states[i].header.type,
                                client_states[i].header.length);
                            remove_client(i);
                            i--;
                            break;
                        }

                        client_states[i].state = STATE_BODY;
                        client_states[i].bytes_needed = client_states[i].header.length;
                    } else if (client_states[i].state == STATE_BODY) {
                        handle_client_packet(i, client_states[i].header.type, clients[i].recv_buffer + processed, client_states[i].header.length);
                        processed += client_states[i].header.length;
                        client_states[i].state = STATE_HEADER;
                        client_states[i].bytes_needed = sizeof(PacketHeader);
                    }
                }

                if (i < 0 || i >= client_count || clients[i].sock_fd != fd)
                    break;

                if (processed > 0) {
                    size_t remaining = clients[i].recv_len - processed;
                    memmove(clients[i].recv_buffer, clients[i].recv_buffer + processed, remaining);
                    clients[i].recv_len = remaining;
                }
            } else if (bytes_recv == 0) {
                TraceLog(LOG_INFO, "Client disconnected (%s)", clients[i].ip_addr);
                char leave_msg[512];
                snprintf(leave_msg, sizeof(leave_msg), "SYSTEM: %s left the workspace",
                    clients[i].authenticated ? clients[i].username : "A peer");
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
        if (!data || len == 0 || len > PROTOCOL_TEXT_MAX_LEN || !protocol_text_is_valid(data, len)) {
            TraceLog(LOG_WARNING, "Rejected invalid text packet from %s", clients[client_index].ip_addr);
            return;
        }

        char msg[PROTOCOL_TEXT_MAX_LEN + 1];
        memcpy(msg, data, len);
        msg[len] = '\0';

        const char* prefix = "USERNAME:";
        size_t prefix_len = strlen(prefix);
        if (!clients[client_index].authenticated && strncmp(msg, prefix, prefix_len) == 0) {
            const char* name = msg + prefix_len;
            if (protocol_username_is_valid(name)) {
                snprintf(clients[client_index].username, sizeof(clients[client_index].username), "%s", name);
                clients[client_index].authenticated = true;
                TraceLog(LOG_INFO, "Client %s set username to '%s'", clients[client_index].ip_addr, clients[client_index].username);
            } else {
                TraceLog(LOG_WARNING, "Client %s supplied an invalid username", clients[client_index].ip_addr);
            }
            return;
        }

        if (!clients[client_index].authenticated || len > PROTOCOL_CHAT_CONTENT_MAX_LEN)
            return;

        // Sender identity is added by the server, so a client cannot impersonate
        // another participant by formatting its own "name: message" prefix.
        char trusted_message[PROTOCOL_TEXT_MAX_LEN + 1];
        int trusted_len = snprintf(trusted_message, sizeof(trusted_message), "%s: %s",
            clients[client_index].username, msg);
        if (trusted_len < 0 || (size_t)trusted_len >= sizeof(trusted_message))
            return;
        server_broadcast_msg(trusted_message, clients[client_index].sock_fd);

        if (g_on_msg_cb)
            g_on_msg_cb(msg, clients[client_index].username);
    } else if (type >= PACKET_TYPE_FILE_START && type <= PACKET_TYPE_FILE_ACCEPT) {
        if (process_file_message(client_index, type, data, len)) {
            broadcast_packet(type, data, (uint32_t)len, clients[client_index].sock_fd);
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
    free(clients[index].recv_buffer);
    clients[index].recv_buffer = NULL;
    clients[index].recv_capacity = 0;
    clients[index].sock_fd = -1;
    clients[index].recv_len = 0;

    for (int j = index + 1; j < client_count; ++j) {
        clients[j - 1] = clients[j];
        client_states[j - 1] = client_states[j];
    }
    client_count--;
    memset(&clients[client_count], 0, sizeof(clients[client_count]));
    memset(&client_states[client_count], 0, sizeof(client_states[client_count]));
}

// Helper: Send all data, handling partial sends and flow control
static ssize_t send_all_to_client(int socket_fd, const char* data, size_t len)
{
    size_t total_sent = 0;
    int retry_count = 0;
    const int MAX_RETRIES = 20;
    const int POLL_TIMEOUT_MS = 100; // Wait up to 100ms for socket to be writable

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
                // Use poll/select to wait for socket to be writable (no busy-wait)
                int poll_result = wait_socket_writable(socket_fd, POLL_TIMEOUT_MS);
                if (poll_result > 0) {
                    continue; // Socket is writable, retry immediately
                } else if (poll_result == 0) {
                    // Timeout
                    retry_count++;
                    if (retry_count > MAX_RETRIES) {
                        TraceLog(LOG_ERROR, "Max retries reached after %d attempts, sent %zu/%zu bytes (%.1f%%)",
                            retry_count, total_sent, len, (float)total_sent / (float)len * 100);
                        return -1;
                    }
                    continue;
                } else {
                    // Poll error
                    TraceLog(LOG_ERROR, "Poll error while waiting for socket");
                    return -1;
                }
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
            retry_count++;
            if (retry_count > MAX_RETRIES) {
                TraceLog(LOG_WARNING, "Send returned 0 after %d retries, sent %zu/%zu bytes", retry_count, total_sent, len);
                return total_sent > 0 ? (ssize_t)total_sent : -1;
            }
            // Use poll to wait instead of usleep
            wait_socket_writable(socket_fd, 10);
            continue;
        }

        total_sent += (size_t)bytes_sent;
        retry_count = 0;
    }

    return (ssize_t)total_sent;
}

static int send_packet_to_client(int fd, uint8_t type, const void* data, uint32_t length)
{
    if (fd < 0 || !protocol_packet_is_valid(type, length) || (length > 0 && !data))
        return -1;

    PacketHeader header = { .type = type, .length = htonl(length) };
    if (send_all_to_client(fd, (const char*)&header, sizeof(header)) != (ssize_t)sizeof(header))
        return -1;
    if (send_all_to_client(fd, data, length) != (ssize_t)length)
        return -1;
    return 0;
}

static void broadcast_packet(uint8_t type, const void* data, uint32_t length, int skip_fd)
{
    for (int i = 0; i < client_count; ++i) {
        if (clients[i].sock_fd != skip_fd)
            (void)send_packet_to_client(clients[i].sock_fd, type, data, length);
    }
}

static void send_abort_to_sender(int fd, const char* file_id, const char* reason)
{
    if (fd < 0 || !file_id || !reason)
        return;

    char msg[PROTOCOL_METADATA_MAX_LEN + 1];
    int len = snprintf(msg, sizeof(msg), "SYSTEM|%s|%s", file_id, reason);
    if (len > 0 && (size_t)len < sizeof(msg))
        (void)send_packet_to_client(fd, PACKET_TYPE_FILE_ABORT, msg, (uint32_t)len);
}

static void broadcast_file_abort(const char* file_id, const char* reason, int skip_fd)
{
    if (!file_id || !reason)
        return;

    char msg[PROTOCOL_METADATA_MAX_LEN + 1];
    int len = snprintf(msg, sizeof(msg), "SYSTEM|%s|%s", file_id, reason);
    if (len > 0 && (size_t)len < sizeof(msg))
        broadcast_packet(PACKET_TYPE_FILE_ABORT, msg, (uint32_t)len, skip_fd);
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
    if (existing)
        return NULL;

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
    if (!clients[client_index].authenticated || !data || len == 0 || len > PROTOCOL_MAX_PAYLOAD)
        return false;

    if (type == PACKET_TYPE_FILE_START) {
        if (len > PROTOCOL_METADATA_MAX_LEN || !protocol_text_is_valid(data, len))
            return false;
        char buffer[PROTOCOL_METADATA_MAX_LEN + 1];
        memcpy(buffer, data, len);
        buffer[len] = '\0';

        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        const char* filename = strtok_r(NULL, "|", &save_ptr);
        const char* total_bytes_str = strtok_r(NULL, "|", &save_ptr);
        const char* chunk_size_str = strtok_r(NULL, "|", &save_ptr);

        if (!sender || !file_id || !filename || !total_bytes_str || !chunk_size_str || strcmp(sender, clients[client_index].username) != 0 || !protocol_file_id_is_valid(file_id) || filename[0] == '\0' || strlen(filename) >= FILE_NAME_MAX_LEN)
            return false;

        char* end = NULL;
        errno = 0;
        unsigned long long total_bytes = strtoull(total_bytes_str, &end, 10);
        if (errno != 0 || end == total_bytes_str || *end != '\0')
            return false;
        end = NULL;
        errno = 0;
        unsigned long chunk_size = strtoul(chunk_size_str, &end, 10);
        if (errno != 0 || end == chunk_size_str || *end != '\0')
            return false;

        if (total_bytes > FILE_TRANSFER_MAX_SIZE || chunk_size == 0 || chunk_size > FILE_CHUNK_SIZE) {
            send_abort_to_sender(clients[client_index].sock_fd, file_id, "Invalid file metadata");
            return false;
        }

        FileTransferSession* session = file_session_create(file_id, clients[client_index].sock_fd, (size_t)total_bytes, (size_t)chunk_size);
        if (!session) {
            send_abort_to_sender(clients[client_index].sock_fd, file_id, "Server busy");
            return false;
        }
        session->forwarded_bytes = 0;
        return true;
    } else if (type == PACKET_TYPE_FILE_CHUNK) {
        // Payload: [FileID][Data]
        if (len <= FILE_ID_LEN)
            return false;

        char file_id[FILE_ID_LEN + 1];
        memcpy(file_id, data, FILE_ID_LEN);
        file_id[FILE_ID_LEN] = '\0';

        FileTransferSession* session = file_session_find(file_id);
        if (!session || session->sender_fd != clients[client_index].sock_fd) {
            send_abort_to_sender(clients[client_index].sock_fd, file_id, "Unknown file session");
            return false;
        }

        size_t chunk_len = len - FILE_ID_LEN;
        if (chunk_len > session->chunk_size || session->forwarded_bytes > session->total_bytes || chunk_len > session->total_bytes - session->forwarded_bytes) {
            broadcast_file_abort(file_id, "Invalid chunk size", -1);
            file_session_remove(session);
            return false;
        }

        session->forwarded_bytes += chunk_len;
        return true;
    } else if (type == PACKET_TYPE_FILE_END) {
        if (len > PROTOCOL_METADATA_MAX_LEN || !protocol_text_is_valid(data, len))
            return false;
        char buffer[PROTOCOL_METADATA_MAX_LEN + 1];
        memcpy(buffer, data, len);
        buffer[len] = '\0';

        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);

        if (!sender || !file_id || strcmp(sender, clients[client_index].username) != 0 || !protocol_file_id_is_valid(file_id)) {
            return false;
        }

        FileTransferSession* session = file_session_find(file_id);
        if (!session || session->sender_fd != clients[client_index].sock_fd) {
            return false;
        }

        if (session->forwarded_bytes != session->total_bytes) {
            broadcast_file_abort(file_id, "Transfer incomplete", -1);
            file_session_remove(session);
            return false;
        }

        file_session_remove(session);
        return true;
    } else if (type == PACKET_TYPE_FILE_ABORT) {
        if (len > PROTOCOL_METADATA_MAX_LEN || !protocol_text_is_valid(data, len))
            return false;
        char buffer[PROTOCOL_METADATA_MAX_LEN + 1];
        memcpy(buffer, data, len);
        buffer[len] = '\0';

        char* save_ptr = NULL;
        const char* sender = strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);

        if (!sender || !file_id || !protocol_file_id_is_valid(file_id))
            return false;

        FileTransferSession* session = file_session_find(file_id);
        if (!session)
            return false;

        if (session->sender_fd == clients[client_index].sock_fd) {
            broadcast_packet(PACKET_TYPE_FILE_ABORT, data, (uint32_t)len,
                clients[client_index].sock_fd);
            file_session_remove(session);
        } else {
            (void)send_packet_to_client(session->sender_fd, PACKET_TYPE_FILE_ABORT,
                data, (uint32_t)len);
        }
        return false;
    } else if (type == PACKET_TYPE_FILE_ACCEPT) {
        if (len > PROTOCOL_METADATA_MAX_LEN || !protocol_text_is_valid(data, len))
            return false;
        char buffer[PROTOCOL_METADATA_MAX_LEN + 1];
        memcpy(buffer, data, len);
        buffer[len] = '\0';
        char* save_ptr = NULL;
        (void)strtok_r(buffer, "|", &save_ptr);
        const char* file_id = strtok_r(NULL, "|", &save_ptr);
        FileTransferSession* session = file_session_find(file_id);
        if (!session || session->sender_fd == clients[client_index].sock_fd)
            return false;
        (void)send_packet_to_client(session->sender_fd, PACKET_TYPE_FILE_ACCEPT,
            data, (uint32_t)len);
        return false;
    }

    return false;
}

void server_broadcast_msg(const char* msg, int sender_fd)
{
    if (!msg)
        return;
    size_t len = strnlen(msg, PROTOCOL_TEXT_MAX_LEN + 1u);
    if (!protocol_text_is_valid(msg, len))
        return;

    // Track failed clients to remove after iteration
    int failed_clients[MAX_CLIENTS];
    int failed_count = 0;

    for (int i = 0; i < client_count; ++i) {
        int fd = clients[i].sock_fd;
        if (fd == sender_fd)
            continue;

        if (send_packet_to_client(fd, PACKET_TYPE_TEXT, msg, (uint32_t)len) != 0) {
            TraceLog(LOG_WARNING, "Failed to send message to %s", clients[i].ip_addr);
            failed_clients[failed_count++] = i;
        }
    }

    // Remove failed clients after iteration to avoid corruption
    for (int i = failed_count - 1; i >= 0; i--) {
        remove_client(failed_clients[i]);
    }
}
