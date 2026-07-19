#include "file_transfer.h"
#include "platform.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <bcrypt.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#endif

typedef enum {
    OUTGOING_FREE,
    OUTGOING_WAITING_FOR_ID,
    OUTGOING_OFFER_OPEN,
    OUTGOING_SENDING,
    OUTGOING_AWAITING_RESULTS
} OutgoingState;

typedef struct {
    OutgoingState state;
    uint64_t request_id;
    uint64_t offer_id;
    FILE* file;
    char filename[PROTOCOL_FILENAME_MAX + 1u];
    uint64_t total_size;
    uint64_t sent_size;
    uint32_t chunk_size;
    uint16_t pending_results;
} OutgoingTransfer;

typedef enum {
    INCOMING_FREE,
    INCOMING_PENDING,
    INCOMING_RECEIVING
} IncomingState;

typedef struct {
    IncomingState state;
    uint64_t offer_id;
    char sender_name[PROTOCOL_DISPLAY_NAME_MAX + 1u];
    char filename[PROTOCOL_FILENAME_MAX + 1u];
    char temporary_path[FILE_TRANSFER_PATH_MAX];
    char destination_directory[FILE_TRANSFER_PATH_MAX];
    FILE* file;
    uint64_t total_size;
    uint64_t received_size;
} IncomingTransfer;

struct FileTransferModule {
    OutgoingTransfer outgoing[FILE_TRANSFER_MAX_ACTIVE];
    IncomingTransfer incoming[FILE_TRANSFER_MAX_ACTIVE];
    ReceivedFileSnapshot received[FILE_TRANSFER_MAX_RECEIVED];
    size_t received_count;
    char receive_directory[FILE_TRANSFER_PATH_MAX];
    RelayMessage pending_controls[FILE_TRANSFER_MAX_PENDING_CONTROL];
    size_t pending_control_count;
    FileTransferNotice notice;
    void* notice_context;
};

static void notify(FileTransferModule* module, const char* format, ...)
{
    if (!module || !module->notice)
        return;
    char message[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    module->notice(module->notice_context, message);
}

static bool send_control(FileTransferModule* module, const RelayTransport* transport,
    const RelayMessage* message)
{
    if (module->pending_control_count > 0) {
        if (module->pending_control_count >= FILE_TRANSFER_MAX_PENDING_CONTROL)
            return false;
        module->pending_controls[module->pending_control_count++] = *message;
        return true;
    }
    RelaySendResult result = relay_transport_send(transport, message);
    if (result == RELAY_SEND_OK)
        return true;
    if (result != RELAY_SEND_BACKPRESSURE
        || module->pending_control_count >= FILE_TRANSFER_MAX_PENDING_CONTROL)
        return false;
    module->pending_controls[module->pending_control_count++] = *message;
    return true;
}

static bool pump_controls(FileTransferModule* module, const RelayTransport* transport)
{
    while (module->pending_control_count > 0) {
        RelaySendResult result = relay_transport_send(transport, &module->pending_controls[0]);
        if (result == RELAY_SEND_BACKPRESSURE)
            return false;
        if (result != RELAY_SEND_OK)
            notify(module, "A deferred File Transfer update could not be sent");
        module->pending_control_count--;
        if (module->pending_control_count > 0) {
            memmove(&module->pending_controls[0], &module->pending_controls[1],
                module->pending_control_count * sizeof(module->pending_controls[0]));
        }
    }
    return true;
}

static bool secure_random_bytes(uint8_t* bytes, size_t length)
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, bytes, (ULONG)length, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = getrandom(bytes + offset, length - offset, 0);
        if (count > 0) {
            offset += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    if (offset == length)
        return true;
    int descriptor = open("/dev/urandom", O_RDONLY);
    if (descriptor < 0)
        return false;
    offset = 0;
    while (offset < length) {
        ssize_t count = read(descriptor, bytes + offset, length - offset);
        if (count > 0)
            offset += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    close(descriptor);
    return offset == length;
#endif
}

static uint64_t new_request_id(void)
{
    for (unsigned attempt = 0; attempt < 4u; ++attempt) {
        uint64_t id = 0;
        if (!secure_random_bytes((uint8_t*)&id, sizeof(id)))
            return 0;
        if (id != 0)
            return id;
    }
    return 0;
}

static void sanitize_filename(char* filename)
{
    size_t length = strlen(filename);
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)filename[i];
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':' || c == '*'
            || c == '?' || c == '"' || c == '<' || c == '>')
            filename[i] = '_';
    }
    while (length > 0 && (filename[length - 1u] == '.' || filename[length - 1u] == ' '))
        filename[--length] = '_';
    if (length == 0 || strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0)
        strcpy(filename, "unnamed_file");
}

