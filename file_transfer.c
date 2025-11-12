#include "file_transfer.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fcntl.h>
static const char base64_chars[]
    = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char* base64_encode(const unsigned char* data, size_t input_length,
    size_t* output_length)
{
    *output_length = 4 * ((input_length + 2) / 3);
    char* encoded = malloc(*output_length + 1);
    if (!encoded)
        return NULL;

    size_t i, j;
    for (i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        encoded[j++] = base64_chars[(triple >> 18) & 0x3F];
        encoded[j++] = base64_chars[(triple >> 12) & 0x3F];
        encoded[j++] = base64_chars[(triple >> 6) & 0x3F];
        encoded[j++] = base64_chars[triple & 0x3F];
    }

    // Add padding
    for (i = 0; i < (3 - input_length % 3) % 3; i++)
        encoded[*output_length - 1 - i] = '=';

    encoded[*output_length] = '\0';
    return encoded;
}

// Load entire file as byte array (LEGACY - for small files only)
bool read_file_to_buffer(const char* filepath, unsigned char** buffer,
    size_t* size)
{
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        TraceLog(LOG_ERROR, "Cannot open file: %s", filepath);
        return false;
    }

    fseek(file, 0, SEEK_END);
    *size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (*size > MAX_FILE_SIZE) {
        TraceLog(LOG_ERROR, "File too large: %zu bytes (max: %d)", *size, MAX_FILE_SIZE);
        fclose(file);
        return false;
    }
    *buffer = malloc(*size);
    if (!*buffer) {
        fclose(file);
        return false;
    }

    size_t bytes_read = fread(*buffer, 1, *size, file);
    fclose(file);

    return bytes_read == *size;
}

// NEW: Stream a single chunk from file at given offset
bool stream_file_chunk(const char* filepath, size_t offset, unsigned char* buffer, size_t* chunk_size)
{
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        TraceLog(LOG_ERROR, "Cannot open file for streaming: %s", filepath);
        return false;
    }

    // Seek to offset
    if (fseek(file, offset, SEEK_SET) != 0) {
        TraceLog(LOG_ERROR, "Failed to seek to offset %zu in file %s", offset, filepath);
        fclose(file);
        return false;
    }

    // Read chunk
    size_t bytes_read = fread(buffer, 1, *chunk_size, file);
    *chunk_size = bytes_read;
    
    // Check for errors before closing
    bool had_error = (bytes_read == 0 && ferror(file));
    
    fclose(file);
    
    if (had_error) {
        TraceLog(LOG_ERROR, "Error reading chunk from file %s", filepath);
        return false;
    }

    return true;
}

// NEW: Write chunk directly to open file handle
bool write_chunk_to_file(FILE* file_handle, const unsigned char* chunk_data, size_t chunk_size)
{
    if (!file_handle || !chunk_data) {
        TraceLog(LOG_ERROR, "Invalid parameters to write_chunk_to_file");
        return false;
    }

    size_t bytes_written = fwrite(chunk_data, 1, chunk_size, file_handle);
    if (bytes_written != chunk_size) {
        TraceLog(LOG_ERROR, "Failed to write chunk: expected %zu bytes, wrote %zu",
                 chunk_size, bytes_written);
        return false;
    }

    // Flush to ensure data is written to disk
    fflush(file_handle);
    return true;
}

char* create_file_packet(const char* filename, size_t filesize,
    int chunk_index, int total_chunks,
    const unsigned char* chunk_data, size_t chunk_size)
{
    size_t encoded_length;
    char* encoded_chunk = base64_encode(chunk_data, chunk_size, &encoded_length);
    if (!encoded_chunk)
        return NULL;

    // Packet format: FILE:<filename>:<filesize>:<chunk_index>:<total_chunks>:<base64_data>\n
    // Add space for newline delimiter
    size_t packet_size = 5 + 1 + strlen(filename) + 1 + 20 + 1 + 10 + 1 + 10 + 1 + encoded_length + 2; // +2 for \n and \0
    char* packet = malloc(packet_size);

    snprintf(packet, packet_size, "FILE:%s:%zu:%d:%d:%s\n", filename, filesize, chunk_index, total_chunks, encoded_chunk);
    free(encoded_chunk);
    return packet;
}

