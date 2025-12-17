#include "unity.h"
#include "fff.h"
#include "client_logic.h"
#include "client_network.h"
#include "message.h"
#include "file_transfer_state.h"
#include "file_transfer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

DEFINE_FFF_GLOBALS;

void setUp(void) {}
void tearDown(void) {}

// Mocks
FAKE_VOID_FUNC(disconnect_from_server, ClientConnection*);
FAKE_VOID_FUNC(abort_all_transfers);
FAKE_VOID_FUNC(finalize_incoming_transfer, IncomingTransfer*, bool, const char*);
FAKE_VOID_FUNC(ensure_receive_directory);
FAKE_VOID_FUNC(sanitize_filename, char*);
FAKE_VOID_FUNC(add_message, MessageQueue*, const char*, const char*);

// We need a custom fake for recv_msg to generate data
int custom_recv_msg(ClientConnection* conn, char* buffer, int size);
FAKE_VALUE_FUNC(int, recv_msg, ClientConnection*, char*, int);

// We need custom fake for get_incoming_transfer/get_free_incoming to return a usable slot
IncomingTransfer* custom_get_incoming_transfer(const char* file_id);
IncomingTransfer* custom_get_free_incoming(void);
FAKE_VALUE_FUNC(IncomingTransfer*, get_incoming_transfer, const char*);
FAKE_VALUE_FUNC(IncomingTransfer*, get_free_incoming);

// Globals expected by client_logic.c
char incoming_stream[INCOMING_STREAM_CAPACITY];
size_t incoming_stream_len;

static IncomingTransfer g_benchmark_transfer;
static bool g_transfer_slot_used = false;

// Benchmark params
#define BENCHMARK_FILE_SIZE (50 * 1024 * 1024) // 50 MB
#define CHUNK_SIZE 4096
#define FILE_ID "bench_file"

static size_t g_bytes_produced = 0;
static bool g_header_sent = false;

int custom_recv_msg(ClientConnection* conn, char* buffer, int max_size) {
    (void)conn;
    
    if (g_bytes_produced >= BENCHMARK_FILE_SIZE) {
        return 0; // Done
    }
    
    // We construct packets into the buffer
    // But recv_msg just returns a byte stream. 
    // We need to respect max_size.
    
    int bytes_written = 0;
    
    if (!g_header_sent) {
        // Send FILE_START packet
        // Format: FILE_START | Sender | FileID | Filename | TotalBytes | ChunkSize
        char payload[1024];
        snprintf(payload, sizeof(payload), "Benchmark|%s|bench.bin|%d|%d", FILE_ID, BENCHMARK_FILE_SIZE, CHUNK_SIZE);
        size_t payload_len = strlen(payload);
        
        uint32_t net_len = htonl(payload_len);
        
        if (max_size < sizeof(PacketHeader) + payload_len) return 0; // Wait for space
        
        PacketHeader hdr;
        hdr.type = PACKET_TYPE_FILE_START;
        hdr.length = net_len;
        
        memcpy(buffer + bytes_written, &hdr, sizeof(hdr));
        bytes_written += sizeof(hdr);
        memcpy(buffer + bytes_written, payload, payload_len);
        bytes_written += payload_len;
        
        g_header_sent = true;
        return bytes_written; // Return one packet at a time for simplicity
    }
    
    // Send CHUNK packets
    // Header + FileID + Data
    size_t data_remaining = BENCHMARK_FILE_SIZE - g_bytes_produced;
    size_t data_to_send = (data_remaining > CHUNK_SIZE) ? CHUNK_SIZE : data_remaining;
    size_t file_id_len = strlen(FILE_ID);
    size_t payload_len = file_id_len + data_to_send;
    
    size_t packet_size = sizeof(PacketHeader) + payload_len;
    
    if (max_size < (int)packet_size) {
        return 0; // Not enough space
    }
    
    PacketHeader hdr;
    hdr.type = PACKET_TYPE_FILE_CHUNK;
    hdr.length = htonl(payload_len);
    
    memcpy(buffer + bytes_written, &hdr, sizeof(hdr));
    bytes_written += sizeof(hdr);
    
    memcpy(buffer + bytes_written, FILE_ID, file_id_len);
    bytes_written += file_id_len;
    
    // Fake data (random garbage/zeros)
    memset(buffer + bytes_written, 'A', data_to_send);
    bytes_written += data_to_send;
    
    g_bytes_produced += data_to_send;
    
    return bytes_written;
}

IncomingTransfer* custom_get_incoming_transfer(const char* file_id) {
    if (strcmp(file_id, FILE_ID) == 0 && g_transfer_slot_used) {
        return &g_benchmark_transfer;
    }
    return NULL;
}

IncomingTransfer* custom_get_free_incoming(void) {
    if (!g_transfer_slot_used) {
        g_transfer_slot_used = true;
        return &g_benchmark_transfer;
    }
    return NULL;
}

double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    printf("Starting Benchmark: %d MB transfer...\n", BENCHMARK_FILE_SIZE / (1024*1024));
    
    // Setup mocks
    recv_msg_fake.custom_fake = custom_recv_msg;
    get_incoming_transfer_fake.custom_fake = custom_get_incoming_transfer;
    get_free_incoming_fake.custom_fake = custom_get_free_incoming;
    
    // Setup transfer slot acting as /dev/null sink
    memset(&g_benchmark_transfer, 0, sizeof(g_benchmark_transfer));
    g_benchmark_transfer.fp = fopen("/dev/null", "wb");
    if (!g_benchmark_transfer.fp) {
        perror("Failed to open /dev/null");
        // Try NUL for windows? Assuming Linux based on previous context.
        return 1;
    }
    
    ClientConnection* conn = calloc(1, sizeof(ClientConnection));
    conn->connected = true;
    MessageQueue* mq = calloc(1, sizeof(MessageQueue));
    incoming_stream_len = 0;
    
    double start_time = get_time_sec();
    
    while (g_bytes_produced < BENCHMARK_FILE_SIZE || incoming_stream_len > 0) {
        // Run the client logic loop
        // It consumes from recv_msg (our custom generator)
        // And writes to g_benchmark_transfer.fp (/dev/null)
        update_client_state(conn, mq);
        
        // If backpressure is active, update_client_state returns. 
        // We just call it again. Our custom_recv_msg won't be called if buffer full.
        // It's a perfect simulation.
    }
    
    double end_time = get_time_sec();
    double duration = end_time - start_time;
    double throughput_mb = (BENCHMARK_FILE_SIZE / (1024.0 * 1024.0)) / duration;
    
    printf("\nBenchmark Complete!\n");
    printf("Transferred: %d MB\n", BENCHMARK_FILE_SIZE / (1024*1024));
    printf("Time: %.4f seconds\n", duration);
    printf("Throughput: %.2f MB/s\n", throughput_mb);
    
    if (g_benchmark_transfer.fp) fclose(g_benchmark_transfer.fp);
    free(conn);
    free(mq);
    
    if (throughput_mb < 50.0) {
        printf("FAIL: Throughput too low (< 50 MB/s)\n");
        return 1;
    }
    
    printf("SUCCESS: Throughput acceptable\n");
    return 0;
}