static bool ensure_directory(const char* path)
{
    struct stat status = { 0 };
    if (stat(path, &status) == 0)
        return S_ISDIR(status.st_mode);
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0700) == 0;
#endif
}

static const char* base_name(const char* path)
{
    const char* slash = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    const char* separator = slash;
    if (!separator || (backslash && backslash > separator))
        separator = backslash;
    return separator ? separator + 1 : path;
}

static bool join_path(char* destination, size_t capacity, const char* directory,
    const char* filename)
{
    size_t directory_length = strlen(directory);
    size_t filename_length = strlen(filename);
    bool needs_separator = directory_length > 0
        && directory[directory_length - 1u] != '/'
        && directory[directory_length - 1u] != '\\';
    size_t required = directory_length + (needs_separator ? 1u : 0u) + filename_length + 1u;
    if (required > capacity)
        return false;
    memcpy(destination, directory, directory_length);
    size_t offset = directory_length;
    if (needs_separator)
        destination[offset++] = '/';
    memcpy(destination + offset, filename, filename_length + 1u);
    return true;
}

static OutgoingTransfer* free_outgoing(FileTransferModule* module)
{
    for (size_t i = 0; i < FILE_TRANSFER_MAX_ACTIVE; ++i) {
        if (module->outgoing[i].state == OUTGOING_FREE)
            return &module->outgoing[i];
    }
    return NULL;
}

static OutgoingTransfer* outgoing_by_request(FileTransferModule* module, uint64_t request_id)
{
    for (size_t i = 0; i < FILE_TRANSFER_MAX_ACTIVE; ++i) {
        if (module->outgoing[i].state != OUTGOING_FREE
            && module->outgoing[i].request_id == request_id)
            return &module->outgoing[i];
    }
    return NULL;
}

static OutgoingTransfer* outgoing_by_offer(FileTransferModule* module, uint64_t offer_id)
{
    for (size_t i = 0; i < FILE_TRANSFER_MAX_ACTIVE; ++i) {
        if (module->outgoing[i].state != OUTGOING_FREE
            && module->outgoing[i].offer_id == offer_id)
            return &module->outgoing[i];
    }
    return NULL;
}

static IncomingTransfer* free_incoming(FileTransferModule* module)
{
    for (size_t i = 0; i < FILE_TRANSFER_MAX_ACTIVE; ++i) {
        if (module->incoming[i].state == INCOMING_FREE)
            return &module->incoming[i];
    }
    return NULL;
}

static IncomingTransfer* incoming_by_offer(FileTransferModule* module, uint64_t offer_id)
{
    for (size_t i = 0; i < FILE_TRANSFER_MAX_ACTIVE; ++i) {
        if (module->incoming[i].state != INCOMING_FREE
            && module->incoming[i].offer_id == offer_id)
            return &module->incoming[i];
    }
    return NULL;
}

static void clear_outgoing(OutgoingTransfer* transfer)
{
    if (!transfer)
        return;
    if (transfer->file)
        fclose(transfer->file);
    memset(transfer, 0, sizeof(*transfer));
}

static void cancel_outgoing(FileTransferModule* module, const RelayTransport* transport,
    OutgoingTransfer* transfer, const char* reason)
{
    if (!transfer)
        return;
    RelayMessage cancel = { .type = RELAY_MESSAGE_FILE_TRANSFER_CANCEL };
    cancel.as.file_transfer_cancel.offer_id = transfer->offer_id;
    snprintf(cancel.as.file_transfer_cancel.reason,
        sizeof(cancel.as.file_transfer_cancel.reason), "%s", reason ? reason : "Cancelled");
    if (transfer->offer_id != 0)
        (void)send_control(module, transport, &cancel);
    notify(module, "%s was cancelled (%s)", transfer->filename,
        reason ? reason : "Cancelled");
    clear_outgoing(transfer);
}

static void clear_incoming(IncomingTransfer* transfer, bool remove_partial)
{
    if (!transfer)
        return;
    if (transfer->file)
        fclose(transfer->file);
    if (remove_partial && transfer->temporary_path[0] != '\0')
        remove(transfer->temporary_path);
    memset(transfer, 0, sizeof(*transfer));
}