bool parse_file_packet(const char* packet, FileTransfer* ft,
    unsigned char** chunk_data, size_t* chunk_size)
{
    // Check prefix
    if (strncmp(packet, "FILE:", 5) != 0)
        return false;

    // Parse format: FILE:<filename>:<filesize>:<chunk_index>:<total_chunks>:<base64_data>
    char filename[MAX_FILENAME];
    size_t filesize;
    int chunk_index, total_chunks;
    char* data_start;

    const char* p = packet + 5; // Skip "FILE:"

    // Extract filename (until first ':')
    const char* colon1 = strchr(p, ':');
    if (!colon1)
        return false;
    size_t name_len = colon1 - p;
    strncpy(filename, p, name_len);
    filename[name_len] = '\0';

    // Parse remaining fields
    int parsed = sscanf(colon1 + 1, "%zu:%d:%d:", &filesize, &chunk_index, &total_chunks);
    if (parsed != 3)
        return false;

    // Find base64 data (after 4th colon)
    data_start = strchr(colon1 + 1, ':');
    if (!data_start)
        return false;
    data_start = strchr(data_start + 1, ':');
    if (!data_start)
        return false;
    data_start = strchr(data_start + 1, ':');
    if (!data_start)
        return false;
    data_start++; // Move past the colon

    // Initialize transfer metadata on first chunk
    if (ft->file_handle == NULL) {
        TraceLog(LOG_INFO, "First chunk - initializing streaming transfer for %s", filename);
        strncpy(ft->filename, filename, MAX_FILENAME);
        ft->total_size = filesize;
        ft->total_chunks = total_chunks;
        ft->chunks_received = 0;
        ft->bytes_received = 0;
        ft->complete = false;

        // Create temporary file for writing
        char temp_path[512];
        snprintf(temp_path, sizeof(temp_path), "./received/%s.tmp", filename);
        
        ft->file_handle = fopen(temp_path, "wb");
        if (ft->file_handle == NULL) {
            TraceLog(LOG_ERROR, "Failed to open temporary file for streaming: %s", temp_path);
            return false;
        }

        TraceLog(LOG_INFO, "Opened streaming file for %s: %zu bytes (%d chunks)",
            filename, filesize, total_chunks);
    } else {
        TraceLog(LOG_INFO, "Subsequent chunk for %s (chunk %d/%d)", filename, chunk_index, total_chunks);
    }

    // Decode base64 data for EVERY chunk (not just first one)
    // Find the newline delimiter to handle concatenated packets
    const char* data_end = strchr(data_start, '\n');
    size_t base64_length;
    if (data_end) {
        base64_length = data_end - data_start;
    } else {
        // No newline found, use remaining string (for backwards compatibility)
        base64_length = strlen(data_start);
    }
    
    *chunk_data = base64_decode(data_start, base64_length, chunk_size);
    if (*chunk_data == NULL) {
        TraceLog(LOG_ERROR, "Failed to decode base64 data for %s chunk %d", filename, chunk_index);
        return false;
    }

    return true;
}

// Helper: Get numeric value from Base64 character
static int base64_char_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A'; // 0-25
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26; // 26-51
    if (c >= '0' && c <= '9')
        return c - '0' + 52; // 52-61
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1; // Invalid character or padding '='
}

