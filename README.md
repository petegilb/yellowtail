# yellowtail

Cross-platform C++20 starter using SDL3 (windowing/input/audio/rendering) and zpl-c/enet (UDP networking). Dependencies are fetched via CMake `FetchContent` — no system packages required.

## Build

```sh
cmake -S . -B build
cmake --build build -j
./build/yellowtail/yellowtail
```

## Dependencies

- [SDL3](https://github.com/libsdl-org/SDL) `release-3.4.10` (static)
- [zpl-c/enet](https://github.com/zpl-c/enet) `v2.6.5`