static void send_delivery_result(FileTransferModule* module,
    const RelayTransport* transport, uint64_t offer_id, bool success, const char* reason)
{
    RelayMessage result = { .type = RELAY_MESSAGE_FILE_DELIVERY_RESULT };
    result.as.file_delivery_result.offer_id = offer_id;
    result.as.file_delivery_result.success = success;
    snprintf(result.as.file_delivery_result.reason,
        sizeof(result.as.file_delivery_result.reason), "%s", reason ? reason : "");
    if (!send_control(module, transport, &result))
        notify(module, "Delivery result could not be queued");
}

static void fail_incoming(FileTransferModule* module, const RelayTransport* transport,
    IncomingTransfer* transfer, const char* reason)
{
    if (!transfer)
        return;
    uint64_t offer_id = transfer->offer_id;
    char filename[PROTOCOL_FILENAME_MAX + 1u];
    snprintf(filename, sizeof(filename), "%s", transfer->filename);
    clear_incoming(transfer, true);
    send_delivery_result(module, transport, offer_id, false, reason);
    notify(module, "Failed to receive %s (%s)", filename, reason ? reason : "unknown error");
}

typedef enum {
    PUBLISH_SUCCEEDED,
    PUBLISH_ALREADY_EXISTS,
    PUBLISH_FAILED
} PublishResult;

static PublishResult publish_without_replacing(const char* temporary_path,
    const char* destination)
{
#ifdef _WIN32
    if (MoveFileExA(temporary_path, destination, MOVEFILE_WRITE_THROUGH))
        return PUBLISH_SUCCEEDED;
    DWORD error = GetLastError();
    return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
        ? PUBLISH_ALREADY_EXISTS : PUBLISH_FAILED;
#else
    if (link(temporary_path, destination) != 0)
        return errno == EEXIST ? PUBLISH_ALREADY_EXISTS : PUBLISH_FAILED;
    if (unlink(temporary_path) == 0)
        return PUBLISH_SUCCEEDED;
    (void)remove(destination);
    return PUBLISH_FAILED;
#endif
}

static bool publish_received_file(IncomingTransfer* transfer, char* destination,
    size_t capacity)
{
    for (unsigned duplicate = 0; duplicate < 1000; ++duplicate) {
        int written = duplicate == 0
            ? snprintf(destination, capacity, "%s/%s", transfer->destination_directory,
                  transfer->filename)
            : snprintf(destination, capacity, "%s/%s(%u)", transfer->destination_directory,
                  transfer->filename, duplicate);
        if (written < 0 || (size_t)written >= capacity)
            return false;
        PublishResult result = publish_without_replacing(transfer->temporary_path, destination);
        if (result == PUBLISH_SUCCEEDED)
            return true;
        if (result == PUBLISH_FAILED)
            return false;
    }
    return false;
}

FileTransferModule* file_transfer_create(const char* receive_directory,
    FileTransferNotice notice, void* notice_context)
{
    FileTransferModule* module = calloc(1, sizeof(*module));
    if (!module)
        return NULL;
    const char* selected_directory = receive_directory && receive_directory[0]
        ? receive_directory : "received";
    if (strnlen(selected_directory, sizeof(module->receive_directory))
        >= sizeof(module->receive_directory)) {
        free(module);
        return NULL;
    }
    snprintf(module->receive_directory, sizeof(module->receive_directory), "%s",
        selected_directory);
    module->notice = notice;
    module->notice_context = notice_context;
    if (!ensure_directory(module->receive_directory)) {
        free(module);
        return NULL;
    }
    file_transfer_scan_received(module);
    return module;
}

void file_transfer_destroy(FileTransferModule* module)
{
    if (!module)
        return;
    file_transfer_abort_all(module, "File Transfer module closed");
    free(module);
}

