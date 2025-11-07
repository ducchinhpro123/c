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
- Build server GUI: `make server_gui_build` or `make server_gui`
- Build client GUI: `make client`
- Run GUI client: `./client_gui`

