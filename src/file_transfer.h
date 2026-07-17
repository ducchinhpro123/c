#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include "protocol.h"
#include <stdbool.h>
#include <stddef.h>

#define FILE_TRANSFER_MAX_SIZE (500LL * 1024 * 1024) // 500 MB
#define FILE_CHUNK_SIZE PROTOCOL_FILE_CHUNK_MAX
#define FILE_ID_LEN PROTOCOL_FILE_ID_LEN
#define FILE_NAME_MAX_LEN 256
#define FILE_PATH_MAX_LEN 512
// Base64 prototypes removed
bool generate_file_id(char* out, size_t len);
void sanitize_filename(char* filename);
void trim_newline(char* text);

#endif // FILE_TRANSFER_H