bool file_transfer_offer_file(FileTransferModule* module, const RelayTransport* transport,
    const char* path)
{
    if (!module || !transport || !path || !relay_transport_is_connected(transport))
        return false;
    OutgoingTransfer* transfer = free_outgoing(module);
    if (!transfer) {
        notify(module, "Too many active File Offers");
        return false;
    }
    struct stat status = { 0 };
    if (stat(path, &status) != 0 || !S_ISREG(status.st_mode)
        || status.st_size < 0 || (uint64_t)status.st_size > PROTOCOL_FILE_MAX_SIZE) {
        notify(module, "File is invalid or exceeds 500 MB");
        return false;
    }
    FILE* file = fopen(path, "rb");
    if (!file) {
        notify(module, "Could not open the file");
        return false;
    }

    memset(transfer, 0, sizeof(*transfer));
    transfer->state = OUTGOING_WAITING_FOR_ID;
    transfer->request_id = new_request_id();
    transfer->file = file;
    transfer->total_size = (uint64_t)status.st_size;
    transfer->chunk_size = PROTOCOL_FILE_CHUNK_MAX;
    snprintf(transfer->filename, sizeof(transfer->filename), "%s", base_name(path));
    sanitize_filename(transfer->filename);
    if (transfer->request_id == 0) {
        clear_outgoing(transfer);
        notify(module, "Could not create a secure File Offer identity");
        return false;
    }

    RelayMessage create = { .type = RELAY_MESSAGE_FILE_OFFER_CREATE };
    create.as.file_offer_create.request_id = transfer->request_id;
    create.as.file_offer_create.total_size = transfer->total_size;
    create.as.file_offer_create.chunk_size = transfer->chunk_size;
    snprintf(create.as.file_offer_create.filename,
        sizeof(create.as.file_offer_create.filename), "%s", transfer->filename);
    if (!send_control(module, transport, &create)) {
        clear_outgoing(transfer);
        notify(module, "File Offer could not be queued");
        return false;
    }
    notify(module, "Offering %s (%.2f MB)", transfer->filename,
        (double)transfer->total_size / (1024.0 * 1024.0));
    return true;
}

static void handle_offer_created(FileTransferModule* module, const RelayMessage* message)
{
    OutgoingTransfer* transfer = outgoing_by_request(module,
        message->as.file_offer_created.request_id);
    if (!transfer || transfer->state != OUTGOING_WAITING_FOR_ID)
        return;
    transfer->offer_id = message->as.file_offer_created.offer_id;
    transfer->state = OUTGOING_OFFER_OPEN;
}

static void handle_offer_published(FileTransferModule* module,
    const RelayTransport* transport, const RelayMessage* message)
{
    if (incoming_by_offer(module, message->as.file_offer_published.offer_id))
        return;
    IncomingTransfer* transfer = free_incoming(module);
    if (!transfer) {
        RelayMessage response = { .type = RELAY_MESSAGE_FILE_OFFER_RESPONSE };
        response.as.file_offer_response.offer_id = message->as.file_offer_published.offer_id;
        response.as.file_offer_response.accepted = false;
        (void)send_control(module, transport, &response);
        notify(module, "A File Offer was declined because all incoming slots are busy");
        return;
    }
    memset(transfer, 0, sizeof(*transfer));
    transfer->state = INCOMING_PENDING;
    transfer->offer_id = message->as.file_offer_published.offer_id;
    transfer->total_size = message->as.file_offer_published.total_size;
    snprintf(transfer->sender_name, sizeof(transfer->sender_name), "%s",
        message->as.file_offer_published.sender_name);
    snprintf(transfer->filename, sizeof(transfer->filename), "%s",
        message->as.file_offer_published.filename);
    sanitize_filename(transfer->filename);
    notify(module, "File Offer: %s from %s (%.2f MB)", transfer->filename,
        transfer->sender_name, (double)transfer->total_size / (1024.0 * 1024.0));
}

static void handle_transfer_ready(FileTransferModule* module, const RelayMessage* message)
{
    OutgoingTransfer* transfer = outgoing_by_offer(module,
        message->as.file_transfer_ready.offer_id);
    if (!transfer || transfer->state != OUTGOING_OFFER_OPEN)
        return;
    transfer->state = OUTGOING_SENDING;
    transfer->pending_results = message->as.file_transfer_ready.recipient_count;
    notify(module, "%u Recipients accepted %s", transfer->pending_results, transfer->filename);
}

static void handle_incoming_chunk(FileTransferModule* module,
    const RelayTransport* transport, const RelayMessage* message)
{
    IncomingTransfer* transfer = incoming_by_offer(module, message->as.file_chunk.offer_id);
    if (!transfer || transfer->state != INCOMING_RECEIVING || !transfer->file)
        return;
    if (message->as.file_chunk.offset != transfer->received_size
        || transfer->received_size > transfer->total_size
        || message->as.file_chunk.data_length > transfer->total_size - transfer->received_size) {
        fail_incoming(module, transport, transfer, "File Transfer offset or size mismatch");
        return;
    }
    size_t written = fwrite(message->as.file_chunk.data, 1,
        message->as.file_chunk.data_length, transfer->file);
    if (written != message->as.file_chunk.data_length) {
        fail_incoming(module, transport, transfer, "Disk write failed");
        return;
    }
    transfer->received_size += written;
}

