cmake_minimum_required(VERSION 3.24...3.30)
project(yellowtail)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/arch:AVX2)
endif()

include(FetchContent)

# --- SDL3 ---
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.10
        GIT_SHALLOW TRUE
        OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(SDL3)

# --- SDL3_image (stb backend, no external image deps) ---
set(SDLIMAGE_VENDORED OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL  OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF  OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3_image
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
        GIT_TAG release-3.4.4
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3_image)

# --- SDL3_ttf (TrueType font rendering, used by the Clay SDL3 renderer for text) ---
set(SDLTTF_VENDORED ON  CACHE BOOL "" FORCE)   # bundle FreeType so no system dep
set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)   # skip text shaping - keeps the build lean
set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)   # skip color-emoji SVG support
set(SDLTTF_SAMPLES  OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3_ttf
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
        GIT_TAG release-3.2.2
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3_ttf)

# --- SDL3_shadercross (HLSL/SPIR-V -> SPIR-V/DXIL/MSL at runtime) ---
# DXC's bundled standalone `clang` executable doesn't build on modern MSVC.
# DXC already excludes it from `all`; CLion Run configs must target
# `yellowtail` (not "Project"/"all") to avoid pulling it in. See README "Build".
set(SDLSHADERCROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_STATIC ON  CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_SPIRVCROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_VENDORED ON  CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_DXC      ON  CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_CLI      OFF CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_INSTALL  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3_shadercross
        GIT_REPOSITORY https://github.com/libsdl-org/SDL_shadercross.git
        GIT_TAG 1d8b0556eefb11a77bc9c28249d16f7a3e0459e9
)
FetchContent_MakeAvailable(SDL3_shadercross)

# --- Dear ImGui (docking branch) with SDL3 + SDL_GPU backends ---
# ImGui ships no CMakeLists.txt for consumers, so we build our own static lib
# from the core sources + the two backend files we actually use.
FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.8-docking
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
)
target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

# --- enet (zpl-c fork) ---
FetchContent_Declare(enet
        GIT_REPOSITORY https://github.com/zpl-c/enet.git
        GIT_TAG v2.6.5
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(enet)

# --- Clay (immediate-mode UI layout - single-header lib + SDL3 renderer .c file) ---
# Clay ships a CMakeLists.txt that ONLY builds examples, no consumer library target.
# We disable all the example switches, fetch the source, and pull clay.h + the SDL3
# renderer into our own TU (see src/clay_impl.c). Renderer functions are `static`,
# so they must be #include'd, not linked. Renderer requires SDL3_ttf and SDL3_image
# (we already have both).
set(CLAY_INCLUDE_ALL_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(CLAY_INCLUDE_DEMOS          OFF CACHE BOOL "" FORCE)
set(CLAY_INCLUDE_CPP_EXAMPLE    OFF CACHE BOOL "" FORCE)
set(CLAY_INCLUDE_RAYLIB_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLAY_INCLUDE_SDL2_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(CLAY_INCLUDE_SDL3_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(CLAY_INCLUDE_WIN32_GDI_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLAY_INCLUDE_SOKOL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLAY_INCLUDE_PLAYDATE_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(clay
        GIT_REPOSITORY https://github.com/nicbarker/clay.git
        GIT_TAG v0.14
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(clay)

# --- GLM (header-only graphics math) ---
FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG 1.0.1
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glm)

# --- Jolt Physics ---
# Jolt's Build/CMakeLists.txt mutates global CMAKE_CXX_FLAGS (adds -fno-rtti,
# -fno-exceptions, /WX, etc.). Save/restore the flag string so the rest of
# yellowtail builds with our own settings.
set(_yt_saved_cxx_flags "${CMAKE_CXX_FLAGS}")

set(OVERRIDE_CXX_FLAGS               OFF CACHE BOOL   "" FORCE)
set(ENABLE_ALL_WARNINGS              OFF CACHE BOOL   "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION     OFF CACHE BOOL   "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY  ON  CACHE BOOL   "" FORCE)
set(DOUBLE_PRECISION                 OFF CACHE BOOL   "" FORCE)
set(OBJECT_LAYER_BITS                16  CACHE STRING "" FORCE)
set(USE_AVX2                         ON  CACHE BOOL   "" FORCE)
set(CPP_RTTI_ENABLED                 OFF CACHE BOOL   "" FORCE)
set(CPP_EXCEPTIONS_ENABLED           OFF CACHE BOOL   "" FORCE)

FetchContent_Declare(JoltPhysics
        GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
        GIT_TAG v5.5.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR Build
)
FetchContent_MakeAvailable(JoltPhysics)

set(CMAKE_CXX_FLAGS "${_yt_saved_cxx_flags}")
unset(_yt_saved_cxx_flags)

# --- Target ---
add_executable(${PROJECT_NAME})
add_subdirectory(src)

target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/external"
        "${JoltPhysics_SOURCE_DIR}/.."
        "${clay_SOURCE_DIR}"
)
target_link_libraries(${PROJECT_NAME} PRIVATE
        SDL3::SDL3-static
        SDL3_image::SDL3_image-static
        SDL3_ttf::SDL3_ttf-static
        SDL3_shadercross::SDL3_shadercross-static
        imgui
        enet::enet_static
        Jolt
        glm::glm
)

# GLM defaults to OpenGL's [-1, 1] clip-space Z; SDL_GPU (Vulkan/D3D12/Metal) wants [0, 1].
# Applying this everywhere keeps perspectiveRH/orthoRH and friends honest.
target_compile_definitions(${PROJECT_NAME} PRIVATE
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${PROJECT_NAME})
set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>)

# --- Copy assets next to the executable (per-config; works in debug and release) ---
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
        COMMENT "Copying assets to output directory"
)

# --- Windows: copy DXC runtime DLLs next to the executable ---
# dxcompiler is always SHARED (DXC forces it). Without our build next to the
# exe, Windows loads System32's dxcompiler.dll by mistake.
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:dxcompiler>"
                    "$<TARGET_FILE:dxildll>"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            COMMENT "Copying dxcompiler.dll + dxil.dll next to executable"
    )
endif()

if (WIN32 AND MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE -mwindows)
endif()
