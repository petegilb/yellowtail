//
// Created by Peter Gilbert on 7/14/26.
//

#ifndef YELLOWTAIL_APPLICATION_H
#define YELLOWTAIL_APPLICATION_H

#include <SDL3/SDL.h>

namespace ytail {
    class Engine;

    // Editor / game derive from this application interface
    class Application {
    public:
        explicit Application(Engine* inEngine) : engine(inEngine) {
            if (engine == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Error: Engine pointer is null inside an Application!");
            }
            SDL_assert(engine != nullptr);
        }
        virtual ~Application() = default;

        // Once, after the engine + resources are up but before the loop starts. Build the scene here.
        virtual void start() {}

        // For each SDL event this frame (input, window, etc.).
        virtual void eventTick(const SDL_Event& event) {}

        // Fixed timestep, runs 0..N times per frame with a constant dt
        virtual void fixedTick(float deltaTime) {}

        virtual void tick(float deltaTime) {}

        // Build ImGui windows for the app (editor panels)
        virtual void uiTick() {}

    protected:
        Engine* engine;
    };
} // ytail

#endif //YELLOWTAIL_APPLICATION_H
