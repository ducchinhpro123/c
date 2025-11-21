# File Transfer Quick Reference Guide

## TL;DR - How It Works

```
Sender Client → Server → Receiver Client(s)

1. User drops file
2. Send FILE_META (filename, size, etc.)
3. Send FILE_CHUNK x N (base64 encoded data)
4. Send FILE_END (completion signal)
5. Receivers save to received/ folder
```

---

## Key Functions Reference

### Sender Side (client_gui.c)

| Function | Line | Purpose |
|----------|------|---------|
| `process_file_drop()` | 763 | Detects dropped files |
| `start_outgoing_transfer()` | 704 | Initializes transfer |
| `pump_outgoing_transfers()` | 775 | Sends chunks every frame |
| `close_outgoing_transfer()` | 421 | Cleanup on error/completion |

### Receiver Side (client_gui.c)

| Function | Line | Purpose |
|----------|------|---------|
| `process_incoming_stream()` | 653 | Buffers incoming data |
| `handle_file_packet()` | 534 | Parses FILE_* messages |
| `finalize_incoming_transfer()` | 449 | Cleanup & save file |

### Server Side (server.c)

| Function | Line | Purpose |
|----------|------|---------|
| `process_file_message()` | 452 | Validates file packets |
| `file_session_create()` | 402 | Track transfer state |
| `server_broadcast_msg()` | 580 | Forward to all clients |

### Network Layer (client_network.c, server.c)

| Function | Line | Purpose |
|----------|------|---------|
| `send_msg()` | 191 | Send with newline |
| `send_all()` | 122 | Handle partial sends |
| `recv_msg()` | 238 | Receive from socket |

---

## Protocol Message Examples

### FILE_META
```
FILE_META|alice|3fa8b1c2d4e5|photo.jpg|2048576|48000\n

Fields:
  1. Type: FILE_META
  2. Sender: alice
  3. File ID: 3fa8b1c2d4e5 (unique random ID)
  4. Filename: photo.jpg
  5. Total bytes: 2048576 (2 MB)
  6. Chunk size: 48000
```

### FILE_CHUNK
```
FILE_CHUNK|alice|3fa8b1c2d4e5|0|iVBORw0KGgoAAAANS...base64data...\n

Fields:
  1. Type: FILE_CHUNK
  2. Sender: alice
  3. File ID: 3fa8b1c2d4e5
  4. Chunk index: 0 (sequential)
  5. Payload: base64 encoded binary data
```

### FILE_END
```
FILE_END|alice|3fa8b1c2d4e5\n

Fields:
  1. Type: FILE_END
  2. Sender: alice
  3. File ID: 3fa8b1c2d4e5
```

### FILE_ABORT
```
FILE_ABORT|alice|3fa8b1c2d4e5|network error\n

Fields:
  1. Type: FILE_ABORT
  2. Sender: alice
  3. File ID: 3fa8b1c2d4e5
  4. Reason: network error
```

---

## State Machine Diagram

### Sender State Machine

```
[IDLE] ──────────────────► [ACTIVE]
  ▲                           │
  │                           │ meta_sent = false
  │                           ↓
  │                       [SENDING_META]
  │                           │
  │                           │ meta_sent = true
  │                           ↓
  │                       [SENDING_CHUNKS]
  │                           │
  │                           │ sent_bytes >= total_bytes
  │                           ↓
  │                       [SENDING_END]
  │                           │
  └───────────────────────────┘
          (cleanup)
```

### Receiver State Machine

```
[IDLE] ──────────────────► [WAITING_META]
  ▲                           │
  │                           │ Receive FILE_META
  │                           ↓
  │                       [RECEIVING_CHUNKS]
  │                           │
  │                           │ Receive FILE_END
  │                           ↓
  │                       [VALIDATING]
  │                           │
  │                           ├─► Success: Save file
  │                           └─► Failure: Delete file
  │                           │
  └───────────────────────────┘
          (cleanup)
```

