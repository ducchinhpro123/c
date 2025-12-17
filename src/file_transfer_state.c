#include "file_transfer_state.h"
#include "client_network.h"
#include "packet_queue.h"
#include "message.h"
#include "platform.h"
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
    #include <io.h>
    #include <direct.h>
#else
    #include <dirent.h>
    #include <unistd.h>
#endif

OutgoingTransfer outgoing_transfers[MAX_ACTIVE_TRANSFERS];
IncomingTransfer incoming_transfers[MAX_ACTIVE_TRANSFERS];
char incoming_stream[INCOMING_STREAM_CAPACITY];
size_t incoming_stream_len = 0;
FileEntry received_files[100];
int received_files_count = 0;

// External dependency on message queue for notifications
extern MessageQueue g_mq;

OutgoingTransfer* get_free_outgoing(void)
{
    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        if (!outgoing_transfers[i].active)
            return &outgoing_transfers[i];
    }
    return NULL;
}

IncomingTransfer* get_free_incoming(void)
{
    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        if (!incoming_transfers[i].active)
            return &incoming_transfers[i];
    }
    return NULL;
}

IncomingTransfer* get_incoming_transfer(const char* file_id)
{
    if (!file_id)
        return NULL;
    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        if (incoming_transfers[i].active && strcmp(incoming_transfers[i].file_id, file_id) == 0)
            return &incoming_transfers[i];
    }
    return NULL;
}

void close_outgoing_transfer(struct ClientConnection* conn, OutgoingTransfer* transfer, const char* error_msg)
{
    if (!transfer)
        return;

    char filename_copy[FILE_NAME_MAX_LEN];
    strncpy(filename_copy, transfer->filename, sizeof(filename_copy) - 1);
    filename_copy[sizeof(filename_copy) - 1] = '\0';

    if (transfer->fp) {
        fclose(transfer->fp);
        transfer->fp = NULL;
    }

    if (error_msg && *error_msg) {
        if (conn && transfer->file_id[0] != '\0') {
            size_t abort_len = strlen("FILE_ABORT|||") + strlen(transfer->sender) + strlen(transfer->file_id) + strlen(error_msg) + 1;
            char* abort_msg = (char*)malloc(abort_len);
            if (abort_msg) {
                snprintf(abort_msg, abort_len, "FILE_ABORT|%s|%s|%s", transfer->sender, transfer->file_id, error_msg);
                send_msg((ClientConnection*)conn, abort_msg);
                free(abort_msg);
            }
        }
        char buf[512];
        snprintf(buf, sizeof(buf), "SYSTEM: File transfer error for %s (%s)", filename_copy[0] ? filename_copy : "file", error_msg);
        add_message(&g_mq, "SYSTEM", buf);
    }

    memset(transfer, 0, sizeof(*transfer));
}

void finalize_incoming_transfer(IncomingTransfer* transfer, bool success, const char* reason)
{
    if (!transfer)
        return;

    char filename_copy[FILE_NAME_MAX_LEN];
    strncpy(filename_copy, transfer->filename, sizeof(filename_copy) - 1);
    filename_copy[sizeof(filename_copy) - 1] = '\0';
    char sender_copy[256];
    strncpy(sender_copy, transfer->sender, sizeof(sender_copy) - 1);
    sender_copy[sizeof(sender_copy) - 1] = '\0';
    char save_path_copy[FILE_PATH_MAX_LEN];
    strncpy(save_path_copy, transfer->save_path, sizeof(save_path_copy) - 1);
    save_path_copy[sizeof(save_path_copy) - 1] = '\0';
    size_t total_bytes_copy = transfer->total_bytes;

    if (transfer->fp) {
        fclose(transfer->fp);
        transfer->fp = NULL;
    }

    if (!success && save_path_copy[0] != '\0') {
        remove(save_path_copy);
    }

    if (success) {
        char buf[512];
        snprintf(buf, sizeof(buf), "SYSTEM: Received file %.200s from %.120s (%zu bytes)",
            filename_copy[0] ? filename_copy : "file",
            sender_copy[0] ? sender_copy : "peer",
            total_bytes_copy);
        add_message(&g_mq, "SYSTEM", buf);
        // Rescan received folder
        scan_received_folder();
    } else if (reason) {
        char buf[512];
        snprintf(buf, sizeof(buf), "SYSTEM: Failed to receive %.200s (%s)",
            filename_copy[0] ? filename_copy : "file",
            reason);
        add_message(&g_mq, "SYSTEM", buf);
    }

    memset(transfer, 0, sizeof(*transfer));
}

