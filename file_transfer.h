#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <stddef.h>
#include <stdbool.h>

#define FILE_TRANSFER_MAX_SIZE (500LL * 1024 * 1024) // 500 MB
#define FILE_CHUNK_SIZE 960000 // 48000 bytes is approximately 46.875 KB
#define FILE_ID_LEN 32
#define FILE_NAME_MAX_LEN 256
#define FILE_PATH_MAX_LEN 512
#define FILE_CHUNK_ENCODED_SIZE ((((FILE_CHUNK_SIZE) + 2) / 3) * 4)


size_t base64_encoded_size(size_t input_len);
int base64_encode(const unsigned char* input, size_t len, char* output, size_t out_size);
int base64_decode(const char* input, size_t len, unsigned char* output, size_t out_size);
void generate_file_id(char* out, size_t len);
void sanitize_filename(char* filename);
void trim_newline(char* text);

#endif // FILE_TRANSFER_H
