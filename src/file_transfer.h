#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <stddef.h>
#include <stdbool.h>

#define FILE_TRANSFER_MAX_SIZE (500LL * 1024 * 1024) // 500 MB
#define FILE_CHUNK_SIZE (4 * 1024 * 1024)
#define MAX_CHUNKS_PER_FRAME 64
#define FILE_ID_LEN 32
#define FILE_NAME_MAX_LEN 256
#define FILE_PATH_MAX_LEN 512
// Base64 prototypes removed
void generate_file_id(char* out, size_t len);
void sanitize_filename(char* filename);
void trim_newline(char* text);

#endif // FILE_TRANSFER_H
