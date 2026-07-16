# yellowtail

Cross-platform SDL3 project. Used as a playground and to learn to code games without an engine as well as learn more about rendering and C++. All dependencies are fetched and built from source via CMake `FetchContent`, so no system packages are required. Builds on Windows (MSVC), Linux, and macOS from a single `CMakeLists.txt`.

## Build

**Windows: use the MSVC toolchain, not MinGW.** DirectX Shader Compiler (pulled in transitively by SDL_shadercross) has no working MinGW build path. Its `MSSupport` module relies on `<windows.h>` types being force-included through MSVC's PCH machinery, and its non-Windows shim (`dxc/WinAdapter.h`) is gated on `!_MSC_VER`, not `!_WIN32`. A MinGW build gets neither and fails with hundreds of "`BOOL` / `HANDLE` / `HRESULT` does not name a type" errors. In CLion: **Settings, Build, Execution, Deployment, Toolchains, + Visual Studio** (needs Visual Studio Build Tools or full VS installed), move it to the top so it's the default, then in **Settings, Build, Execution, Deployment, CMake** delete existing profiles and add fresh Debug/Release profiles using the Visual Studio toolchain. Blow away `cmake-build-*/` before reconfiguring.

Open the project in CLion - it'll configure into `cmake-build-debug/` and `cmake-build-release/` automatically. Build/run via the toolbar, or from a terminal:

The build produces two executables that share a common `engine` static library: `ytail_game`
(the game) and `ytail_editor` (the editor host). Build and run whichever you want:

```sh
cmake --build cmake-build-debug --target ytail_game -j
./cmake-build-debug/ytail_game/ytail_game
# or the editor:
cmake --build cmake-build-debug --target ytail_editor -j
./cmake-build-debug/ytail_editor/ytail_editor
```

The first clean configure takes **5 to 15 minutes**. It clones and builds SDL3, plus DXC (DirectX Shader Compiler, a fork of LLVM/Clang pulled in by SDL_shadercross) and SPIRV-Cross from source. Subsequent incremental builds are fast because everything is cached under `cmake-build-debug/_deps/`. **Avoid deleting the build dir** unless you actually need to reconfigure from scratch.

**One CLion pitfall on Windows.** DXC ships a standalone `clang` executable target (`tools/clang/tools/driver`) whose `cc1_main.cpp` / `cc1as_main.cpp` call stale `llvm::opt` / `CompilerInstance` APIs. It will not compile against modern MSVC. Yellowtail never links `clang`, and DXC's own CMake marks it `EXCLUDE_FROM_ALL`, but CLion's "Build Project" (`Ctrl+F9`) runs `ninja all`, which re-includes it. Make sure the Run/Debug configuration's **Before launch** step builds a specific executable target (`ytail_game` or `ytail_editor`), not "Project":

1. Toolbar, **Edit Configurations...**
2. Select the run config, then the **Before launch** panel at the bottom.
3. Remove any "Build 'Project'" entry; add **Build** with target = `ytail_game` (or `ytail_editor`).

Then the play button only builds that target's dependency chain (`engine`, `dxcompiler`, `dxildll`, SPIRV-Cross, etc.) and never touches the broken standalone `clang`.

## Project layout

```
yellowtail/
├── CMakeLists.txt        # top-level: fetches deps, then add_subdirectory(src)
├── src/
│   ├── CMakeLists.txt    # engine static lib + ytail_game / ytail_editor exes
│   ├── engine/           # engine runtime -> static `engine` library
│   ├── game/main.cpp     # ytail_game entry point
│   └── editor/main.cpp   # ytail_editor entry point
├── external/             # third-party single-header libraries
│   └── cgltf.h
└── assets/               # textures, models, audio, shaders - anything loaded at runtime
```

