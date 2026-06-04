# yellowtail

Cross-platform C++20 starter using SDL3 (windowing / input / audio / 2D + GPU rendering), SDL3_image (texture loading), zpl-c/enet (UDP networking), and cgltf (glTF 2.0 models). All dependencies are fetched and built from source via CMake `FetchContent` — no system packages required. Builds on Windows (MSVC / MinGW), Linux, and macOS from a single `CMakeLists.txt`.

## Build

```sh
cmake -S . -B build
cmake --build build -j
./build/yellowtail/yellowtail
```

The first configure takes a few minutes (it clones and builds SDL3 from source); subsequent configures are fast.

## Project layout

```
yellowtail/
├── CMakeLists.txt        # top-level build
├── src/
│   ├── CMakeLists.txt    # globs *.cpp and *.h
│   ├── main.cpp
│   └── ...               # all C++ source/header files go here
├── external/             # third-party single-header libraries
│   └── cgltf.h
└── assets/               # textures, models, audio, shaders — anything loaded at runtime
```

- **`src/`** — drop new `.cpp` and `.h` files anywhere under here (subdirectories work too). `src/CMakeLists.txt` globs `*.cpp` and `*.h` recursively with `CONFIGURE_DEPENDS`, so no need to edit CMake when adding files. Re-run the build and they're picked up.
- **`external/`** — for single-header / drop-in third-party code (cgltf is here). Added to the include path, so `#include <cgltf.h>` works. Prefer `FetchContent` in `CMakeLists.txt` for anything with its own build system; reserve `external/` for headers you copy into the repo.
- **`assets/`** — runtime resources. Everything under here is copied to `<build>/yellowtail/assets/` after every build (debug *and* release), so the binary always has its assets next to it. Load at runtime via `SDL_GetBasePath()` + `"assets/..."` — see [Loading assets](#loading-assets) below.

## Dependencies

| Library | Version | Purpose | Why this one |
|---|---|---|---|
| [SDL3](https://github.com/libsdl-org/SDL) | `release-3.4.10` | Window, input, audio, 2D + GPU rendering | Modern cross-platform foundation. Static-linked so the binary is self-contained. |
| [SDL3_image](https://github.com/libsdl-org/SDL_image) | `release-3.4.4` | Load PNG / JPG / BMP / etc. as `SDL_Texture` | SDL's officially-recommended texture loader. Configured with the `stb` backend, so no system `libpng`/`libjpeg` required. |
| [zpl-c/enet](https://github.com/zpl-c/enet) | `v2.6.5` | Reliable UDP networking | Modernized fork of lsalzman/enet with a clean CMake target. Ships `ws2_32` linkage on Windows automatically. |
| [cgltf](https://github.com/jkuhlmann/cgltf) | `v1.9` | glTF 2.0 model parsing | Single-header, MIT, zero deps. glTF is the modern interchange format (Blender / Maya / Substance export it natively). |

## Loading assets

Don't load with bare relative paths — the current working directory is unreliable across platforms (CLion sets it to the build dir, double-clicking on macOS sets it to `/`, Windows shortcuts can set it anywhere). Always anchor paths to the executable using `SDL_GetBasePath()`:

```cpp
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

const char* base = SDL_GetBasePath();          // dir of the executable; do NOT free in SDL3
std::string path = std::string(base) + "assets/cube.png";
SDL_Texture* tex = IMG_LoadTexture(renderer, path.c_str());
```

`SDL_GetBasePath()` returns the right directory on Windows (next to `.exe`), Linux (next to ELF), macOS plain binary, *and* macOS `.app` bundles (`Contents/Resources/`) — same code, no `#ifdef`s.

For glTF models, include cgltf once with the implementation macro:

```cpp
// in exactly one .cpp file:
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

cgltf_options opts{};
cgltf_data* data = nullptr;
cgltf_parse_file(&opts, path.c_str(), &data);
cgltf_load_buffers(&opts, data, path.c_str());
// ... walk data->meshes, upload to GPU ...
cgltf_free(data);
```

## CMake notes

A few non-obvious decisions in `CMakeLists.txt`:

- **Static linking everything** (`SDL_SHARED=OFF`, `SDL_STATIC=ON`, `BUILD_SHARED_LIBS=OFF`) — produces a single self-contained executable per platform. No DLL/`.dylib`/`.so` hunting at runtime.
- **`OVERRIDE_FIND_PACKAGE` on SDL3** — required so that SDL3_image's internal `find_package(SDL3)` resolves to our FetchContent build instead of looking for a system install. Without it, configure fails with "Could not find SDL3".
- **`SDLIMAGE_VENDORED=OFF` + `SDLIMAGE_BACKEND_STB=ON`** — uses SDL_image's bundled `stb_image` for PNG/JPG decoding instead of building vendored copies of libpng/libjpeg/libwebp/libavif. Faster build, fewer transitive deps, covers every format we care about. AVIF/JXL/TIF/WebP are disabled explicitly for the same reason.
- **No manual `-framework Cocoa` / `IOKit` / etc. on macOS** — SDL3's static target already propagates every macOS framework it needs via its `INTERFACE_LINK_LIBRARIES` (see SDL3's `sdl_link_dependency(...)` calls). Linking `SDL3::SDL3-static` is enough.
- **No manual `ws2_32` / `winmm` on Windows** — same reason: enet adds `ws2_32` and SDL3 adds `winmm` via their public link interfaces.
- **`MultiThreaded$<$<CONFIG:Debug>:Debug>`** for `CMAKE_MSVC_RUNTIME_LIBRARY` — statically links the CRT on MSVC. Note: appending `$<$<CONFIG:Release>:Release>` (a common copy-paste pattern) produces the invalid value `MultiThreadedRelease` in Release builds and silently falls back to the DLL runtime.
- **`/arch:AVX2`** on MSVC — assumes a reasonably modern CPU. Drop this if you need to target older hardware.
- **`add_custom_command(... POST_BUILD copy_directory assets ...)`** — runs every build, into `$<TARGET_FILE_DIR:${PROJECT_NAME}>` which resolves per-configuration. Means `cmake-build-debug/yellowtail/assets/` and `cmake-build-release/yellowtail/assets/` both stay in sync automatically.
- **`file(GLOB_RECURSE ... CONFIGURE_DEPENDS)`** in `src/CMakeLists.txt` — the `CONFIGURE_DEPENDS` flag makes CMake re-run globbing on every build, so new files appear without manually re-running `cmake -S . -B build`. Slight build-time cost; worth it for ergonomics.
