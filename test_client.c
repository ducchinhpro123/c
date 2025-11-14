// Simple automated test client for file transfer testing
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include "message.h"
#include "file_transfer.h"

#define SERVER_PORT 8898
#define FILE_RECV_BUFFER_SIZE (262144)  // 256KB

int sock_fd = -1;

// Simple send_all implementation
static ssize_t send_all(int fd, const char* data, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = send(fd, data + total_sent, len - total_sent, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        total_sent += sent;
    }
    return total_sent;
}

// Connect to server
int connect_to_server(const char* ip, int port) {
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);
    
    if (connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock_fd);
        return -1;
    }
    
    // Set non-blocking
    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);
    
    printf("Connected to server %s:%d\n", ip, port);
    return sock_fd;
}

// Send a file
int send_file(const char* filepath) {
    // Check if file exists
    struct stat st;
    if (stat(filepath, &st) != 0) {
        fprintf(stderr, "File not found: %s\n", filepath);
        return -1;
    }
    
    size_t file_size = st.st_size;
    
    if (file_size > MAX_FILE_SIZE) {
        fprintf(stderr, "File too large: %zu bytes (max: %d)\n", file_size, MAX_FILE_SIZE);
        return -1;
    }
    
    // Extract filename from path
    const char* filename = strrchr(filepath, '/');
    filename = filename ? filename + 1 : filepath;
    
    // Calculate chunks
    int total_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    if (total_chunks == 0) total_chunks = 1;  // Handle empty files
    
    printf("Sending file: %s (%zu bytes, %d chunks)\n", filename, file_size, total_chunks);
    
    // Open file
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        fprintf(stderr, "Cannot open file: %s\n", filepath);
        return -1;
    }
    
    // Send file chunks
    unsigned char* chunk_buffer = malloc(CHUNK_SIZE);
    if (!chunk_buffer) {
        fclose(file);
        return -1;
    }
    
    for (int i = 0; i < total_chunks; i++) {
        size_t chunk_size = CHUNK_SIZE;
        size_t bytes_read = fread(chunk_buffer, 1, chunk_size, file);
        
        if (bytes_read == 0 && i < total_chunks - 1) {
            fprintf(stderr, "Failed to read chunk %d\n", i);
            free(chunk_buffer);
            fclose(file);
            return -1;
        }
        
        // Create packet
        char* packet = create_file_packet(filename, file_size, i, total_chunks, 
                                          chunk_buffer, bytes_read);
        if (!packet) {
            fprintf(stderr, "Failed to create packet for chunk %d\n", i);
            free(chunk_buffer);
            fclose(file);
            return -1;
        }
        
        // Send packet
        size_t packet_len = strlen(packet);
        ssize_t sent = send_all(sock_fd, packet, packet_len);
        free(packet);
        
        if (sent < 0) {
            fprintf(stderr, "Failed to send chunk %d\n", i);
            free(chunk_buffer);
            fclose(file);
            return -1;
        }
        
        printf("  Sent chunk %d/%d (%zu bytes)\n", i + 1, total_chunks, bytes_read);
        
        // Small delay to avoid overwhelming server
        usleep(10000);  // 10ms
    }
    
    free(chunk_buffer);
    fclose(file);
    
    printf("File sent successfully: %s\n", filename);
    return 0;
}

// Receive messages/files from server
int receive_data() {
    static char recv_buffer[FILE_RECV_BUFFER_SIZE] = {0};
    static size_t buffer_len = 0;
    char temp_buffer[MSG_BUFFER];
    
    ssize_t bytes_recv = recv(sock_fd, temp_buffer, sizeof(temp_buffer) - 1, 0);
    
    if (bytes_recv <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  // No data available
        }
        return -1;  // Error or disconnection
    }
    
    temp_buffer[bytes_recv] = '\0';
    
    // Check if it's a file packet
    if (strncmp(temp_buffer, "FILE:", 5) == 0) {
        // Accumulate in buffer
        if (buffer_len + bytes_recv < FILE_RECV_BUFFER_SIZE * 0.9) {
            memcpy(recv_buffer + buffer_len, temp_buffer, bytes_recv);
            buffer_len += bytes_recv;
        } else {
            fprintf(stderr, "Receive buffer overflow!\n");
            buffer_len = 0;
            return -1;
        }
        
        // Process complete packets
        char* current = recv_buffer;
        size_t remaining = buffer_len;
        
        while (remaining > 0) {
            char* newline = memchr(current, '\n', remaining);
            if (!newline) break;
            
            size_t packet_len = newline - current + 1;
            char packet[packet_len + 1];
            memcpy(packet, current, packet_len);
            packet[packet_len] = '\0';
            
            // Parse file packet
            FileTransfer ft = {0};
            unsigned char* chunk_data = NULL;
            size_t chunk_size = 0;
            
            if (parse_file_packet(packet, &ft, &chunk_data, &chunk_size)) {
                printf("Received file chunk: %s (chunk %d/%d)\n", 
                       ft.filename, ft.chunks_received, ft.total_chunks);
                
                // For testing, just validate, don't save
                free(chunk_data);
            }
            
            current += packet_len;
            remaining -= packet_len;
        }
        
        // Keep incomplete data
        if (remaining > 0 && current != recv_buffer) {
            memmove(recv_buffer, current, remaining);
        }
        buffer_len = remaining;
    } else {
        // Regular message
        printf("Received: %s\n", temp_buffer);
    }
    
    return bytes_recv;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <test_file_path> [server_ip]\n", argv[0]);
        fprintf(stderr, "Example: %s test_files/small_1kb.bin\n", argv[0]);
        return 1;
    }
    
    const char* file_path = argv[1];
    const char* server_ip = argc > 2 ? argv[2] : "127.0.0.1";
    
    // Connect to server
    if (connect_to_server(server_ip, SERVER_PORT) < 0) {
        return 1;
    }
    
    // Send initial message
    const char* hello = "TEST_CLIENT: Connected for automated testing\n";
    send_all(sock_fd, hello, strlen(hello));
    
    // Wait a bit for any server response
    usleep(100000);
    receive_data();
    
    // Send the file
    if (send_file(file_path) != 0) {
        close(sock_fd);
        return 1;
    }
    
    // Wait for broadcast back (if any)
    printf("Waiting for broadcast...\n");
    for (int i = 0; i < 50; i++) {  // Wait up to 5 seconds
        if (receive_data() > 0) {
            // Got some data
        }
        usleep(100000);  // 100ms
    }
    
    // Disconnect
    printf("Test complete, disconnecting\n");
    close(sock_fd);
    
    return 0;
}
