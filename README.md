# Relay

Relay is a small desktop chat and file-transfer app for trusted local networks. A lightweight C server relays framed TCP packets between raylib clients; recipients explicitly approve files before anything is written to disk.

## What it does

- LAN chat with server-authored sender identities
- Drag-and-drop file transfer up to 500 MB per file
- Explicit accept/reject flow and exclusive file creation
- Linux builds and Windows cross-builds from Linux
- Bounded packet sizes, queues, connection counts, and transfer slots

## Security model

Relay is intended for a **trusted LAN**. Traffic is not encrypted and there is no password or cryptographic peer authentication, so do not expose port `8898` to the public internet. The implementation rejects malformed/oversized packets, validates usernames and metadata, uses OS-provided secure randomness for transfer IDs, and sanitizes received filenames.

For use across an untrusted network, run Relay through a trusted VPN or add authenticated TLS before deployment.

## Build

The repository vendors raylib for 64-bit Linux and Windows. A C compiler and the normal Linux X11 development/runtime libraries are required.

```console
cc -o nob nob.c       # bootstrap once
./nob                 # build client and server
./nob test            # build and run the test suite
```

Run the apps in separate terminals:

```console
./nob server
./nob run
```

The server listens on all local interfaces at TCP port `8898`. The client defaults to `127.0.0.1:8898` and also accepts hostnames.

## Using Relay

1. Start `./nob server` on one machine in the LAN.
2. Start `./nob run` on each participant's machine.
3. Enter a display name and the server machine's LAN address, then connect.
4. Type a message and press Enter, or drag a regular file anywhere onto the client window.
5. Recipients choose whether to accept the file and where it should be saved.

Controls:

- `Enter` sends the current message.
- `Shift+Enter` inserts a line break.
- `F3` toggles the FPS diagnostic overlay.
- Closing the window safely stops queued sends and active transfers.

### Windows cross-build

Install a MinGW-w64 toolchain, then run:

```console
./nob win
```

The resulting `client_gui.exe`, `server.exe`, and runtime DLLs are placed in `build/`.

## Project layout

```text
src/client_gui.c           application loop and transfer pump
src/ui_components.c       raylib/raygui interface
src/client_network.c       connection and sender-thread lifecycle
src/client_logic.c         framed-packet parser and handlers
src/server.c               relay server and protocol enforcement
src/protocol.h             shared wire limits and validation
src/file_transfer_state.c  incoming/outgoing transfer lifecycle
src/test/                  Unity tests and throughput benchmark
```

Received files are stored in `received/` by default. That directory and generated build artifacts are intentionally ignored by Git.

## Verification

The test suite covers stream backpressure, malformed packet rejection, file metadata and size handling, filename sanitization, secure transfer-ID formatting, queue lifecycle, concurrency, and throughput. Run it before submitting changes:

```console
./nob test
```
