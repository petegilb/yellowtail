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
#include "GameplayStatics.h"
#include "serialize/ComponentRegistry.h"

namespace ytail {
    class ResourceManager;
    class Application;
    class DebugLineRenderer;
    class BillboardRenderer;
    class PointShadowRenderer;

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
        
        // ui tick run right before the render tick
        void uiTick();

        // One fixed simulation step (physics, deterministic gameplay). Driven by the accumulator
        // in tick(), so it runs 0..N times per frame with a constant dt.
        void fixedTick(float deltaTime);

        void updateTick();

        // alpha is the fraction (0..1) into the next fixed step, for interpolating rendered state.
        int renderTick(float alpha);

        // set the play state of the engine (i.e. paused or simulating)
        void setPlayState(PlayState newState) {playState = newState;}
        [[nodiscard]] bool isSimulating() const { return playState == PlayState::Simulating; }
        [[nodiscard]] PlayState getPlayState() const { return playState; }

        // count of fixed steps run. The backbone for networking (tag state/inputs by tick).
        [[nodiscard]] Uint64 getTickNumber() const { return tickNumber; }

        // Create (or resize) the depth+stencil texture to match the given pixel size.
        void ensureDepthTexture(int width, int height);

        // Create (or resize) the square, sampleable shadow-map depth texture.
        void ensureShadowMapTexture(int size);

        // Offscreen scene color target the geometry pass renders into (scaled res, blitted to swapchain).
        void ensureSceneColorTexture(int width, int height);

        // Pixel size the scene renders at: window pixels * resolutionScale, clamped to >= 1.
        void getRenderTargetSize(int& outWidth, int& outHeight) const;

        // Switch the swapchain present mode at runtime (VSYNC/MAILBOX/IMMEDIATE)
        void setPresentMode(SDL_GPUPresentMode mode);

        enum class WindowMode { Windowed, Borderless, Fullscreen };
        void setWindowMode(WindowMode mode);
        void setResolution(int width, int height);
        void setTargetDisplay(SDL_DisplayID display);
        // Unique resolutions supported by the given display (0 == the window's current display).
        [[nodiscard]] std::vector<glm::ivec2> getAvailableResolutions(SDL_DisplayID display) const;

        void handleInput(const SDL_KeyboardEvent& keyboard_event);

        Entity* addEntity();
        // Create an entity with a set id, used when loading a scene.
        Entity* addEntityWithId(Uint32 id);
        // Remove every entity. Used before loading a scene.
        void clearScene();
        Entity* getEntity(Uint32 id);

        // Parent childId under parentId, keeping both link sides in sync. parentId == 0 detaches
        // to root. Returns false (no-op) on unknown ids, self-parenting, or a cycle.
        bool reparent(Uint32 childId, Uint32 parentId);

        // Remove an entity and its whole subtree (all descendants go with it).
        void removeEntity(Uint32 id);

        // All entities, for the editor outliner. Lookups by id still go through getEntity().
        [[nodiscard]] const std::unordered_map<Uint32, std::unique_ptr<Entity>>& getEntities() const { return entities; }

        void setActiveCamera(Uint32 id);

        // View + projection matrices for the active camera. False if there's no active camera.
        [[nodiscard]] bool getCameraMatrices(glm::mat4& outView, glm::mat4& outProjection) const;

        // Build a world-space ray from a window pixel through the active camera. False if no camera.
        [[nodiscard]] bool screenPointToRay(float screenX, float screenY,
                                            glm::vec3& outOrigin, glm::vec3& outDir) const;

        // Scene ambient light: the shader uses color * intensity.
        [[nodiscard]] glm::vec3 getAmbientColor() const { return { ambientDebug.x, ambientDebug.y, ambientDebug.z }; }
        void setAmbientColor(const glm::vec3& color) { ambientDebug = ImVec4(color.x, color.y, color.z, 1.0f); }
        [[nodiscard]] float getAmbientIntensity() const { return ambientIntensity; }
        void setAmbientIntensity(float intensity) { ambientIntensity = intensity; }

        // Builds components by serial id when loading a scene.
        [[nodiscard]] const ComponentRegistry& getComponentRegistry() const { return componentRegistry; }

        bool showPhysicsShapes = false;

        // Editor reference grid on the XZ plane. gridSpacing = world units per cell,
        // gridExtent = cells from the origin in each direction.
        bool showGrid = false;
        float gridSpacing = 1.0f;
        int gridExtent = 100;
        float gridOpacity = 0.8f;

        // Editor light gizmos
        bool showLightGizmos = false;
        Uint32 selectedEntity = 0;

        // Editor billboard icons for lights and inactive cameras (camera-facing sprites).
        bool showEditorIcons = false;

