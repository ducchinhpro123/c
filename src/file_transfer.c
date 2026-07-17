#include "file_transfer.h"
#include "platform.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <bcrypt.h>
#include <io.h>
#include <stdint.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#endif

// Base64 implementation removed as we switched to binary streams

static bool secure_random_bytes(unsigned char* out, size_t len)
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, out, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    size_t offset = 0;
    while (offset < len) {
        ssize_t count = getrandom(out + offset, len - offset, 0);
        if (count > 0) {
            offset += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    if (offset == len)
        return true;

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return false;
    offset = 0;
    while (offset < len) {
        ssize_t count = read(fd, out + offset, len - offset);
        if (count > 0)
            offset += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    close(fd);
    return offset == len;
#endif
}

bool generate_file_id(char* out, size_t len)
{
    if (!out || len < 2)
        return false;

    const char* hex = "0123456789abcdef";
    size_t random_len = (len - 1 + 1) / 2;
    unsigned char random[FILE_ID_LEN] = { 0 };
    if (random_len > sizeof(random) || !secure_random_bytes(random, random_len)) {
        out[0] = '\0';
        return false;
    }

    for (size_t i = 0; i < len - 1; ++i) {
        unsigned char byte = random[i / 2];
        out[i] = hex[(i % 2 == 0) ? (byte >> 4) : (byte & 0x0f)];
    }
    out[len - 1] = '\0';
    return true;
}

void sanitize_filename(char* filename)
{
    size_t len = strlen(filename);

    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)filename[i];
        if (c < 0x20 || c == 0x7f || c == '|' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>') {
            filename[i] = '_';
        }
    }

    // Windows silently strips trailing dots/spaces; normalizing them keeps the
    // path we validate identical to the path the OS creates.
    while (len > 0 && (filename[len - 1] == '.' || filename[len - 1] == ' '))
        filename[--len] = '_';
    if (len == 0 || strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0)
        strcpy(filename, "unnamed_file");
}

void trim_newline(char* text)
{
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == '\0')) {
        text[len - 1] = '\0';
        len--;
    }
}
