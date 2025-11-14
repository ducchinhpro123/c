#include "file_transfer.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encoded_size(size_t input_len)
{
    return ((input_len + 2) / 3) * 4;
}

static int base64_lookup(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    if (c == '=')
        return -2; // padding
    return -1; // invalid
}

int base64_encode(const unsigned char* input, size_t len, char* output, size_t out_size)
{
    size_t needed = base64_encoded_size(len);
    if (out_size < needed + 1)
        return -1;

    size_t i = 0;
    size_t j = 0;

    while (i < len) {
        unsigned int octet_a = i < len ? input[i++] : 0;
        unsigned int octet_b = i < len ? input[i++] : 0;
        unsigned int octet_c = i < len ? input[i++] : 0;

        unsigned int triple = (octet_a << 0x10) | (octet_b << 0x08) | octet_c;

        output[j++] = base64_table[(triple >> 18) & 0x3F];
        output[j++] = base64_table[(triple >> 12) & 0x3F];
        output[j++] = base64_table[(triple >> 6) & 0x3F];
        output[j++] = base64_table[triple & 0x3F];
    }

    size_t mod = len % 3;
    if (mod > 0) {
        output[needed - 1] = '=';
        if (mod == 1)
            output[needed - 2] = '=';
    }

    output[needed] = '\0';
    return (int)needed;
}

int base64_decode(const char* input, size_t len, unsigned char* output, size_t out_size)
{
    if (len % 4 != 0)
        return -1;

    size_t output_len = (len / 4) * 3;
    if (len >= 1 && input[len - 1] == '=')
        output_len--;
    if (len >= 2 && input[len - 2] == '=')
        output_len--;

    if (out_size < output_len)
        return -1;

    size_t i = 0;
    size_t j = 0;

    while (i < len) {
        int sextet_a = base64_lookup(input[i++]);
        int sextet_b = base64_lookup(input[i++]);
        int sextet_c = base64_lookup(input[i++]);
        int sextet_d = base64_lookup(input[i++]);

        if (sextet_a < 0 || sextet_b < 0)
            return -1;

        unsigned int triple = (unsigned int)((sextet_a << 18) | (sextet_b << 12));
        output[j++] = (triple >> 16) & 0xFF;

        if (sextet_c == -2) {
            break;
        } else if (sextet_c < 0) {
            return -1;
        }

        triple |= (unsigned int)(sextet_c << 6);
        if (j >= output_len)
            break;
        output[j++] = (triple >> 8) & 0xFF;

        if (sextet_d == -2) {
            break;
        } else if (sextet_d < 0) {
            return -1;
        }

        triple |= (unsigned int)sextet_d;
        if (j >= output_len)
            break;
        output[j++] = triple & 0xFF;
    }

    return (int)output_len;
}

void generate_file_id(char* out, size_t len)
{
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
        seeded = true;
    }

    const char* hex = "0123456789abcdef";
    if (len == 0)
        return;

    for (size_t i = 0; i < len - 1; ++i) {
        out[i] = hex[rand() % 16];
    }
    out[len - 1] = '\0';
}

void sanitize_filename(char* filename)
{
    size_t len = strlen(filename);
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)filename[i];
        if (c == '|' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '\n' || c == '\r') {
            filename[i] = '_';
        }
    }
}

void trim_newline(char* text)
{
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == '\0')) {
        text[len - 1] = '\0';
        len--;
    }
}
