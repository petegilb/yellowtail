//
// Created by Peter Gilbert on 6/27/26.
//

#ifndef YELLOWTAIL_ENGINE_H
#define YELLOWTAIL_ENGINE_H

#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3_shadercross/SDL_shadercross.h>
#include "imgui.h"
#include <glm/glm.hpp>

#include "Entity.h"

namespace ytail {
    class ResourceManager;
    class Application;

    class Engine {
    public:
        Engine();
        ~Engine();

        // The application (game or editor) the engine drives. Non-owning; set before run().
        void setApplication(Application* inApp) { app = inApp; }

        // Apps build their scene against the resource manager (meshes, textures, samplers).
        [[nodiscard]] ResourceManager* getResourceManager() const { return resourceManager.get(); }

        int run();

        void mainLoop();

        void quit();

        // Advance a frame (one iteration in the main loop)
        void tick(float deltaTime);

        void eventTick();

        void updateTick();

        int renderTick();

        // Create (or resize) the depth+stencil texture to match the given pixel size.
        void ensureDepthTexture(int width, int height);

        // Switch the swapchain present mode at runtime (VSYNC/MAILBOX/IMMEDIATE)
        void setPresentMode(SDL_GPUPresentMode mode);

        void handleInput(const SDL_KeyboardEvent& keyboard_event);

        Entity* addEntity();
        Entity* getEntity(Uint32 id);

        void setActiveCamera(Uint32 id);
    protected:
        SDL_Window* window = nullptr;
        bool bRunning = true;
        SDL_GPUDevice* device = nullptr;
        const char* BasePath = nullptr;
        bool bUsingSRGB = false;

        // Depth+stencil target, recreated when the window size changes (see ensureDepthTexture).
        SDL_GPUTexture* depthTexture = nullptr;
        int depthTextureW = 0;
        int depthTextureH = 0;

        // Recorded so a present-mode change re-applies the same composition
        SDL_GPUSwapchainComposition swapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
        SDL_GPUPresentMode presentMode = SDL_GPU_PRESENTMODE_VSYNC;

        // locks the framerate if greater than 0
        int framerateLock = 0;
        int entityCounter = 0;
        int drawCallsLastFrame = 0;

        // world stuff
        glm::vec3 ambientLight{0.0f}; // currently set to ambientDebug
        std::unordered_map<Uint32, std::unique_ptr<Entity>> entities;

        // The camera to render from this frame. Non-owning - the entity itself lives in
        // `entities`. Must have a TransformComponent (view) + CameraComponent (projection).
        Entity* activeCamera = nullptr;

        // ResourceManager that handles the lifetimes of objects loaded into memory
        std::unique_ptr<ytail::ResourceManager> resourceManager;

        // The game or editor driving this engine. Non-owning, lives in main()
        Application* app = nullptr;

        // dear imgui
        void initializeImGui();
        void updateImGui();
        void renderImGui(SDL_GPUCommandBuffer* commandBuffer);
        void shutdownImGui();
        ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
        ImVec4 ambientDebug = ImVec4(1.f, 1.f, 1.f, 1.00f);
        float ambientIntensity = 0.2f;
        bool showDebugWindow = false;
};

} // ytail

#endif //YELLOWTAIL_ENGINE_H