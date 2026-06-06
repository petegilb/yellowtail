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

# --- SDL3_shadercross (HLSL/SPIR-V -> SPIR-V/DXIL/MSL at runtime) ---
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
)
target_link_libraries(${PROJECT_NAME} PRIVATE
        SDL3::SDL3-static
        SDL3_image::SDL3_image-static
        SDL3_shadercross::SDL3_shadercross-static
        imgui
        enet::enet_static
        Jolt
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

if (WIN32 AND MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE -mwindows)
endif()