---

## Timing & Performance

### Frame-Based Processing
```c
// Every frame (60 FPS = ~16ms per frame)
pump_outgoing_transfers() {
    Time budget: 8ms          // Don't block UI
    Max chunks: 64            // Per frame
    Per chunk: ~48 KB         // Binary data
    
    Theoretical max: 64 × 48KB × 60 FPS = 184 MB/s
    Real-world LAN: ~50-100 MB/s
}
```

### Retry Logic
```c
send_all() {
    Max retries: 100 (small packets) to 10000 (large packets)
    Base wait: 2-100ms (depends on packet size)
    Backoff: Exponential (2x every 100 retries)
    Max wait: 1000ms (1 second)
}
```

---

## Configuration Constants

### file_transfer.h
```c
#define FILE_TRANSFER_MAX_SIZE (500LL * 1024 * 1024)  // 500 MB
#define FILE_CHUNK_SIZE 48000                          // 48 KB
#define FILE_ID_LEN 32                                 // Hex string
#define FILE_NAME_MAX_LEN 256
#define FILE_PATH_MAX_LEN 512
#define FILE_CHUNK_ENCODED_SIZE 64000                  // ~48KB * 1.33
```

### client_gui.c
```c
#define MAX_ACTIVE_TRANSFERS 8           // Per client
#define MAX_CHUNKS_PER_BATCH 64          // Per frame
#define TRANSFER_PUMP_BUDGET_MS 8.0f     // 8ms max per frame
#define INCOMING_STREAM_CAPACITY 131072  // 128 KB buffer
```

### server.c
```c
#define MAX_FILE_SESSIONS 32             // Server-wide tracking
```

---

## Common Debugging Scenarios

### Problem: File not sending

**Check:**
1. Is `OutgoingTransfer.active == true`?
2. Is `send_msg()` returning > 0?
3. Check console for "Sending FILE_META" log
4. Verify file readable: `fp != NULL`

**Code to add:**
```c
TraceLog(LOG_INFO, "Transfer state: active=%d, meta_sent=%d, sent=%zu/%zu",
         transfer->active, transfer->meta_sent, 
         transfer->sent_bytes, transfer->total_bytes);
```

### Problem: Receiver not getting file

**Check:**
1. Is server broadcasting? Check server logs
2. Is `IncomingTransfer.active == true`?
3. Check "FILE_META received" log on receiver
4. Verify `received/` directory exists and writable

**Code to add:**
```c
TraceLog(LOG_INFO, "Incoming stream: len=%zu, looking for newline", 
         incoming_stream_len);
```

### Problem: Transfer stalls mid-way

**Check:**
1. Network buffer full? Look for "Socket buffer full" logs
2. Check `send_all()` retry count
3. Verify both sides still connected
4. Check if time budget exceeded

**Code to add:**
```c
TraceLog(LOG_WARNING, "Chunk %zu/%zu took too long", 
         transfer->next_chunk_index, transfer->chunks_total);
```

---

## Modification Recipes

### Recipe 1: Add transfer speed indicator

```c
// In OutgoingTransfer struct, add:
double last_update_time;
size_t last_sent_bytes;

// In pump_outgoing_transfers(), after sending chunks:
double now = GetTime();
if (now - transfer->last_update_time >= 1.0) {  // Every second
    size_t delta = transfer->sent_bytes - transfer->last_sent_bytes;
    double speed_mbps = (delta / 1024.0 / 1024.0);
    TraceLog(LOG_INFO, "Speed: %.2f MB/s", speed_mbps);
    
    transfer->last_update_time = now;
    transfer->last_sent_bytes = transfer->sent_bytes;
}
```

### Recipe 2: Add file type filtering

