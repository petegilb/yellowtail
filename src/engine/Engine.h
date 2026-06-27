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

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    void drawTriangle();

    SDL_GPUShader* loadShader(
        SDL_GPUDevice* inDevice,
        const char* shaderFilename,
        Uint32 samplerCount,
        Uint32 uniformBufferCount,
        Uint32 storageBufferCount,
        Uint32 storageTextureCount
    );

    void InitializeAssetLoader();

protected:
    SDL_Window* window = nullptr;
    bool bRunning = true;
    SDL_GPUDevice* device = nullptr;
    const char* BasePath = nullptr;

    // locks the framerate if greater than 0
    int framerateLock = 60;

    // world stuff
    std::unordered_map<std::uint32_t, std::unique_ptr<Entity>> entities;
};

} // ytail

#endif //YELLOWTAIL_ENGINE_H