#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include "protocol.h"
#include "relay_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FILE_TRANSFER_MAX_ACTIVE 8u
#define FILE_TRANSFER_MAX_PENDING_CONTROL 32u
#define FILE_TRANSFER_PATH_MAX 512u
#define FILE_TRANSFER_MAX_RECEIVED 100u

typedef struct FileTransferModule FileTransferModule;

typedef void (*FileTransferNotice)(void* context, const char* message);

typedef enum {
    FILE_TRANSFER_SENDING,
    FILE_TRANSFER_RECEIVING
} FileTransferDirection;

typedef struct {
    uint64_t offer_id;
    FileTransferDirection direction;
    char filename[PROTOCOL_FILENAME_MAX + 1u];
    char participant_name[PROTOCOL_DISPLAY_NAME_MAX + 1u];
    uint64_t total_size;
    uint64_t transferred_size;
} FileTransferProgress;

typedef struct {
    uint64_t offer_id;
    char sender_name[PROTOCOL_DISPLAY_NAME_MAX + 1u];
    char filename[PROTOCOL_FILENAME_MAX + 1u];
    uint64_t total_size;
} FileOfferSnapshot;

typedef struct {
    char filename[PROTOCOL_FILENAME_MAX + 1u];
    uint64_t size;
} ReceivedFileSnapshot;

FileTransferModule* file_transfer_create(const char* receive_directory,
    FileTransferNotice notice, void* notice_context);
void file_transfer_destroy(FileTransferModule* module);

bool file_transfer_offer_file(FileTransferModule* module, const RelayTransport* transport,
    const char* path);
void file_transfer_handle_message(FileTransferModule* module,
    const RelayTransport* transport, const RelayMessage* message);
void file_transfer_pump(FileTransferModule* module, const RelayTransport* transport);
void file_transfer_abort_all(FileTransferModule* module, const char* reason);

size_t file_transfer_pending_count(const FileTransferModule* module);
bool file_transfer_pending(const FileTransferModule* module, size_t index,
    FileOfferSnapshot* snapshot);
bool file_transfer_respond(FileTransferModule* module, const RelayTransport* transport,
    uint64_t offer_id, bool accepted, const char* save_directory);

size_t file_transfer_active_count(const FileTransferModule* module);
bool file_transfer_progress(const FileTransferModule* module, size_t index,
    FileTransferProgress* progress);

void file_transfer_scan_received(FileTransferModule* module);
size_t file_transfer_received_count(const FileTransferModule* module);
bool file_transfer_received(const FileTransferModule* module, size_t index,
    ReceivedFileSnapshot* snapshot);
bool file_transfer_remove_received(FileTransferModule* module, const char* filename);
void file_transfer_clear_received(FileTransferModule* module);

#endif
