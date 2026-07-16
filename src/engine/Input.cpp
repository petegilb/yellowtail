//
// Created by Peter Gilbert on 7/16/26.
//

#include "Input.h"

namespace ytail {
    Input& Input::get() {
        static Input instance;
        return instance;
    }

    void Input::init(SDL_Window* inWindow) {
        window = inWindow;
    }

    void Input::setMouseCaptured(bool captured) {
        // The UI owns the cursor while it is up, so never grab it out from under a menu.
        if (captured && uiActive) return;
        if (window == nullptr || captured == mouseCaptured) return;

        // Remember where the visible cursor was as we grab, so relative-mode warping
        // doesn't leave it somewhere random when we let go.
        if (captured) SDL_GetMouseState(&capturedMouseX, &capturedMouseY);

        SDL_SetWindowRelativeMouseMode(window, captured);
        mouseCaptured = captured;

        // Drop the cursor back where the grab started.
        if (!captured) SDL_WarpMouseInWindow(window, capturedMouseX, capturedMouseY);
    }

    void Input::setUiActive(bool active) {
        if (active == uiActive) return;
        uiActive = active;
        // Hand the cursor to the UI as it opens. Gameplay systems re-grab once it closes.
        if (uiActive) setMouseCaptured(false);
    }

    bool Input::isWindowFocused() const {
        return window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
    }
} // ytail
