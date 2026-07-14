//
// Created by PeterPC on 7/14/2026.
//

#ifndef YELLOWTAIL_EDITOR_H
#define YELLOWTAIL_EDITOR_H

#include <SDL3/SDL.h>

#include "engine/Application.h"

namespace ytail
{
    class Engine;

    class Editor : public Application {
    public:
        explicit Editor(Engine* inEngine);
        ~Editor() override;

        void start() override;
        void eventTick(const SDL_Event& event) override;
        void tick(float deltaTime) override;

    protected:
        void handleInput(const SDL_KeyboardEvent& keyboard_event);
    };
} // ytail

#endif //YELLOWTAIL_EDITOR_H
