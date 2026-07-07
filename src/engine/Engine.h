//
// Created by Peter Gilbert on 6/27/26.
//

#ifndef YELLOWTAIL_ENGINE_H
#define YELLOWTAIL_ENGINE_H

#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3_shadercross/SDL_shadercross.h>

#include "Entity.h"

namespace ytail {
    class ResourceManager;

    class Engine {
    public:
        Engine();
        ~Engine();

        int run();

        void mainLoop();

        void quit();

        // Advance a frame (one iteration in the main loop)
        void tick(float deltaTime);

        void inputTick();

        void updateTick();

        int renderTick();

        void handleInput(const SDL_KeyboardEvent& keyboard_event);

        Entity* addEntity();
        Entity* getEntity(Uint32 id);

        void setActiveCamera(Uint32 id);
    protected:
        SDL_Window* window = nullptr;
        bool bRunning = true;
        SDL_GPUDevice* device = nullptr;
        const char* BasePath = nullptr;

        // locks the framerate if greater than 0
        int framerateLock = 60;
        int entityCounter = 0;

        // world stuff
        std::unordered_map<Uint32, std::unique_ptr<Entity>> entities;

        // The camera to render from this frame. Non-owning — the entity itself lives in
        // `entities`. Must have a TransformComponent (view) + CameraComponent (projection).
        Entity* activeCamera = nullptr;

        // ResourceManager that handles the lifetimes of objects loaded into memory
        std::unique_ptr<ytail::ResourceManager> resourceManager;
};

} // ytail

#endif //YELLOWTAIL_ENGINE_H