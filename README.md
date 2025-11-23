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

#### Run Client GUI

```console
$ ./build/client_gui
```

#### Run Server

```console
$ ./build/server
```


