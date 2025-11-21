# File Transfer Architecture Documentation

## Overview
This document explains how the file transfer system works in your client-server chat application.

---

## System Architecture

```
┌─────────────┐         ┌─────────────┐         ┌─────────────┐
│   Client A  │         │   Server    │         │   Client B  │
│  (Sender)   │◄───────►│             │◄───────►│ (Receiver)  │
└─────────────┘         └─────────────┘         └─────────────┘
      │                        │                        │
      │   1. Drop file         │                        │
      │   2. Send FILE_META    │                        │
      │   3. Send FILE_CHUNK   │   Forward packets      │
      │   4. Send FILE_END     │   ─────────────────►   │
      │                        │                        │
```

---

## File Transfer Protocol

### Message Format
All messages are newline-delimited text packets sent over TCP sockets.

```
FILE_META|sender|file_id|filename|total_bytes|chunk_size\n
FILE_CHUNK|sender|file_id|chunk_index|base64_payload\n
FILE_END|sender|file_id\n
FILE_ABORT|sender|file_id|reason\n
```

### Key Constants
```c
FILE_CHUNK_SIZE = 48000 bytes        // Raw binary chunk size
FILE_TRANSFER_MAX_SIZE = 500 MB     // Maximum file size
MSG_BUFFER = 65536 bytes             // Message buffer size
FILE_CHUNK_ENCODED_SIZE ≈ 64000 bytes // Base64 encoded chunk size
```

---

## Complete File Transfer Flow

### Phase 1: File Drop & Initialization

```
┌─────────────────────────────────────────────────────────┐
│ Client A (Sender)                                       │
└─────────────────────────────────────────────────────────┘

User drops file onto window
         ↓
process_file_drop() detects dropped file
         ↓
start_outgoing_transfer() called
         ↓
┌────────────────────────────────────────┐
│ Create OutgoingTransfer struct:       │
│  - Read file stats (size, name)       │
│  - Open file for reading               │
│  - Generate unique file_id             │
│  - Calculate total chunks needed       │
│  - Set sender username                 │
│  - Mark as active                      │
└────────────────────────────────────────┘
         ↓
Add "SYSTEM: Sending..." to local UI
```

**Code Location**: [`client_gui.c:704-761`](client_gui.c:704)

---

### Phase 2: Metadata Transmission

```
┌──────────────────────────────────────────────────────────┐
│ Client A (Sender) - pump_outgoing_transfers()           │
└──────────────────────────────────────────────────────────┘

Every frame (60 FPS):
         ↓
Check if meta_sent == false
         ↓
Format FILE_META message:
  "FILE_META|username|abc123|file.txt|1024000|48000"
         ↓
send_msg() → client_network.c
         ↓
┌────────────────────────────────────┐
│ send_all() helper function:       │
│  - Handles partial sends           │
│  - Retries on EAGAIN/EWOULDBLOCK  │
│  - Appends newline delimiter       │
└────────────────────────────────────┘
         ↓
         ▼
┌──────────────────────────────────────────────────────────┐
│ Server - handle_client_message()                        │
└──────────────────────────────────────────────────────────┘
         ↓
Detects "FILE_" prefix
         ↓
process_file_message() called
         ↓
Parse FILE_META packet:
  - Extract sender, file_id, filename, size, chunk_size
  - Validate: size < 500MB, chunk_size valid
         ↓
Create FileTransferSession:
  - Track file_id, sender_fd, total_bytes
  - Initialize forwarded_bytes = 0
         ↓
Return true (allow broadcast)
         ↓
server_broadcast_msg() to all OTHER clients
         ↓
         ▼
┌──────────────────────────────────────────────────────────┐
│ Client B (Receiver) - process_incoming_stream()         │
└──────────────────────────────────────────────────────────┘
         ↓
Receive data from server
         ↓
Buffer in incoming_stream[] until '\n' found
         ↓
handle_file_packet() called
         ↓
Parse FILE_META:
  sender = "username"
  file_id = "abc123"
  filename = "file.txt"
  total_bytes = 1024000
  chunk_size = 48000
         ↓
┌────────────────────────────────────────┐
│ Create IncomingTransfer struct:       │
│  - Store file_id, filename, sender    │
│  - Sanitize filename (remove /, :)    │
│  - Create path: received/filename     │
│  - Handle duplicates: file(1), file(2)│
│  - Open file for writing (wb mode)    │
│  - Mark as active                      │
└────────────────────────────────────────┘
         ↓
Add "SYSTEM: Receiving..." to local UI
```

