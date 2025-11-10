#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <raylib.h>

#define MAX_FILENAME 256
#define CHUNK_SIZE 8192 // 8KB chunks for network efficiency
#define MAX_FILE_SIZE (50 * 1024 * 1024) // 50MB limit

typedef struct {
    char filename[MAX_FILENAME];
    size_t total_size;
    size_t bytes_received;
    int total_chunks;
    int chunks_received;
    unsigned char* buffer; // Accumulator for chunks
    bool complete;
} FileTransfer;

// Encoding/Decoding
char* base64_encode(const unsigned char* data, size_t input_length, size_t* output_length);
unsigned char* base64_decode(const char* data, size_t input_length, size_t* output_length);

// File operations
bool read_file_to_buffer(const char* filepath, unsigned char** buffer, size_t* size);
bool write_buffer_to_file(const char* filepath, const unsigned char* buffer, size_t size);

// Protocol functions
char* create_file_packet(const char* filename, size_t filesize,
    int chunk_index, int total_chunks,
    const unsigned char* chunk_data, size_t chunk_size);
bool parse_file_packet(const char* packet, FileTransfer* ft, unsigned char** chunk_data, size_t* chunk_size);

// Transfer management
FileTransfer* create_file_transfer(const char* filename, size_t total_size, int total_chunks);
bool add_chunk_to_transfer(FileTransfer* ft, int chunk_index, const unsigned char* data, size_t size);
void free_file_transfer(FileTransfer* ft);

#endif
