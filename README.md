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

### Build

```console
$ cc -o nob nob.c # ONLY ONCE!!!
$ ./nob
```

#### Build Windows `.exe` (from Linux)

Requires MinGW-w64 cross compiler (`x86_64-w64-mingw32-gcc`).

```console
$ ./nob win
```

Artifacts:
- `build/client_gui.exe`
- `build/server.exe`
- `build/raylib.dll` (copied automatically; keep next to the exe when running on Windows)

#### Run Client GUI

```console
$ ./build/client_gui
```

#### Run Server

```console
$ ./build/server
```