**Key Code Locations**:
- Sender: [`client_gui.c:788-803`](client_gui.c:788)
- Server: [`server.c:468-494`](server.c:468)
- Receiver: [`client_gui.c:546-601`](client_gui.c:546)

---

### Phase 3: Chunk Transmission (Bulk Data Transfer)

```
┌──────────────────────────────────────────────────────────┐
│ Client A (Sender) - pump_outgoing_transfers()           │
└──────────────────────────────────────────────────────────┘

Loop (up to MAX_CHUNKS_PER_BATCH = 64 per frame):
         ↓
Read 48,000 bytes from file into chunk_buffer[]
         ↓
┌────────────────────────────────────────┐
│ Base64 Encode:                         │
│  Input:  48000 bytes binary            │
│  Output: ~64000 bytes ASCII text       │
│                                        │
│  Why? TCP is text-based protocol,     │
│  binary data needs encoding            │
└────────────────────────────────────────┘
         ↓
Format FILE_CHUNK message:
  "FILE_CHUNK|username|abc123|0|AAAABBBBCCCC...base64data"
         ↓
send_msg() to server
         ↓
Update: sent_bytes += 48000
Update: next_chunk_index++
         ↓
Repeat until all chunks sent OR time budget exceeded
         ↓
         ▼
┌──────────────────────────────────────────────────────────┐
│ Server - process_file_message()                         │
└──────────────────────────────────────────────────────────┘
         ↓
Parse FILE_CHUNK packet
         ↓
Find FileTransferSession by file_id
         ↓
Validate:
  - Session exists and sender_fd matches
  - Payload size reasonable
  - Total forwarded doesn't exceed file size
         ↓
Update: forwarded_bytes += chunk_size
         ↓
Broadcast to all OTHER clients
         ↓
         ▼
┌──────────────────────────────────────────────────────────┐
│ Client B (Receiver) - handle_file_packet()              │
└──────────────────────────────────────────────────────────┘
         ↓
Parse FILE_CHUNK packet
         ↓
Extract base64 payload string
         ↓
┌────────────────────────────────────────┐
│ Base64 Decode:                         │
│  Input:  ~64000 bytes ASCII            │
│  Output: 48000 bytes binary            │
│                                        │
│  Restore original binary data          │
└────────────────────────────────────────┘
         ↓
fwrite() decoded bytes to file
         ↓
Update: received_bytes += written
         ↓
Check: received_bytes > total_bytes?
  YES → Abort (too much data)
  NO  → Continue
         ↓
Repeat for each chunk...
```

**Key Code Locations**:
- Sender: [`client_gui.c:805-841`](client_gui.c:805)
- Server: [`server.c:496-540`](server.c:496)
- Receiver: [`client_gui.c:602-629`](client_gui.c:602)
- Encoding: [`file_transfer.c:33-64`](file_transfer.c:33)
- Decoding: [`file_transfer.c:66-119`](file_transfer.c:66)

---

### Phase 4: Transfer Completion

