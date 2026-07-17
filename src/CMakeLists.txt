# --- Engine static library (runtime; shared by the game and editor executables) ---
# Everything under engine; the per-app entry points live in
# game/ and editor/ and are compiled into their own executables below.
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.c)
file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.h)

add_library(engine STATIC ${ENGINE_SOURCES} ${ENGINE_HEADERS})

# PUBLIC so the game/editor executables inherit these transitively by linking `engine`.
target_include_directories(engine PUBLIC
        "${CMAKE_SOURCE_DIR}/external"
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
        enet::enet_static
        Jolt
        glm::glm
)

# GLM defaults to OpenGL's [-1, 1] clip-space Z; SDL_GPU (Vulkan/D3D12/Metal) wants [0, 1].
# PUBLIC so every TU that includes glm through the engine sees the same convention.
target_compile_definitions(engine PUBLIC
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_ENABLE_EXPERIMENTAL
)

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

# --- Game executable ---
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/game/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/game/*.c)
file(GLOB_RECURSE GAME_HEADERS CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/game/*.h)
add_executable(ytail_game ${GAME_SOURCES} ${GAME_HEADERS})
target_link_libraries(ytail_game PRIVATE engine)
yt_configure_app(ytail_game)

# --- Editor executable ---
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/editor/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/editor/*.c)
file(GLOB_RECURSE EDITOR_HEADERS CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/editor/*.h)
add_executable(ytail_editor ${EDITOR_SOURCES} ${EDITOR_HEADERS}
        ${CMAKE_SOURCE_DIR}/external/imguizmo/ImGuizmo.cpp)
target_link_libraries(ytail_editor PRIVATE engine)
yt_configure_app(ytail_editor)