unsigned char* base64_decode(const char* data, size_t input_length, size_t* output_length)
{
    TraceLog(LOG_INFO, "base64_decode: input_length=%zu", input_length);
    
    if (data == NULL || input_length == 0) {
        TraceLog(LOG_ERROR, "base64_decode: NULL or empty input");
        *output_length = 0;
        return NULL;
    }

    // Remove padding to get effective length
    size_t effective_length = input_length;
    size_t padding = 0;
    
    while (effective_length > 0 && data[effective_length - 1] == '=') {
        effective_length--;
        padding++;
    }

    TraceLog(LOG_INFO, "base64_decode: effective_length=%zu, padding=%zu", effective_length, padding);

    // Allocate maximum possible size first
    // Every 4 input chars can produce up to 3 output bytes
    size_t max_output = ((input_length + 3) / 4) * 3;
    unsigned char* decoded = malloc(max_output);
    if (!decoded) {
        TraceLog(LOG_ERROR, "base64_decode: malloc failed for %zu bytes", max_output);
        *output_length = 0;
        return NULL;
    }
    TraceLog(LOG_INFO, "base64_decode: allocated %zu bytes", max_output);

    size_t i = 0;  // Input position
    size_t j = 0;  // Output position

    // Simple approach: read groups of 4 characters, decode them
    while (i + 3 < input_length) {
        // Stop at padding
        if (data[i] == '=') break;
        
        // Read exactly 4 characters
        int val0 = base64_char_value(data[i++]);
        int val1 = base64_char_value(data[i++]);
        int val2 = base64_char_value(data[i++]);
        int val3 = base64_char_value(data[i++]);

        // If any of the first 2 are invalid, skip this group
        if (val0 < 0 || val1 < 0) {
            TraceLog(LOG_WARNING, "Invalid base64 at position %zu", i-4);
            continue;
        }

        // Decode the group
        uint32_t combined = ((uint32_t)val0 << 18) | ((uint32_t)val1 << 12);
        
        decoded[j++] = (combined >> 16) & 0xFF;
        
        if (val2 >= 0) {
            combined |= (uint32_t)val2 << 6;
            decoded[j++] = (combined >> 8) & 0xFF;
            
            if (val3 >= 0) {
                combined |= (uint32_t)val3;
                decoded[j++] = combined & 0xFF;
            }
        }
    }
    
    // Handle remaining characters (less than 4)
    if (i < input_length && data[i] != '=') {
        int val0 = base64_char_value(data[i++]);
        int val1 = (i < input_length && data[i] != '=') ? base64_char_value(data[i++]) : -1;
        int val2 = (i < input_length && data[i] != '=') ? base64_char_value(data[i++]) : -1;
        
        if (val0 >= 0 && val1 >= 0) {
            uint32_t combined = ((uint32_t)val0 << 18) | ((uint32_t)val1 << 12);
            decoded[j++] = (combined >> 16) & 0xFF;
            
            if (val2 >= 0) {
                combined |= (uint32_t)val2 << 6;
                decoded[j++] = (combined >> 8) & 0xFF;
            }
        }
    }

    // Set actual output length (j = bytes actually written)
    *output_length = j;
    TraceLog(LOG_INFO, "base64_decode: decoded %zu bytes (max was %zu)", j, max_output);
    
    // Optionally shrink buffer to actual size (not necessary but cleaner)
    if (j < max_output) {
        unsigned char* resized = realloc(decoded, j);
        if (resized) {
            decoded = resized;
        }
    }

    TraceLog(LOG_INFO, "base64_decode: returning decoded buffer");
    return decoded;
}

bool write_buffer_to_file(const char* filepath, const unsigned char* buffer, size_t size)
{
    if (filepath == NULL || buffer == NULL || size == 0) {
        TraceLog(LOG_ERROR, "Invalid parameters to write_buffer_to_file");
        return false;
    }

    FILE* file = fopen(filepath, "wb");

    if (file == NULL) {
        TraceLog(LOG_ERROR, "Failed to open file for writing: %s", filepath);
        return false;
    }

    size_t bytes_written = fwrite(buffer, 1, size, file);
    if (bytes_written != size) {
        TraceLog(LOG_ERROR, "Failed to write complete file. Expected %zu bytes, wrote %zu bytes",
            size, bytes_written);
        fclose(file);
        return false;
    }

    fflush(file);
    fclose(file);

    TraceLog(LOG_INFO, "Successfully wrote %zu bytes to %s", size, filepath);

    return true;
}

FileTransfer* create_file_transfer(const char* filename, size_t total_size, int total_chunks)
{
    FileTransfer* ft = malloc(sizeof(FileTransfer));
    if (!ft)
        return NULL;

    memset(ft, 0, sizeof(FileTransfer));
    strncpy(ft->filename, filename, MAX_FILENAME - 1);
    ft->total_size = total_size;
    ft->total_chunks = total_chunks;
    ft->file_handle = NULL;

    return ft;
}