static void handle_incoming_end(FileTransferModule* module,
    const RelayTransport* transport, const RelayMessage* message)
{
    IncomingTransfer* transfer = incoming_by_offer(module,
        message->as.file_transfer_end.offer_id);
    if (!transfer || transfer->state != INCOMING_RECEIVING)
        return;
    if (message->as.file_transfer_end.total_size != transfer->total_size
        || transfer->received_size != transfer->total_size) {
        fail_incoming(module, transport, transfer, "File Transfer ended before all bytes arrived");
        return;
    }
    if (transfer->file && fclose(transfer->file) != 0) {
        transfer->file = NULL;
        fail_incoming(module, transport, transfer, "Could not close the received file");
        return;
    }
    transfer->file = NULL;

    char destination[FILE_TRANSFER_PATH_MAX];
    if (!publish_received_file(transfer, destination, sizeof(destination))) {
        fail_incoming(module, transport, transfer, "Could not publish the Received File");
        return;
    }
    uint64_t offer_id = transfer->offer_id;
    char filename[PROTOCOL_FILENAME_MAX + 1u];
    snprintf(filename, sizeof(filename), "%s", transfer->filename);
    memset(transfer, 0, sizeof(*transfer));
    send_delivery_result(module, transport, offer_id, true, "");
    file_transfer_scan_received(module);
    notify(module, "Received File %s", filename);
}

static void handle_delivery_update(FileTransferModule* module, const RelayMessage* message)
{
    OutgoingTransfer* transfer = outgoing_by_offer(module,
        message->as.file_delivery_update.offer_id);
    if (!transfer || (transfer->state != OUTGOING_SENDING
                         && transfer->state != OUTGOING_AWAITING_RESULTS))
        return;
    notify(module, "%s: %s%s%s%s", message->as.file_delivery_update.recipient_name,
        message->as.file_delivery_update.success ? "received " : "failed ",
        transfer->filename,
        message->as.file_delivery_update.reason[0] ? " — " : "",
        message->as.file_delivery_update.reason[0] ? message->as.file_delivery_update.reason : "");
    if (transfer->pending_results > 0)
        transfer->pending_results--;
    if (transfer->pending_results == 0 && transfer->state == OUTGOING_AWAITING_RESULTS)
        clear_outgoing(transfer);
}

static void handle_cancel(FileTransferModule* module, const RelayMessage* message)
{
    IncomingTransfer* incoming = incoming_by_offer(module,
        message->as.file_transfer_cancel.offer_id);
    if (incoming) {
        char filename[PROTOCOL_FILENAME_MAX + 1u];
        snprintf(filename, sizeof(filename), "%s", incoming->filename);
        clear_incoming(incoming, true);
        notify(module, "%s was cancelled (%s)", filename,
            message->as.file_transfer_cancel.reason);
    }
    OutgoingTransfer* outgoing = outgoing_by_offer(module,
        message->as.file_transfer_cancel.offer_id);
    if (outgoing) {
        char filename[PROTOCOL_FILENAME_MAX + 1u];
        snprintf(filename, sizeof(filename), "%s", outgoing->filename);
        clear_outgoing(outgoing);
        notify(module, "%s was cancelled (%s)", filename,
            message->as.file_transfer_cancel.reason);
    }
}

