#ifndef FILE_H
#define FILE_H

#include <stddef.h>
#include <stdio.h>

#define CHUNK_SIZE 4096

typedef enum {
    MSG_TYPE_TEXT,
    MSG_TYPE_FILE_METADATA,
    MSG_TYPE_FILE_CHUNK,
    MSG_TYPE_FILE_COMPLETE
} MessageType;

typedef struct {
    MessageType type;
    char filename[256];
    size_t file_size;
    size_t chunk_size;
    size_t total_chunks;
    size_t chunk_index;
    char data[CHUNK_SIZE];
} FileMessage;

typedef struct {
    char filepath[512];
    char filename[256];
    size_t file_size;
    float progress;
} FileTransfer;

static FileTransfer current_transfer = { 0 };
static bool transfer_in_progress = false;

size_t calculate_total_chunks(size_t file_size, size_t chunk_size);
const char* get_message_type_string(MessageType type);
bool is_valid_filename(const char* filename);

#endif