// NEW: Create streaming transfer with file handle
FileTransfer* create_streaming_transfer(const char* filepath, size_t total_size, int total_chunks)
{
    FileTransfer* ft = create_file_transfer(filepath, total_size, total_chunks);
    if (!ft)
        return NULL;

    // Open temporary file for writing
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", filepath);
    
    ft->file_handle = fopen(temp_path, "wb");
    if (!ft->file_handle) {
        TraceLog(LOG_ERROR, "Failed to create streaming transfer file: %s", temp_path);
        free(ft);
        return NULL;
    }

    TraceLog(LOG_INFO, "Created streaming transfer: %s (%zu bytes, %d chunks)",
             filepath, total_size, total_chunks);
    return ft;
}

// NEW: Add chunk to streaming transfer (writes directly to disk)
bool add_chunk_to_streaming_transfer(FileTransfer* ft, int chunk_index,
                                     const unsigned char* data, size_t size)
{
    if (!ft || !ft->file_handle || !data) {
        TraceLog(LOG_ERROR, "Invalid streaming transfer state");
        return false;
    }

    // Write chunk to file
    if (!write_chunk_to_file(ft->file_handle, data, size)) {
        TraceLog(LOG_ERROR, "Failed to write chunk %d to file", chunk_index);
        return false;
    }

    ft->chunks_received++;
    ft->bytes_received += size;

    TraceLog(LOG_INFO, "Wrote chunk %d/%d (%zu bytes, total: %zu/%zu)",
             chunk_index + 1, ft->total_chunks, size,
             ft->bytes_received, ft->total_size);

    if (ft->chunks_received == ft->total_chunks) {
        ft->complete = true;
        TraceLog(LOG_INFO, "File transfer complete: %s", ft->filename);
    }

    return true;
}

// LEGACY: Old buffer-based function - kept for backward compatibility but deprecated
bool add_chunk_to_transfer(FileTransfer* ft, int chunk_index,
    const unsigned char* data, size_t size)
{
    // This function is deprecated - use add_chunk_to_streaming_transfer instead
    TraceLog(LOG_WARNING, "Using deprecated add_chunk_to_transfer function");
    return add_chunk_to_streaming_transfer(ft, chunk_index, data, size);
}

void free_file_transfer(FileTransfer* ft)
{
    if (ft) {
        if (ft->file_handle) {
            fclose(ft->file_handle);
            ft->file_handle = NULL;
        }
        free(ft);
    }
}

// NEW: Close streaming transfer and rename temp file on success
void close_streaming_transfer(FileTransfer* ft, bool success)
{
    if (!ft) return;

    if (ft->file_handle) {
        fclose(ft->file_handle);
        ft->file_handle = NULL;
    }

    char temp_path[512];
    char final_path[512];
    snprintf(temp_path, sizeof(temp_path), "./received/%s.tmp", ft->filename);
    snprintf(final_path, sizeof(final_path), "./received/%s", ft->filename);

    if (success && ft->complete) {
        // Rename temp file to final name
        if (rename(temp_path, final_path) == 0) {
            TraceLog(LOG_INFO, "Successfully saved file: %s", final_path);
        } else {
            TraceLog(LOG_ERROR, "Failed to rename temp file to final: %s", strerror(errno));
        }
    } else {
        // Delete temp file on failure
        if (remove(temp_path) == 0) {
            TraceLog(LOG_INFO, "Removed incomplete temp file: %s", temp_path);
        }
    }

    free(ft);
}

bool ensure_dir_exist(const char* dirpath)
{
    struct stat st = { 0 };
    if (stat(dirpath, &st) == -1) {
        if (mkdir(dirpath, 0755) == -1) {

            TraceLog(LOG_ERROR, "Failed to create directory %s: %s",
                dirpath, strerror(errno));
            return false;
        }
        TraceLog(LOG_INFO, "Created directory: %s", dirpath);
    }
    return true;
}