```
┌──────────────────────────────────────────────────────────┐
│ Client A (Sender)                                        │
└──────────────────────────────────────────────────────────┘

Check: sent_bytes >= total_bytes?
         ↓
YES: All chunks sent
         ↓
Close file handle
         ↓
Format FILE_END message:
  "FILE_END|username|abc123"
         ↓
send_msg() to server
         ↓
Add "SYSTEM: Finished sending..." to local UI
         ↓
Clear OutgoingTransfer struct (mark inactive)
         ↓
         ▼
┌──────────────────────────────────────────────────────────┐
│ Server                                                   │
└──────────────────────────────────────────────────────────┘
         ↓
Parse FILE_END packet
         ↓
Find FileTransferSession
         ↓
Validate: forwarded_bytes ≈ total_bytes (±2KB tolerance)
         ↓
Remove session (cleanup)
         ↓
Broadcast FILE_END to receivers
         ↓
         ▼
┌──────────────────────────────────────────────────────────┐
│ Client B (Receiver)                                      │
└──────────────────────────────────────────────────────────┘
         ↓
Parse FILE_END packet
         ↓
Find IncomingTransfer by file_id
         ↓
Validate: received_bytes == total_bytes?
         ↓
YES: Success!
  - Close file handle
  - Add "SYSTEM: Received file..." to UI
  - Keep file in received/ folder
         ↓
NO: Incomplete!
  - Close and DELETE partial file
  - Add "SYSTEM: Failed..." to UI
         ↓
Clear IncomingTransfer struct
```

**Key Code Locations**:
- Sender: [`client_gui.c:846-865`](client_gui.c:846)
- Server: [`server.c:542-563`](server.c:542)
- Receiver: [`client_gui.c:630-643`](client_gui.c:630)

---

## Data Structures

### OutgoingTransfer (Sender Side)
```c
typedef struct {
    bool active;                      // Is transfer in progress?
    bool meta_sent;                   // Has FILE_META been sent?
    char file_id[32];                 // Unique transfer ID
    char filename[256];               // Original filename
    char sender[256];                 // Sender username
    FILE* fp;                         // File handle
    size_t total_bytes;               // Total file size
    size_t sent_bytes;                // Bytes sent so far
    size_t chunk_size;                // Size per chunk (48000)
    size_t next_chunk_index;          // Next chunk number
    size_t chunks_total;              // Total chunks needed
} OutgoingTransfer;
```

### IncomingTransfer (Receiver Side)
```c
typedef struct {
    bool active;                      // Is transfer in progress?
    char file_id[32];                 // Transfer ID (matches sender)
    char filename[256];               // Sanitized filename
    char sender[256];                 // Who sent it
    char save_path[512];              // Full path: received/filename
    FILE* fp;                         // File handle for writing
    size_t total_bytes;               // Expected total size
    size_t received_bytes;            // Bytes written so far
} IncomingTransfer;
```

### FileTransferSession (Server Side)
```c
typedef struct {
    bool active;                      // Is session active?
    char file_id[32];                 // Transfer ID
    int sender_fd;                    // Sender's socket FD
    size_t total_bytes;               // Expected total size
    size_t forwarded_bytes;           // Bytes forwarded so far
    size_t chunk_size;                // Chunk size
} FileTransferSession;
```

---

## Network Flow Control

### send_all() Function
Handles the complexity of non-blocking socket writes:

```
┌─────────────────────────────────────┐
│ send_all(socket, data, length)     │
└─────────────────────────────────────┘
         ↓
   total_sent = 0
         ↓
  ┌──────────────────┐
  │ While total_sent │
  │    < length      │
  └──────────────────┘
         ↓
   Try send() remaining data
         ↓
   ┌─────────────────────────────┐
   │ Result?                     │
   └─────────────────────────────┘
         ↓
   ┌────────────┬──────────────┬──────────────┐
   │ Success    │ EAGAIN       │ Error        │
   │ (bytes>0)  │ (buffer full)│ (EPIPE, etc) │
   └────────────┴──────────────┴──────────────┘
         ↓            ↓               ↓
   Update total  usleep()        Return -1
   Reset retry   Retry++         (failed)
         ↓            ↓
   Continue      Check 