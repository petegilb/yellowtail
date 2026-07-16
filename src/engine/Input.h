//
// Created by Peter Gilbert on 7/16/26.
//

#ifndef YELLOWTAIL_INPUT_H
#define YELLOWTAIL_INPUT_H
#include <SDL3/SDL.h>

namespace ytail {
    // Single owner of input state
    class Input {
    public:
        static Input& get();

        // Called once by the Engine after the window exists.
        void init(SDL_Window* inWindow);

        void setMouseCaptured(bool captured);
        [[nodiscard]] bool isMouseCaptured() const { return mouseCaptured; }

        // UI mode: while active the cursor is handed to the UI and gameplay capture is blocked.
        // Driven by the Engine from whatever UI it is showing.
        void setUiActive(bool active);
        [[nodiscard]] bool isUiActive() const { return uiActive; }

        // True while our window holds OS input focus.
        [[nodiscard]] bool isWindowFocused() const;

        Input(const Input&) = delete;
        Input& operator=(const Input&) = delete;

    private:
        Input() = default;

        SDL_Window* window = nullptr;
        bool mouseCaptured = false;
        bool uiActive = false;

        // Cursor position saved at capture time, restored on release.
        float capturedMouseX = 0.f;
        float capturedMouseY = 0.f;
    };
} // ytail

#endif //YELLOWTAIL_INPUT_H
