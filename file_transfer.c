#include "file_transfer.h"

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

bool read_file_to_buffer(const char* filepath, unsigned char** buffer,
    size_t* size)
{
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        TraceLog(LOG_ERROR, "Cannot open file: %s", filepath);
    }

    return false;
}
