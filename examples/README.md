# kamiwayland examples

## Prerequisites

- GCC 15+ or Clang 18+ (Clang requires libc++)
- CMake 4.2+
- Ninja

## Build

```sh
cmake -B build -G Ninja
cmake --build build --parallel
```

## Run

```sh
./build/01_list_globals/list_globals
./build/02_shm_window/shm_window
./build/03_bouncy_box/bouncy_box
```

## Examples

| Example                               | Description                                                                                                      |
|---------------------------------------|------------------------------------------------------------------------------------------------------------------|
| [`01_list_globals`](01_list_globals/) | Connect, enumerate every compositor global, print interface name and version.                                    |
| [`02_shm_window`](02_shm_window/)     | SHM buffer, `xdg_toplevel` window, configure/ack handshake, resize, server-side decorations, event loop.         |
| [`03_bouncy_box`](03_bouncy_box/)     | Double-buffered SHM (shared pool, two slots), frame callbacks, keyboard input, pointer motion and button events. |