bool has_active_transfer(void)
{
    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; i++) {
        if (outgoing_transfers[i].active) {
            return true;
        }
        if (incoming_transfers[i].active) {
            return true;
        }
    }
    return false;
}

void abort_all_transfers(void)
{
    for (int i = 0; i < MAX_ACTIVE_TRANSFERS; ++i) {
        close_outgoing_transfer(NULL, &outgoing_transfers[i], "connection closed");
        finalize_incoming_transfer(&incoming_transfers[i], false, "connection closed");
    }
    incoming_stream_len = 0;
}

void ensure_receive_directory(void)
{
    struct stat st = { 0 };
    if (stat("received", &st) == -1) {
#ifdef _WIN32
        _mkdir("received"); // Windows: no mode parameter
#else
        mkdir("received", 0755); // Linux: with permissions
#endif
    }
}

void scan_received_folder(void)
{
    received_files_count = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA("received\\*", &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        TraceLog(LOG_ERROR, "Failed to open 'received' folder");
        return;
    }
    do {
        if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0)
            continue;
        if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && received_files_count < 100) {
            strncpy(received_files[received_files_count].filename, ffd.cFileName,
                    sizeof(received_files[received_files_count].filename) - 1);
            LARGE_INTEGER filesize;
            filesize.LowPart = ffd.nFileSizeLow;
            filesize.HighPart = ffd.nFileSizeHigh;
            received_files[received_files_count].size = (long)filesize.QuadPart;
            received_files_count++;
        }
    } while (FindNextFileA(hFind, &ffd));
    FindClose(hFind);
#else
    DIR* dir = opendir("received");
    if (!dir) {
        TraceLog(LOG_ERROR, "Failed to open 'received' folder: %s", strerror(errno));
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && received_files_count < 100) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "received/%s", entry->d_name);
        struct stat st;
        if (stat(filepath, &st) == 0 && S_ISREG(st.st_mode)) {
            strncpy(received_files[received_files_count].filename, entry->d_name,
                    sizeof(received_files[received_files_count].filename) - 1);
            received_files[received_files_count].size = st.st_size;
            received_files_count++;
        }
    }
    closedir(dir);
#endif
}

void remove_selected_file(const char* filepath)
{
    char full_filepath[512];
#ifdef _WIN32
    snprintf(full_filepath, sizeof(full_filepath), "received\\%s", filepath);
    if (DeleteFileA(full_filepath)) {
        TraceLog(LOG_INFO, "Deleted file: %s", full_filepath);
        scan_received_folder();
    } else {
        TraceLog(LOG_WARNING, "Failed to delete file %s", full_filepath);
    }
#else
    snprintf(full_filepath, sizeof(full_filepath), "received/%s", filepath);
    if (remove(full_filepath) == 0) {
        TraceLog(LOG_INFO, "Deleted file: %s", full_filepath);
        scan_received_folder();
    } else {
        TraceLog(LOG_WARNING, "Failed to delete file %s: %s", full_filepath, strerror(errno));
    }
#endif
}

void remove_all_files(void)
{
#ifdef _WIN32
    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA("received\\*", &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        TraceLog(LOG_ERROR, "Failed to open 'received' folder");
        return;
    }
    char filepath[512];
    do {
        if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0)
            continue;
        snprintf(filepath, sizeof(filepath), "received\\%s", ffd.cFileName);
        if (DeleteFileA(filepath)) {
            TraceLog(LOG_INFO, "Deleted file: %s", filepath);
        } else {
            TraceLog(LOG_WARNING, "Failed to delete file %s", filepath);
        }
    } while (FindNextFileA(hFind, &ffd));
    FindClose(hFind);
#else
    DIR* dir = opendir("received");
    if (dir == NULL) {
        TraceLog(LOG_ERROR, "Failed to open 'received' folder: %s", strerror(errno));
        return;
    }
    struct dirent* entry;
    char filepath[512];
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        snprintf(filepath, sizeof(filepath), "received/%s", entry->d_name);
        if (remove(filepath) == 0) {
            TraceLog(LOG_INFO, "Deleted file: %s", filepath);
        } else {
            TraceLog(LOG_WARNING, "Failed to delete file %s: %s", filepath, strerror(errno));
        }
    }
    closedir(dir);
#endif
    received_files_count = 0;
}