- **`src/engine/`** - compiled into the `engine` static library, shared by both executables (UI code lives under `engine/ui/`). `src/CMakeLists.txt` globs `engine/*.cpp`/`*.c`/`*.h` recursively with `CONFIGURE_DEPENDS`, so new engine files are picked up without editing CMake. Drop new engine code under here.
- **`src/game/`, `src/editor/`** - the two executables that link `engine`, each globbed recursively (same as `engine/`), so new files under either dir are picked up without editing CMake. Both currently just run the engine; the editor becomes a distinct host once the engine exposes a game seam.
- **`external/`** - for single-header / drop-in third-party code (cgltf is here). Added to the include path, so `#include <cgltf.h>` works. Prefer `FetchContent` in `CMakeLists.txt` for anything with its own build system; reserve `external/` for headers you copy into the repo.
- **`assets/`** - runtime resources. Everything under here is copied next to *each* executable (`<build>/ytail_game/assets/`, `<build>/ytail_editor/assets/`) after every build (debug *and* release), so each binary always has its assets beside it. Load at runtime via `SDL_GetBasePath()` + `"assets/..."` - see [Loading assets](#loading-assets) below.

## Dependencies

| Library | Version | Purpose | Why this one |
|---|---|---|---|
| [SDL3](https://github.com/libsdl-org/SDL) | `release-3.4.10` | Window, input, audio, 2D + GPU rendering | Modern cross-platform foundation. Static-linked so the binary is self-contained. |
| [SDL3_image](https://github.com/libsdl-org/SDL_image) | `release-3.4.4` | Load PNG / JPG / BMP / etc. as `SDL_Texture` | SDL's officially-recommended texture loader. Configured with the `stb` backend, so no system `libpng`/`libjpeg` required. |
| [SDL3_ttf](https://github.com/libsdl-org/SDL_ttf) | `release-3.2.2` | TrueType font rasterization | Linked and ready for whichever UI path renders text (a Clay-with-SDL_Renderer overlay, or a custom SDL_GPU renderer that consumes glyph atlases). Vendored FreeType, HarfBuzz/PlutoSVG disabled to keep the build lean. |
| [SDL_shadercross](https://github.com/libsdl-org/SDL_shadercross) | `main` (pinned SHA) | Translate HLSL or SPIR-V into SPIR-V / DXIL / MSL at runtime for `SDL_gpu` | Lets you write shaders once in HLSL and have them work on Vulkan, D3D12, and Metal without an offline build step. Vendors DXC + SPIRV-Cross from source. On Windows, DXC forces `dxcompiler` to be `SHARED`, so `dxcompiler.dll` + `dxil.dll` ship next to `yellowtail.exe`. macOS/Linux stay fully static. No tagged releases yet, hence pinning a commit. |
| [Dear ImGui](https://github.com/ocornut/imgui) | `v1.92.8-docking` | Immediate-mode debug UI (overlays, dev tools, editors) | The standard for in-game debug UI. Docking branch adds dockable / detachable windows. Built with the `imgui_impl_sdl3` + `imgui_impl_sdlgpu3` backends so it draws through the same `SDL_gpu` device as the rest of the scene. |
| [Clay](https://github.com/nicbarker/clay) | `v0.14` | Immediate-mode UI layout (game UI / HUDs / menus) | Layout-only library (~3K LOC, single header) with a declarative `CLAY({ ... })` tree API. **Renderer not currently wired.** Clay's bundled `clay_renderer_SDL3.c` targets `SDL_Renderer` and uses C99 VLAs that MSVC's C compiler doesn't support. See "Clay UI" below for the three integration options. |
| [zpl-c/enet](https://github.com/zpl-c/enet) | `v2.6.5` | Reliable UDP networking | Modernized fork of lsalzman/enet with a clean CMake target. Ships `ws2_32` linkage on Windows automatically. |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | `v5.5.0` | Rigid-body physics (collision, constraints, vehicles, ragdolls) | Modern, fast, multithreaded, used in Horizon Forbidden West. Header-light public API, no exceptions/RTTI, single static lib (`Jolt`). Consumed via `SOURCE_SUBDIR Build` per upstream's integration guide. |
| [GLM](https://github.com/g-truc/glm) | `1.0.1` | Graphics math (vec/mat/quat, projection helpers) | Header-only, GLSL-style API - most online graphics tutorials and Vulkan/SDL_GPU samples use it. Despite the "GL" name, it's just C++ linear algebra with no runtime dependency on OpenGL; we use it with `SDL_GPU`. Build defines `GLM_FORCE_DEPTH_ZERO_TO_ONE` (Vulkan/D3D12/Metal clip-space Z) and `GLM_ENABLE_EXPERIMENTAL` (for `glm/gtx/*` headers tutorials reach for). |
| [cgltf](https://github.com/jkuhlmann/cgltf) | `v1.9` | glTF 2.0 model parsing | Single-header, MIT, zero deps. glTF is the modern interchange format (Blender / Maya / Substance export it natively). |

## Helpful References/Resources
- https://gdcvault.com/play/1027891/Architecting-Jolt-Physics-for-Horizon
- https://github.com/ocornut/imgui/blob/master/examples/example_sdl3_sdlgpu3/main.cpp
- https://www.gafferongames.com/post/fix_your_timestep/
- https://www.opengl-tutorial.org/miscellaneous/clicking-on-objects/picking-with-custom-ray-obb-function/
- https://webgpufundamentals.org/webgpu/lessons/webgpu-picking.html

## Shaders (SDL_gpu via SDL_shadercross)

Some info on shadercross: https://moonside.games/posts/introducing-sdl-shadercross/

**HLSL authoring rules and the D3D12 interstage-signature failure mode are in [docs/shaders.md](docs/shaders.md).** Read that before writing new shaders.

`SDL_gpu` is built into SDL3 itself - no extra dependency for the GPU API. But each backend wants a different shader format (Vulkan→SPIR-V, D3D12→DXIL, Metal→MSL). SDL_shadercross translates from HLSL or SPIR-V into whatever the active backend needs at runtime:

```cpp
#include <SDL3_shadercross/SDL_shadercross.h>

// load assets/shaders/triangle.hlsl, pass to:
SDL_ShaderCross_HLSL_Info info{ /* source, entrypoint, stage, ... */ };
SDL_GPUShader* shader = SDL_ShaderCross_CompileGraphicsShaderFromHLSL(device, &info, ...);
```

This is the "pattern #1" workflow - fastest iteration loop, edit HLSL and re-run. The trade-off is binary size (DXC adds tens of MB) and the slow first-time build. Two alternative workflows you can migrate to later:

- **Pattern #3 (hybrid)**: precompile HLSL → SPIR-V offline using the `shadercross` CLI, then translate SPIR-V → DXIL/MSL at runtime via SPIRV-Cross. Drops the DXC runtime dep. Flip `SDLSHADERCROSS_DXC=OFF` and `SDLSHADERCROSS_CLI=ON` in `CMakeLists.txt`.
- **Pattern #2 (full offline)**: precompile to all three formats offline, pick at runtime with `SDL_GetGPUShaderFormats(device)`. No runtime translator dep. Most work, smallest binary.

## CMake notes

A few non-obvious decisions in `CMakeLists.txt`:

- **Static linking everything** (`SDL_SHARED=OFF`, `SDL_STATIC=ON`, `BUILD_SHARED_LIBS=OFF`) produces a single self-contained executable on macOS/Linux. **Windows exception:** DXC's `tools/clang/tools/dxcompiler/CMakeLists.txt:127` defines `dxcompiler` as `add_clang_library(dxcompiler SHARED ...)` unconditionally, so `dxcompiler.dll` and its runtime companion `dxil.dll` are always DLLs on Windows. Yellowtail imports `dxcompiler.dll` by name, so a POST_BUILD step copies both DLLs alongside `yellowtail.exe`. **This is not optional.** Windows ships its own `dxcompiler.dll` in `System32` that lacks SPIR-V codegen, and it will be loaded instead if our built DLL isn't next to the exe. The "SPIR-V CodeGen not available" runtime error is the symptom.
- **`OVERRIDE_FIND_PACKAGE` on SDL3** - required so that SDL3_image's and SDL_shadercross's internal `find_package(SDL3)` calls resolve to our FetchContent build instead of looking for a system install. Without it, configure fails with "Could not find SDL3".
- **`SDLSHADERCROSS_VENDORED=ON`, `SDLSHADERCROSS_CLI=OFF`, `SDLSHADERCROSS_DXC=ON`** bundle SPIRV-Cross and DXC (we don't want to depend on system-installed shader compilers), skip building the offline `shadercross` CLI (we don't use it in pattern #1), and keep DXC enabled so HLSL works at runtime. `SDLSHADERCROSS_SPIRVCROSS_SHARED=OFF` matches our mostly-static policy. DXC's broken standalone `clang` executable (`tools/clang/tools/driver`) is left excluded by DXC's own `HLSL_OPTIONAL_PROJS_IN_DEFAULT=OFF` default; the CLion Run-config guidance in "Build" above keeps `ninja all` from pulling it in anyway.
- **SDL_shadercross is pinned to a `main` commit SHA, not a tag** - the project has no tagged releases yet. Bump the SHA manually when you want updates.
- **Dear ImGui is built as a hand-rolled `add_library(imgui STATIC ...)`** - ImGui upstream deliberately doesn't ship a CMakeLists.txt. We pick exactly the backends we use (`imgui_impl_sdl3` + `imgui_impl_sdlgpu3`), so the lib only carries what we link. To add another backend, append the `.cpp` to the source list in the ImGui block.
- **Clay ships a CMakeLists.txt that builds examples, not a consumer library.** Its commented-out `add_library(clay INTERFACE)` line at the bottom is the giveaway. We set `CLAY_INCLUDE_ALL_EXAMPLES=OFF` (plus every per-backend toggle) before `FetchContent_MakeAvailable(clay)` to suppress example targets, then add `${clay_SOURCE_DIR}` to our include path manually and compile `src/engine/ui/clay/clay_impl.c` (which defines `CLAY_IMPLEMENTATION` and includes `<clay.h>`). We do not include Clay's bundled SDL3 renderer; see "Clay UI" for why. `clay_impl.c` stays a `.c` file for now to keep the door open for later including a C99 renderer; `src/CMakeLists.txt` globs `engine/*.cpp` and `engine/*.c` so it's picked up regardless.
- **Jolt's `Build/CMakeLists.txt` mutates `CMAKE_CXX_FLAGS` globally** (`-fno-rtti`, `-fno-exceptions`, `-Wall -Werror`, MSVC equivalents). We save `CMAKE_CXX_FLAGS` before `FetchContent_MakeAvailable(JoltPhysics)` and restore it after, so the rest of yellowtail compiles with its normal flags. We also set `OVERRIDE_CXX_FLAGS=OFF`, `ENABLE_ALL_WARNINGS=OFF`, and `INTERPROCEDURAL_OPTIMIZATION=OFF` to keep Jolt's own build well-behaved. Don't drop these or reorder the block without moving the save/restore with it.
- **`SDLIMAGE_VENDORED=OFF` + `SDLIMAGE_BACKEND_STB=ON`** - uses SDL_image's bundled `stb_image` for PNG/JPG decoding instead of building vendored copies of libpng/libjpeg/libwebp/libavif. Faster build, fewer transitive deps, covers every format we care about. AVIF/JXL/TIF/WebP are disabled explicitly for the same reason.
- **No manual `-framework Cocoa` / `IOKit` / etc. on macOS** - SDL3's static target already propagates every macOS framework it needs via its `INTERFACE_LINK_LIBRARIES` (see SDL3's `sdl_link_dependency(...)` calls). Linking `SDL3::SDL3-static` is enough.
- **No manual `ws2_32` / `winmm` on Windows** - same reason: enet adds `ws2_32` and SDL3 adds `winmm` via their public link interfaces.
- **`MultiThreaded$<$<CONFIG:Debug>:Debug>`** for `CMAKE_MSVC_RUNTIME_LIBRARY` - statically links the CRT on MSVC. Note: appending `$<$<CONFIG:Release>:Release>` (a common copy-paste pattern) produces the invalid value `MultiThreadedRelease` in Release builds and silently falls back to the DLL runtime.
- **`/arch:AVX2`** on MSVC - assumes a reasonably modern CPU. Drop this if you need to target older hardware.
- **`add_custom_command(... POST_BUILD copy_directory assets ...)`** - runs every build, into `$<TARGET_FILE_DIR:${target}>` which resolves per-configuration. Applied to each executable via the `yt_configure_app()` helper, so e.g. `cmake-build-debug/ytail_game/assets/` and `cmake-build-release/ytail_game/assets/` (and the `ytail_editor` equivalents) stay in sync automatically.
- **`file(GLOB_RECURSE ... CONFIGURE_DEPENDS)`** in `src/CMakeLists.txt` - the `CONFIGURE_DEPENDS` flag makes CMake re-run globbing on every build, so new files appear without manually re-running `cmake -S . -B build`. Slight build-time cost; worth it for ergonomics.