```c
// In start_outgoing_transfer(), add:
const char* allowed_extensions[] = {".txt", ".jpg", ".png", ".pdf", NULL};
const char* ext = strrchr(file_path, '.');
bool allowed = false;

for (int i = 0; allowed_extensions[i]; i++) {
    if (ext && strcasecmp(ext, allowed_extensions[i]) == 0) {
        allowed = true;
        break;
    }
}

if (!allowed) {
    show_error("File type not allowed");
    return;
}
```

### Recipe 3: Add pause/resume capability

```c
// In OutgoingTransfer struct, add:
bool paused;

// In pump_outgoing_transfers(), add:
if (transfer->paused) {
    continue;  // Skip this transfer
}

// Add UI button:
if (GuiButton((Rectangle){x, y, 80, 20}, "Pause")) {
    outgoing_transfers[i].paused = !outgoing_transfers[i].paused;
}
```

---

## Base64 Encoding Quick Reference

### Why Base64?
- Binary data contains control characters (`\n`, `\0`, etc.)
- Protocol uses `\n` as message delimiter
- Base64 converts binary → safe ASCII text

### Size Impact
```
Original:  48,000 bytes binary
Encoded:   64,000 bytes ASCII (33% overhead)
Formula:   encoded_size = ((input + 2) / 3) * 4
```

### Character Set
```
A-Z (26) + a-z (26) + 0-9 (10) + + (1) + / (1) = 64 characters
Padding: = (when needed)
```

---

## Memory Layout

### Per Active Transfer
```
OutgoingTransfer: 
  - Struct: ~600 bytes
  - File buffer: FILE* (system managed)
  - chunk_buffer: 48,000 bytes
  - encoded_buffer: 64,000 bytes
  - message_buffer: 65,536 bytes
  Total: ~178 KB per transfer

IncomingTransfer:
  - Struct: ~800 bytes
  - File buffer: FILE* (system managed)
  - decoded_buffer: 48,000 bytes
  Total: ~49 KB per transfer
```

### Total Memory (Max Load)
```
8 outgoing × 178 KB = 1.4 MB
8 incoming × 49 KB = 392 KB
Stream buffer: 128 KB
Message queues: ~200 KB
Total: ~2.1 MB per client
```

---

## Security Checklist

- [x] Filename sanitization (removes `../`, `:`, etc.)
- [x] File size limits (500 MB max)
- [x] Chunk size validation
- [x] Total bytes validation (can't exceed declared size)
- [x] Session tracking (server validates sender)
- [x] Auto-rename on collision (file(1), file(2))
- [ ] TODO: Add SHA256 checksum validation
- [ ] TODO: Add encryption for file data
- [ ] TODO: Add user authentication

---

## Testing Checklist

### Basic Tests
- [ ] Send small file (< 1 KB)
- [ ] Send medium file (1-10 MB)
- [ ] Send large file (100-500 MB)
- [ ] Send to multiple receivers simultaneously
- [ ] Send multiple files concurrently

### Edge Cases
- [ ] Filename with spaces
- [ ] Filename with special chars (unicode)
- [ ] Duplicate filenames (auto-rename)
- [ ] Disconnect sender mid-transfer
- [ ] Disconnect receiver mid-transfer
- [ ] Network congestion (simulate with tc/netem)
- [ ] File exactly at size limit (500 MB)

### Error Handling
- [ ] Send file larger than 500 MB
- [ ] Read-only received/ directory
- [ ] Disk full during receive
- [ ] Corrupted chunk data
- [ ] Missing FILE_END packet

---

## Performance Tuning

### To Increase Speed
1. Increase `FILE_CHUNK_SIZE` (but < MSG_BUFFER)
2. Increase `MAX_CHUNKS_PER_BATCH`
3. Increase socket buffers (SO_SNDBUF, SO_RCVBUF)
4. Decrease `base_wait_ms` in send_all()

### To Reduce UI Lag
1. Decrease `TRANSFER_PUMP_BUDGET_MS`
2. Decrease `MAX_CHUNKS_PER_BATCH`
3. Add usleep() between chunks

### To Reduce 