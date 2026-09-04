# WOD-Internal-Match-Server

Qt/C++ internal match server and broadcast console for a 2v2 infantry robot match.

## Current scope

- UDP robot status monitoring and event logging
- Dual-window operation: broadcast output and control console
- Frameless full-screen broadcast output window
- Qt Widgets and Qt Network based foundation

## Build

This project uses CMake and requires Qt 6 with the `Widgets` and `Network` components.

```text
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<Qt installation>
cmake --build build
```

Runtime deployment can be performed with Qt's `windeployqt` tool after building on Windows.