void file_transfer_handle_message(FileTransferModule* module,
    const RelayTransport* transport, const RelayMessage* message)
{
    if (!module || !transport || !message)
        return;
    switch (message->type) {
    case RELAY_MESSAGE_FILE_OFFER_CREATED:
        handle_offer_created(module, message);
        break;
    case RELAY_MESSAGE_FILE_OFFER_PUBLISHED:
        handle_offer_published(module, transport, message);
        break;
    case RELAY_MESSAGE_FILE_TRANSFER_READY:
        handle_transfer_ready(module, message);
        break;
    case RELAY_MESSAGE_FILE_CHUNK:
        handle_incoming_chunk(module, transport, message);
        break;
    case RELAY_MESSAGE_FILE_TRANSFER_END:
        handle_incoming_end(module, transport, message);
        break;
    case RELAY_MESSAGE_FILE_DELIVERY_UPDATE:
        handle_delivery_update(module, message);
        break;
    case RELAY_MESSAGE_FILE_OFFER_DECLINED: {
        OutgoingTransfer* transfer = outgoing_by_offer(module,
            message->as.file_offer_declined.offer_id);
        if (transfer) {
            char filename[PROTOCOL_FILENAME_MAX + 1u];
            snprintf(filename, sizeof(filename), "%s", transfer->filename);
            clear_outgoing(transfer);
            notify(module, "File Offer for %s was declined", filename);
        }
        break;
    }
    case RELAY_MESSAGE_FILE_TRANSFER_CANCEL:
        handle_cancel(module, message);
        break;
    case RELAY_MESSAGE_ACTION_REJECTED: {
        OutgoingTransfer* transfer = message->as.action_rejected.rejected_type
                == RELAY_MESSAGE_FILE_OFFER_CREATE
            ? outgoing_by_request(module, message->as.action_rejected.correlation_id)
            : outgoing_by_offer(module, message->as.action_rejected.correlation_id);
        if (transfer) {
            char filename[PROTOCOL_FILENAME_MAX + 1u];
            snprintf(filename, sizeof(filename), "%s", transfer->filename);
            clear_outgoing(transfer);
            notify(module, "%s was rejected (%s)", filename,
                message->as.action_rejected.reason);
        }
        break;
    }
    default:
        break;
    }
}

void file_transfer_pump(FileTransferModule* module, const RelayTransport* transport)
{
    if (!module || !transport || !relay_transport_is_connected(transport))
        return;
    if (!pump_controls(module, transport))
        return;
    unsigned budget = 4;
    for (size_t i = 0; i < FILE_TRANSFER_MAX_ACTIVE && budget > 0; ++i) {
        OutgoingTransfer* transfer = &module->outgoing[i];
        if (transfer->state != OUTGOING_SENDING)
            continue;
        while (transfer->sent_size < transfer->total_size && budget > 0) {
            uint64_t remaining = transfer->total_size - transfer->sent_size;
            uint32_t wanted = remaining > transfer->chunk_size
                ? transfer->chunk_size : (uint32_t)remaining;
            uint8_t* bytes = malloc(wanted);
            if (!bytes) {
                notify(module, "Could not allocate a File Transfer chunk");
                return;
            }
            size_t read_count = fread(bytes, 1, wanted, transfer->file);
            if (read_count != wanted) {
                free(bytes);
                cancel_outgoing(module, transport, transfer, "File read failed");
                break;
            }
            RelayMessage chunk = { .type = RELAY_MESSAGE_FILE_CHUNK };
            chunk.as.file_chunk.offer_id = transfer->offer_id;
            chunk.as.file_chunk.offset = transfer->sent_size;
            chunk.as.file_chunk.data = bytes;
            chunk.as.file_chunk.data_length = (uint32_t)read_count;
            RelaySendResult result = relay_transport_send(transport, &chunk);
            free(bytes);
            if (result == RELAY_SEND_BACKPRESSURE) {
                if (fseek(transfer->file, -(long)read_count, SEEK_CUR) != 0)
                    cancel_outgoing(module, transport, transfer,
                        "Could not retry after backpressure");
                return;
            }
            if (result != RELAY_SEND_OK) {
                notify(module, "File Transfer connection closed");
                clear_outgoing(transfer);
                return;
            }
            transfer->sent_size += read_count;
            budget--;
        }
        if (transfer->state == OUTGOING_SENDING
            && transfer->sent_size == transfer->total_size) {
            RelayMessage end = { .type = RELAY_MESSAGE_FILE_TRANSFER_END };
            end.as.file_transfer_end.offer_id = transfer->offer_id;
            end.as.file_transfer_end.total_size = transfer->total_size;
            RelaySendResult result = relay_transport_send(transport, &end);
            if (result == RELAY_SEND_BACKPRESSURE)
                return;
            if (result != RELAY_SEND_OK) {
                clear_outgoing(transfer);
                return;
            }
            fclose(transfer->file);
            transfer->file = NULL;
            transfer->state = OUTGOING_AWAITING_RESULTS;
            notify(module, "Finished sending %s; awaiting Delivery results", transfer->filename);
        }
    }
}

void file_transfer_abort_all(FileTransferModule* module, const char* reason)
{
    if (!module)
        return;
    for (size_t i = 0; i < FILE_TRANSFER_MAX_ACTIVE; ++i) {
        if (module->outgoing[i].state != OUTGOING_FREE)
            clear_outgoing(&module->outgoing[i]);
        if (module->incoming[i].state != INCOMING_FREE)
            clear_incoming(&module->incoming[i], true);
    }
    module->pending_control_count = 0;
    if (reason)
        notify(module, "%s", reason);
}

