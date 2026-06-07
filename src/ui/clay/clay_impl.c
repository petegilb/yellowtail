/*
 * Clay UI — implementation translation unit.
 *
 * This is the ONE place in the whole project that:
 *   1. defines CLAY_IMPLEMENTATION (turns clay.h from a header into a lib), and
 *   2. #includes Clay's SDL3 renderer source file (whose drawing functions
 *      are `static` and therefore cannot be linked — only #included).
 *
 * Don't add a second TU that does either of those things. clay.h enforces
 * the CLAY_IMPLEMENTATION rule with multiple-definition link errors; the
 * renderer rule is enforced socially (you'd get duplicate `static` copies
 * but no link error — symbols would just diverge silently).
 *
 * Language choice
 * ---------------
 * This file is `.c`, not `.cpp`, on purpose. clay_renderer_SDL3.c uses
 * C99 VLAs (`SDL_Vertex vertices[totalVertices];`) which C++ does not
 * accept. The C-linkage wrappers at the bottom are what C++ TUs call
 * into; see clay_impl.h for the public interface.
 *
 * Build wiring
 * ------------
 * src/CMakeLists.txt globs *.cpp AND *.c so this file is picked up
 * automatically. The renderer is reachable via the `<renderers/...>`
 * angle-bracket include because the root CMakeLists.txt adds
 * ${clay_SOURCE_DIR} to the executable's include path.
 */

#define CLAY_IMPLEMENTATION
#include <clay.h>

/* Pulls in Clay_SDL3RendererData + SDL_Clay_RenderClayCommands + the
 * supporting `static` helpers (rounded-rect tessellation, arcs, text
 * measurement, etc.). The renderer's own `#include "../../clay.h"` is
 * resolved relative to its own location inside clay-src/renderers/SDL3/,
 * so it finds the correct clay.h regardless of where we include it from. */
#include <renderers/SDL3/clay_renderer_SDL3.c>

#include "clay_impl.h"

#include <stdio.h>

void yt_clay_handle_errors(Clay_ErrorData error_data) {
    /* Clay strings are length-prefixed slices, not null-terminated —
     * use the %.*s form so we don't read past the end. */
    fprintf(stderr, "[clay] %.*s\n",
            (int)error_data.errorText.length,
            error_data.errorText.chars);
}

void yt_clay_render(SDL_Renderer *renderer,
                    TTF_TextEngine *text_engine,
                    TTF_Font **fonts,
                    Clay_RenderCommandArray *commands) {
    Clay_SDL3RendererData data = {
        .renderer = renderer,
        .textEngine = text_engine,
        .fonts = fonts,
    };
    SDL_Clay_RenderClayCommands(&data, commands);
}
