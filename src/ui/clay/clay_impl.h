#pragma once
/*
 * Clay UI — C-linkage wrappers around Clay's bundled SDL3 renderer.
 *
 * Why this file exists
 * --------------------
 * Clay is a single-header layout library (clay.h). Its SDL3 reference
 * renderer (renderers/SDL3/clay_renderer_SDL3.c) is a `.c` file whose
 * drawing functions are declared `static`. That's the canonical "drop-in
 * a TU" pattern — you can't link against them, you have to #include them.
 * Our actual TU lives in clay_impl.c next to this header.
 *
 * Because the renderer is `static` we can't call it directly from C++ TUs.
 * This header exposes a thin `extern "C"` interface so main.cpp (or
 * anywhere else in C++) can drive Clay rendering without dragging Clay's
 * macro soup or the renderer's C99 VLAs into a C++ compilation context.
 *
 * Compatibility caveat
 * --------------------
 * The bundled renderer draws via `SDL_Renderer` (SDL3's 2D API), NOT
 * `SDL_GPU`. As long as the app uses SDL_Renderer this is fine; once the
 * main scene moves to SDL_GPU (see docs/3d-roadmap.md Phase 0) Clay needs
 * either a custom SDL_GPU-backed renderer or a second window/renderer for
 * UI. Don't grow Clay-driven gameplay UI until you've decided the path.
 *
 * Usage (from C++)
 * ----------------
 *   #include "ui/clay/clay_impl.h"
 *
 *   // once, at app startup (after SDL_Init, after the SDL_Renderer exists):
 *   TTF_Init();
 *   TTF_TextEngine* text_engine = TTF_CreateRendererTextEngine(renderer);
 *   TTF_Font* fonts[1] = { TTF_OpenFont(font_path, 16.0f) };
 *
 *   uint64_t mem_size = Clay_MinMemorySize();
 *   Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(mem_size, malloc(mem_size));
 *   Clay_Initialize(arena, { (float)width, (float)height },
 *                   Clay_ErrorHandler{ yt_clay_handle_errors, nullptr });
 *   Clay_SetMeasureTextFunction(your_measure_text_cb, fonts);
 *
 *   // each frame:
 *   Clay_SetLayoutDimensions({ (float)w, (float)h });
 *   Clay_SetPointerState({ (float)mx, (float)my }, mouse_down);
 *   Clay_BeginLayout();
 *   CLAY({ .id = CLAY_ID("Root"), .layout = { ... } }) {
 *       // ...children...
 *   }
 *   Clay_RenderCommandArray cmds = Clay_EndLayout();
 *   yt_clay_render(renderer, text_engine, fonts, &cmds);
 *
 * `fonts` is a TTF_Font** indexed by Clay_TextElementConfig.fontId; the
 * renderer expects that layout (matches upstream's SDL3-simple-demo).
 */

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <clay.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pass to Clay_Initialize via Clay_ErrorHandler. Logs the Clay error
 * string (which is NOT null-terminated — clay uses length-prefixed slices)
 * to stderr. Replace with your engine logger once you have one.
 */
void yt_clay_handle_errors(Clay_ErrorData error_data);

/*
 * Submit a frame of Clay render commands to the SDL3 renderer.
 *
 *   renderer    — the SDL_Renderer that owns the current window's backbuffer.
 *   text_engine — created via TTF_CreateRendererTextEngine(renderer); kept
 *                 alive across frames (don't recreate per-frame).
 *   fonts       — array of TTF_Font*, indexed by Clay_TextElementConfig.fontId.
 *                 Owned by the caller; this function does not free.
 *   commands    — the result of Clay_EndLayout() this frame.
 *
 * Internally calls Clay's static `SDL_Clay_RenderClayCommands` after
 * packing the inputs into the renderer's expected struct.
 */
void yt_clay_render(SDL_Renderer *renderer,
                    TTF_TextEngine *text_engine,
                    TTF_Font **fonts,
                    Clay_RenderCommandArray *commands);

#ifdef __cplusplus
}
#endif