size_t file_transfer_pending_count(const FileTransferModule* module)
{
    if (!module)
        return 0;
    size_t count = 0;
    for (size_t i = 0; i < FILE_TRANSFER_MAX_ACTIVE; ++i) {
        if (module->incoming[i].state == INCOMING_PENDING)
            count++;
    }
    return count;
}

bool file_transfer_pending(const FileTransferModule* module, size_t index,
    FileOfferSnapshot* snapshot)
{
    if (!module || !snapshot)
        return false;
    for (size_t i = 0; i < FILE_TRANSFER_MAX_ACTIVE; ++i) {
        const IncomingTransfer* transfer = &module->incoming[i];
        if (transfer->state != INCOMING_PENDING)
            continue;
        if (index-- == 0) {
            memset(snapshot, 0, sizeof(*snapshot));
            snapshot->offer_id = transfer->offer_id;
            snapshot->total_size = transfer->total_size;
            snprintf(snapshot->sender_name, sizeof(snapshot->sender_name), "%s",
                transfer->sender_name);
            snprintf(snapshot->filename, sizeof(snapshot->filename), "%s",
                transfer->filename);
            return true;
        }
    }
    return false;
}

bool file_transfer_respond(FileTransferModule* module, const RelayTransport* transport,
    uint64_t offer_id, bool accepted, const char* save_directory)
{
    if (!module || !transport)
        return false;
    IncomingTransfer* transfer = incoming_by_offer(module, offer_id);
    if (!transfer || transfer->state != INCOMING_PENDING)
        return false;

    if (accepted) {
        const char* selected_directory = save_directory && save_directory[0]
            ? save_directory : module->receive_directory;
        if (strnlen(selected_directory, sizeof(transfer->destination_directory))
            >= sizeof(transfer->destination_directory)) {
            accepted = false;
            notify(module, "The selected receive path is too long");
        }
        if (accepted) {
            snprintf(transfer->destination_directory,
                sizeof(transfer->destination_directory), "%s", selected_directory);
        }
        if (accepted && !ensure_directory(transfer->destination_directory)) {
            accepted = false;
            notify(module, "Could not create the selected receive directory");
        } else if (accepted) {
            int written = snprintf(transfer->temporary_path, sizeof(transfer->temporary_path),
                "%s/.relay-%016llx.part", transfer->destination_directory,
                (unsigned long long)offer_id);
            if (written < 0 || (size_t)written >= sizeof(transfer->temporary_path)) {
                accepted = false;
                notify(module, "The temporary receive path is too long");
            } else {
                transfer->file = fopen(transfer->temporary_path, "wbx");
                if (!transfer->file) {
                    accepted = false;
                    notify(module, "Could not create the partial Received File");
                }
            }
        }
    }

    RelayMessage response = { .type = RELAY_MESSAGE_FILE_OFFER_RESPONSE };
    response.as.file_offer_response.offer_id = offer_id;
    response.as.file_offer_response.accepted = accepted;
    if (!send_control(module, transport, &response)) {
        clear_incoming(transfer, true);
        notify(module, "File Offer response could not be queued");
        return false;
    }
    if (!accepted) {
        notify(module, "Declined File Offer for %s", transfer->filename);
        clear_incoming(transfer, true);
    } else {
        transfer->state = INCOMING_RECEIVING;
        notify(module, "Accepted %s from %s", transfer->filename, transfer->sender_name);
    }
    return true;
}

size_t file_transfer_active_count(const FileTransferModule* module)
{
    if (!module)
        return 0;
    size_t count = 0;
    for (size_t i = 0; i < FILE_TRANSFER_MAX_ACTIVE; ++i) {
        if (module->outgoing[i].state == OUTGOING_SENDING
            || module->outgoing[i].state == OUTGOING_AWAITING_RESULTS)
            count++;
        if (module->incoming[i].state == INCOMING_RECEIVING)
            count++;
    }
    return count;
}

