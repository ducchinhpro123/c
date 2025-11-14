```
  [Server App]          [Client1]          [Client2]          [Client3]
       |                     |                   |                   |
       |-- LISTEN:8898       |                   |                   |
       |                     |                   |                   |
       |                  CONNECT                |                   |
       |<------------------- |                   |                   |
       |                     |                   |                   |
       |                  "Alice"                |                   |
       |<------------------- |                   |                   |
       |                     |                   |                   |
       |                     |           CONNECT |                   |
       |<--------------------------------------  |                   |
       |                     |                   |                   |
       |                     |                   |           "Bob"   |
       |<--------------------------------------  |-------------------|
       |                     |                   |                   |
       |  [Message Flow]     |                   |                   |
       |  "Hi everyone!"     |                   |                   |
       |<------------------- |                   |                   |
       |----broadcast------> |------------------>|                   |
       |-------------------- |-------------------|------------------>|
       |                     |                   |                   |
```

Headless server
- Build: `make server` (creates `./server`)
- Run: `./server` (listens on port 8898)
- Stop: Ctrl+C

GUI builds (existing)
- Build client GUI: `make client_gui`
- Run GUI client: `./client_gui`

## File transfer workflow

- Connect the GUI client, then drag & drop any file (up to 500 MB) onto the window to start uploading it to every connected peer.
- The sender streams the file in 45 KB chunks over the existing TCP socket; a status card at the bottom of the UI shows progress and allows you to monitor multiple concurrent uploads.
- Each receiver validates metadata from the server, writes the data incrementally, and saves the completed file under the `received/` folder (unique names are chosen to avoid overwriting).
- Transfers automatically abort if the sender disconnects or exceeds declared limits; the chat log shows `SYSTEM` messages for every start, completion, or failure so you can verify what happened.

