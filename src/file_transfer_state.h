#ifndef FILE_TRANSFER_STATE_H
#define FILE_TRANSFER_STATE_H

#include "file_transfer.h"
#include <stdio.h>
#include <stdbool.h>

#define MAX_ACTIVE_TRANSFERS 8
#define INCOMING_STREAM_CAPACITY (65536 * 80)

struct ClientConnection;

typedef struct {
    bool active;
    bool meta_sent;
    char file_id[FILE_ID_LEN];
    char filename[FILE_NAME_MAX_LEN];
    char sender[256];
    FILE* fp;
    size_t total_bytes;
    size_t sent_bytes;
    size_t chunk_size;
    size_t next_chunk_index;
    size_t chunks_total;
} OutgoingTransfer;

typedef struct {
    bool active;
    char file_id[FILE_ID_LEN];
    char filename[FILE_NAME_MAX_LEN];
    char sender[256];
    char save_path[FILE_PATH_MAX_LEN];
    FILE* fp;
    size_t total_bytes;
    size_t received_bytes;
} IncomingTransfer;

typedef struct {
    char filename[256];
    long size;
} FileEntry;

// Globals exposed (or accessed via getters)
extern OutgoingTransfer outgoing_transfers[MAX_ACTIVE_TRANSFERS];
extern IncomingTransfer incoming_transfers[MAX_ACTIVE_TRANSFERS];
extern char incoming_stream[INCOMING_STREAM_CAPACITY];
extern size_t incoming_stream_len;
extern FileEntry received_files[100];
extern int received_files_count;

// Function prototypes
OutgoingTransfer* get_free_outgoing(void);
IncomingTransfer* get_free_incoming(void);
IncomingTransfer* get_incoming_transfer(const char* file_id);
void close_outgoing_transfer(struct ClientConnection* conn, OutgoingTransfer* transfer, const char* error_msg);
void finalize_incoming_transfer(IncomingTransfer* transfer, bool success, const char* reason);
bool has_active_transfer(void);
void abort_all_transfers(void);
void scan_received_folder(void);
void remove_all_files(void);
void remove_selected_file(const char* filepath);
void ensure_receive_directory(void);



#endif // FILE_TRANSFER_STATE_H
