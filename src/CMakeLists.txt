# --- Engine static library (runtime; shared by the game and editor executables) ---
# Everything under engine; the per-app entry points live in
# game/ and editor/ and are compiled into their own executables below.
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.c)
file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.h)

add_library(engine STATIC ${ENGINE_SOURCES} ${ENGINE_HEADERS})

# PUBLIC so linking `engine` inherits these. PROJECT_SOURCE_DIR (not CMAKE_SOURCE_DIR) so the
# paths stay pinned to yellowtail's tree when it's consumed as a submodule of another project.
target_include_directories(engine PUBLIC
        "${PROJECT_SOURCE_DIR}/external"
        "${JoltPhysics_SOURCE_DIR}/.."
        "${clay_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"   # lets game/editor do #include "engine/Engine.h"
)
target_link_libraries(engine PUBLIC
        SDL3::SDL3-static
        SDL3_image::SDL3_image-static
        SDL3_ttf::SDL3_ttf-static
        SDL3_shadercross::SDL3_shadercross-static
        imgui
        Jolt
        glm::glm
        nlohmann_json::nlohmann_json
        Tracy::TracyClient
)

# GLM defaults to OpenGL's [-1, 1] clip-space Z; SDL_GPU (Vulkan/D3D12/Metal) wants [0, 1].
# PUBLIC so every TU that includes glm through the engine sees the same convention.
target_compile_definitions(engine PUBLIC
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_ENABLE_EXPERIMENTAL
)

# Absolute path to the engine's runtime assets. A game that links `engine` as a submodule
# needs the shaders beside its executable; yt_copy_engine_assets below handles that, and this
# variable is here for anything else a consumer wants to copy (icons, etc). Cached so it stays
# visible after add_subdirectory returns to the consumer's CMakeLists.
set(YELLOWTAIL_ASSETS_DIR "${PROJECT_SOURCE_DIR}/assets" CACHE INTERNAL "yellowtail engine runtime assets")

# --- Per-app finalize: output dir, debugger working dir, and the asset/DLL copy steps ---
# Applied to every executable so each one is independently runnable from its own directory.
function(yt_configure_app target)
    set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${target})
    set_property(TARGET ${target} PROPERTY
            VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${target}>)

    # Copy assets next to the executable (per-config; works in debug and release).
    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/assets"
                    "$<TARGET_FILE_DIR:${target}>/assets"
            COMMENT "Copying assets next to ${target}")

    # Windows: dxcompiler is always SHARED (DXC forces it). Without our build next to the
    # exe, Windows loads System32's dxcompiler.dll by mistake.
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:dxcompiler>"
                        "$<TARGET_FILE:dxildll>"
                        "$<TARGET_FILE_DIR:${target}>"
                COMMENT "Copying dxcompiler.dll + dxil.dll next to ${target}")
    endif()

    if(WIN32 AND MINGW)
        target_link_options(${target} PRIVATE -mwindows)
    endif()
endfunction()

# Copy the engine's shaders next to a consumer's executable. A game that links `engine`
# calls this alongside shipping its own assets, so the required shaders end up beside the exe.
# Merges into the target's assets/ dir.
function(yt_copy_engine_assets target)
    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${YELLOWTAIL_ASSETS_DIR}/shaders"
                    "$<TARGET_FILE_DIR:${target}>/assets/shaders"
            COMMENT "Copying engine shaders next to ${target}")
endfunction()

# Copy the engine's editor icons (light/camera gizmo sprites, app icon) next to an executable.
# An editor build calls this on top of yt_copy_engine_assets.
function(yt_copy_editor_assets target)
    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${YELLOWTAIL_ASSETS_DIR}/textures/icons"
                    "$<TARGET_FILE_DIR:${target}>/assets/textures/icons"
            COMMENT "Copying engine editor icons next to ${target}")
endfunction()

# --- Editor library (reusable; the Editor application + its ImGui panels) ---
# A static lib so a game consuming yellowtail can build its own editor executable that knows
# about the game's components. main.cpp stays out of the lib; each executable supplies its own.
file(GLOB_RECURSE EDITOR_LIB_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/editor/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/editor/*.c)
list(FILTER EDITOR_LIB_SOURCES EXCLUDE REGEX "/main\\.cpp$")
file(GLOB_RECURSE EDITOR_LIB_HEADERS CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/editor/*.h)
add_library(editor STATIC ${EDITOR_LIB_SOURCES} ${EDITOR_LIB_HEADERS}
        ${PROJECT_SOURCE_DIR}/external/imguizmo/ImGuizmo.cpp)
target_link_libraries(editor PUBLIC engine)

# Bake in the top-level project's source assets dir so the editor can also write saved scenes
# back into the git tree, not just the exe-relative copy. CMAKE_SOURCE_DIR is the project being
# built (yellowtail standalone, or the game that consumes it as a submodule).
target_compile_definitions(editor PRIVATE YT_EDITOR_SOURCE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets/")

# The sample game and the editor only build when yellowtail is the project being built.
# Consumed as a submodule these are skipped: a game only wants the `engine` library, not
# yellowtail's own executables (and their asset/DLL copy steps) in its build.
if(PROJECT_IS_TOP_LEVEL)
    # --- Game executable (sample) ---
    file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/game/*.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/game/*.c)
    file(GLOB_RECURSE GAME_HEADERS CONFIGURE_DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/game/*.h)
    add_executable(ytail_game ${GAME_SOURCES} ${GAME_HEADERS})
    target_link_libraries(ytail_game PRIVATE engine)
    yt_configure_app(ytail_game)

    # --- Editor executable ---
    # Entry point only; the editor itself lives in the `editor` library above.
    add_executable(ytail_editor ${CMAKE_CURRENT_SOURCE_DIR}/editor/main.cpp)
    target_link_libraries(ytail_editor PRIVATE editor)
    yt_configure_app(ytail_editor)
endif()
