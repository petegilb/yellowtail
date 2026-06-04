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

# --- enet (zpl-c fork) ---
FetchContent_Declare(enet
        GIT_REPOSITORY https://github.com/zpl-c/enet.git
        GIT_TAG v2.6.5
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(enet)

# --- Target ---
add_executable(${PROJECT_NAME})
add_subdirectory(src)

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external")
target_link_libraries(${PROJECT_NAME} PRIVATE
        SDL3::SDL3-static
        SDL3_image::SDL3_image-static
        SDL3_shadercross::SDL3_shadercross-static
        enet::enet_static
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