bool file_transfer_progress(const FileTransferModule* module, size_t index,
    FileTransferProgress* progress)
{
    if (!module || !progress)
        return false;
    for (size_t i = 0; i < FILE_TRANSFER_MAX_ACTIVE; ++i) {
        const OutgoingTransfer* outgoing = &module->outgoing[i];
        if (outgoing->state == OUTGOING_SENDING
            || outgoing->state == OUTGOING_AWAITING_RESULTS) {
            if (index-- == 0) {
                memset(progress, 0, sizeof(*progress));
                progress->offer_id = outgoing->offer_id;
                progress->direction = FILE_TRANSFER_SENDING;
                progress->total_size = outgoing->total_size;
                progress->transferred_size = outgoing->sent_size;
                snprintf(progress->filename, sizeof(progress->filename), "%s",
                    outgoing->filename);
                return true;
            }
        }
        const IncomingTransfer* incoming = &module->incoming[i];
        if (incoming->state == INCOMING_RECEIVING && index-- == 0) {
            memset(progress, 0, sizeof(*progress));
            progress->offer_id = incoming->offer_id;
            progress->direction = FILE_TRANSFER_RECEIVING;
            progress->total_size = incoming->total_size;
            progress->transferred_size = incoming->received_size;
            snprintf(progress->filename, sizeof(progress->filename), "%s",
                incoming->filename);
            snprintf(progress->participant_name, sizeof(progress->participant_name), "%s",
                incoming->sender_name);
            return true;
        }
    }
    return false;
}

static bool is_internal_receive_name(const char* filename)
{
    if (!filename || strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0)
        return true;
    size_t length = strlen(filename);
    return strncmp(filename, ".relay-", 7u) == 0 && length >= 5u
        && strcmp(filename + length - 5u, ".part") == 0;
}

void file_transfer_scan_received(FileTransferModule* module)
{
    if (!module)
        return;
    module->received_count = 0;
#ifdef _WIN32
    char pattern[FILE_TRANSFER_PATH_MAX];
    if (!join_path(pattern, sizeof(pattern), module->receive_directory, "*"))
        return;
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    do {
        if (is_internal_receive_name(data.cFileName)
            || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            || module->received_count >= FILE_TRANSFER_MAX_RECEIVED)
            continue;
        ReceivedFileSnapshot* entry = &module->received[module->received_count++];
        snprintf(entry->filename, sizeof(entry->filename), "%s", data.cFileName);
        ULARGE_INTEGER size;
        size.LowPart = data.nFileSizeLow;
        size.HighPart = data.nFileSizeHigh;
        entry->size = size.QuadPart;
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
#else
    DIR* directory = opendir(module->receive_directory);
    if (!directory)
        return;
    struct dirent* entry;
    while ((entry = readdir(directory)) != NULL
        && module->received_count < FILE_TRANSFER_MAX_RECEIVED) {
        if (is_internal_receive_name(entry->d_name))
            continue;
        char path[FILE_TRANSFER_PATH_MAX];
        if (!join_path(path, sizeof(path), module->receive_directory, entry->d_name))
            continue;
        struct stat status;
        if (stat(path, &status) != 0 || !S_ISREG(status.st_mode))
            continue;
        ReceivedFileSnapshot* received = &module->received[module->received_count++];
        snprintf(received->filename, sizeof(received->filename), "%s", entry->d_name);
        received->size = (uint64_t)status.st_size;
    }
    closedir(directory);
#endif
}

size_t file_transfer_received_count(const FileTransferModule* module)
{
    return module ? module->received_count : 0;
}

bool file_transfer_received(const FileTransferModule* module, size_t index,
    ReceivedFileSnapshot* snapshot)
{
    if (!module || !snapshot || index >= module->received_count)
        return false;
    *snapshot = module->received[index];
    return true;
}

static bool safe_received_name(const char* filename)
{
    return filename && filename[0] != '\0' && !is_internal_receive_name(filename)
        && !strchr(filename, '/') && !strchr(filename, '\\');
}

bool file_transfer_remove_received(FileTransferModule* module, const char* filename)
{
    if (!module || !safe_received_name(filename))
        return false;
    bool catalogued = false;
    for (size_t i = 0; i < module->received_count; ++i) {
        if (strcmp(module->received[i].filename, filename) == 0) {
            catalogued = true;
            break;
        }
    }
    if (!catalogued)
        return false;
    char path[FILE_TRANSFER_PATH_MAX];
    if (!join_path(path, sizeof(path), module->receive_directory, filename))
        return false;
    if (remove(path) != 0)
        return false;
    file_transfer_scan_received(module);
    return true;
}

void file_transfer_clear_received(FileTransferModule* module)
{
    if (!module)
        return;
    for (size_t i = 0; i < module->received_count; ++i) {
        char path[FILE_TRANSFER_PATH_MAX];
        if (join_path(path, sizeof(path), module->receive_directory,
                module->received[i].filename))
            (void)remove(path);
    }
    file_transfer_scan_received(module);
}
