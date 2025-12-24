#include "file_transfer.h"
#include "platform.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
    #include <io.h>
    #include <stdint.h>
#else
    #include <unistd.h>
#endif

// Base64 implementation removed as we switched to binary streams

void generate_file_id(char* out, size_t len)
{
    static bool seeded = false;
    if (!seeded) {
#ifdef _WIN32
        srand((unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)&seeded);
#else
        srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
#endif
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