        // Directional (sun) shadow map. The first directional light with castsShadows drives it.
        bool showShadows = false;
        float shadowOrthoExtent = 40.0f;  // half-size of the ortho box, world units
        float shadowDistance = 50.0f;     // how far back along -direction the light sits
        float shadowNear = 1.0f;
        float shadowFar = 150.0f;
        float shadowBias = 0.0005f;
        glm::vec3 shadowFocus{0.0f};      // world point the shadow box is centered on

        // Omnidirectional shadows for point lights flagged castsShadows.
        bool showPointShadows = true;
        float pointShadowBias = 0.0f;           // slope-scaled bias floor (back-face render needs none)
        float pointShadowSlope = 0.0f;          // bias slope (× (1 - NdotL))
        float pointShadowDiskRadius = 0.0032f;  // PCF softness (× distance)
        int pointShadowBudget = 4;              // max cube slots re-rendered per frame (rest cached)
    protected:
        SDL_Window* window = nullptr;
        bool bRunning = true;
        SDL_GPUDevice* device = nullptr;
        const char* BasePath = nullptr;
        bool bUsingSRGB = false;
        SDL_LogPriority logPriority = SDL_LOG_PRIORITY_VERBOSE;

        // Depth+stencil target, recreated when the window size changes (see ensureDepthTexture).
        SDL_GPUTexture* depthTexture = nullptr;
        int depthTextureW = 0;
        int depthTextureH = 0;

        // Offscreen scene color target, recreated when the scaled resolution changes.
        SDL_GPUTexture* sceneColorTexture = nullptr;
        int sceneColorW = 0;
        int sceneColorH = 0;

        // Sun shadow-map depth target (sampleable), rendered from the caster's POV.
        SDL_GPUTexture* shadowMapTexture = nullptr;
        int shadowMapSize = 2048;         // desired resolution, tunable in the editor
        int shadowMapCurrentSize = 0;     // resolution the current texture was created at

        // Light-space view*proj for the first directional shadow caster. False if none casts.
        [[nodiscard]] bool computeSunLightMatrix(glm::mat4& outLightViewProj) const;

        // Recorded so a present-mode change re-applies the same composition
        SDL_GPUSwapchainComposition swapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
        SDL_GPUPresentMode presentMode = SDL_GPU_PRESENTMODE_VSYNC;

        // Display settings driven from the debug menu.
        WindowMode windowMode = WindowMode::Windowed;
        // size the windowed window restores to
        int windowedWidth = 1280;        
        int windowedHeight = 720;
        // exclusive-fullscreen target; 0 == use the desktop mode
        int fullscreenWidth = 0;         
        int fullscreenHeight = 0;
        // 0.25..2.0, scales the offscreen target's pixel size
        float resolutionScale = 1.0f;    
        float uiScale = 1.0f;
        // 0 == primary/current
        SDL_DisplayID targetDisplay = 0;

        // Fixed simulation timestep: 60 steps/sec. Physics + deterministic gameplay run at this rate
        // regardless of render frame rate.
        static constexpr float FIXED_DT = 1.0f / 60.0f;
        // Cap on accumulated time so a hitch can't trigger a runaway catch-up.
        // Past this, the sim briefly runs in slow motion instead of trying to replay every missed step.
        static constexpr float MAX_ACCUMULATOR = 0.25f;

        // Fixed-step simulation state. Leftover time carried between frames, the running fixed-step count
        float fixedAccumulator = 0.0f;
        Uint64 tickNumber = 0;
        PlayState playState = PlayState::Simulating;

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

        // Maps component serial ids to factories, filled once in the constructor.
        ComponentRegistry componentRegistry;

        // Draws the physics debug wireframe. Off unless showPhysicsShapes is enabled.
        std::unique_ptr<ytail::DebugLineRenderer> debugLineRenderer;

        // Draws the editor reference grid, on its own buffer independent of the physics wireframe.
        std::unique_ptr<ytail::DebugLineRenderer> gridLineRenderer;

        // Draws editor light gizmos (arrows / attenuation spheres), on its own buffer.
        std::unique_ptr<ytail::DebugLineRenderer> gizmoLineRenderer;

        // Draws camera-facing editor icons. Icon textures come from the ResourceManager cache.
        std::unique_ptr<ytail::BillboardRenderer> billboardRenderer;

        // Omnidirectional point-light shadows (cube-array depth maps).
        std::unique_ptr<ytail::PointShadowRenderer> pointShadowRenderer;

        // The game or editor driving this engine. Non-owning, lives in main()
        Application* app = nullptr;

        // dear imgui
        void initializeImGui() const;
        static void renderImGui(SDL_GPUCommandBuffer* commandBuffer, Uint32 fbWidth, Uint32 fbHeight);
        static void shutdownImGui();
        ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
        ImVec4 ambientDebug = ImVec4(1.f, 1.f, 1.f, 1.00f);
        float ambientIntensity = 0.2f;
        bool showDebugWindow = false;

        bool shaderCrossInitialized = false;
};

} // ytail

#endif //YELLOWTAIL_ENGINE_H