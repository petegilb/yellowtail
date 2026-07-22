//
// Created by Peter Gilbert on 6/27/26.
//

#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <ranges>

#include "Profiling.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include <SDL3_image/SDL_image.h>

#include "Application.h"
#include "Input.h"
#include "managers/PhysicsManager.h"
#include "render/DebugLineRenderer.h"
#include "render/DebugDraw.h"
#include "render/BillboardRenderer.h"
#include "render/PointShadowRenderer.h"
#include "render/Texture.h"
#include "components/RenderComponent.h"
#include "components/TransformComponent.h"
#include "components/CameraComponent.h"
#include "components/LightComponent.h"
#include "managers/ResourceManager.h"

namespace ytail {
    Engine::Engine() {
        GameplayStatics::engine = this;
        componentRegistry.registerBuiltins();

        SDL_SetLogPriorities(logPriority);

        SDL_Log("Starting yellowtail...");
        SDL_Log("Current log verbosity is %d", logPriority);
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Error starting SDL! %s", SDL_GetError());
        }
    }

    Engine::~Engine() {
        SDL_Log("Ending yellowtail...");

        // Wait for any in-flight frames to finish before we tear down GPU-backed
        // resources (ImGui backend owns pipelines, buffers, fonts).
        if (device) SDL_WaitForGPUIdle(device);
        if (ImGui::GetCurrentContext()) shutdownImGui();

        // Release everything that owns GPU resources BEFORE destroying the device.
        entities.clear(); // RenderComponents drop their shared_ptr<Mesh>/Material
        debugLineRenderer.reset(); // frees the debug line vertex/transfer buffers
        gridLineRenderer.reset();
        gizmoLineRenderer.reset();
        billboardRenderer.reset();
        pointShadowRenderer.reset();
        resourceManager.reset(); // frees pipelines, samplers, and cached meshes/textures
        if (device && depthTexture) SDL_ReleaseGPUTexture(device, depthTexture);
        if (device && sceneColorTexture) SDL_ReleaseGPUTexture(device, sceneColorTexture);
        if (device && shadowMapTexture) SDL_ReleaseGPUTexture(device, shadowMapTexture);

        GameplayStatics::engine = nullptr;

        if (shaderCrossInitialized) SDL_ShaderCross_Quit();
        if (device && window) SDL_ReleaseWindowFromGPUDevice(device, window);
        if (device) SDL_DestroyGPUDevice(device);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    int Engine::run() {
        device = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
            true,
            nullptr);
        if (device == nullptr){
            SDL_Log("CreateDevice failed");
            return -1;
        }
        window = SDL_CreateWindow("yellowtail.", 1280, 720, SDL_WINDOW_RESIZABLE);
        if (!window) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Error creating SDL window! %s", SDL_GetError());
            return -1;
        }
        if (!SDL_ClaimWindowForGPUDevice(device, window)){
            SDL_Log("ClaimWindow failed");
            return -1;
        }
        Input::get().init(window);
        // Output encode: render in linear, let the swapchain re-encode to sRGB on scanout.
        // SDR_LINEAR makes the swapchain the *_SRGB format, so a linear fragment result is
        // gamma-encoded by the hardware on write. Pairs with the per-texture srgb decode in
        // getTexture() to close the loop on a fully linear lighting pipeline.
        // Must run before pipelines/ImGui query the swapchain format (they pick up the _SRGB variant).
        if (SDL_WindowSupportsGPUSwapchainComposition(device, window,
                SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR)) {
            swapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR;
            SDL_SetGPUSwapchainParameters(device, window, swapchainComposition, presentMode);
            bUsingSRGB = true;
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SDR_LINEAR swapchain unsupported; output will not be gamma-correct");
        }
        shaderCrossInitialized = SDL_ShaderCross_Init();
        if (!shaderCrossInitialized) {
            SDL_Log("ShaderCross_Init failed");
            return -1;
        }
        BasePath = SDL_GetBasePath();
        resourceManager = std::make_unique<ResourceManager>(device, window, BasePath);
        physics::PhysicsManager::get();
        // TODO should I shield these from being created in release builds?
        debugLineRenderer = std::make_unique<DebugLineRenderer>(device);
        gridLineRenderer = std::make_unique<DebugLineRenderer>(device);
        gizmoLineRenderer = std::make_unique<DebugLineRenderer>(device);
        billboardRenderer = std::make_unique<BillboardRenderer>(device);
        pointShadowRenderer = std::make_unique<PointShadowRenderer>(device, resourceManager.get());
        initializeImGui();

        // set app icon
        SDL_Surface* imageData = IMG_Load(resourceManager->resolveAssetPath("textures/icons/appicon.png").c_str());
        if (imageData == nullptr){
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not app icon! %s", SDL_GetError());
        }
        SDL_SetWindowIcon(window, imageData);

        // The app (game or editor) builds its scene now that the engine + resources are ready.
        if (app) app->start();

        mainLoop();
        return 0;
    }

    void Engine::mainLoop() {
        Uint64 fps = 0;
        Uint64 fpsTimer = SDL_GetTicksNS();
        Uint64 lastFrame = SDL_GetTicksNS();
        // TODO change to accumulator and fixed time step
        while (bRunning) {
            const Uint64 frameStart = SDL_GetTicksNS();
            const float deltaTime = static_cast<float>(frameStart - lastFrame) / SDL_NS_PER_SECOND;
            lastFrame = frameStart;

            tick(deltaTime);
            fps++;

            // framerate lock: sleep for whatever is left of this frame's budget.
            // 1 sec / N frames = budget per frame; subtract the work we just did.
            if (framerateLock > 0) {
                const Uint64 targetNs = SDL_NS_PER_SECOND / framerateLock;
                const Uint64 workedNs = SDL_GetTicksNS() - frameStart;
                if (workedNs < targetNs) {
                    SDL_DelayNS(targetNs - workedNs);
                }
            }

            // once a second, report FPS and the average frame time in ms
            if (frameStart - fpsTimer >= SDL_NS_PER_SECOND) {
                fpsTimer = frameStart;
                const double frameMs = fps > 0 ? 1000.0 / static_cast<double>(fps) : 0.0;
                char title[128];
                SDL_snprintf(title, sizeof(title), "yellowtail | %d FPS | %.2f ms/frame",
                             static_cast<int>(fps), frameMs);
                SDL_SetWindowTitle(window, title);
                fps = 0;
            }

            FrameMark;  // Tracy frame boundary
        }
    }

    void Engine::quit() {
        bRunning = false;
    }

    void Engine::tick(float deltaTime) {
        ZoneScoped;
        eventTick();

        // see https://www.gafferongames.com/post/fix_your_timestep/

        // Fixed-step simulation: accumulate real time and drain it in constant-sized steps, so the
        // sim advances at FIXED_DT no matter the frame rate.
        if (isSimulating()) {
            fixedAccumulator += deltaTime;
            fixedAccumulator = std::min(fixedAccumulator, MAX_ACCUMULATOR);
            while (fixedAccumulator >= FIXED_DT) {
                fixedTick(FIXED_DT);
                fixedAccumulator -= FIXED_DT;
            }
        }

        // Variable-step per-frame work (runs as fast as we render): engine, then app, then components.
        updateTick();
        if (app) app->tick(deltaTime);
        for (const auto& [id, entity] : entities) {
            if (entity) entity->tick(deltaTime);
        }

        // UI should be updated right before everything is drawn to match the state of the engine.
        uiTick();

        // Fraction into the next fixed step, so render can interpolate between the last two sim states.
        const float alpha = fixedAccumulator / FIXED_DT;
        renderTick(alpha);
    }

    void Engine::fixedTick(float deltaTime) {
        ZoneScoped;
        // engine (physics), then app, then components.
        {
            ZoneScopedN("Physics step");
            physics::PhysicsManager::get().step(deltaTime);
        }
        if (app) app->fixedTick(deltaTime);
        for (const auto& [id, entity] : entities) {
            if (entity) entity->fixedTick(deltaTime);
        }
        tickNumber++;
    }

    void Engine::eventTick() {
        ZoneScoped;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (app) app->eventTick(event);
            for (const auto& [id, entity] : entities) {
                if (entity) entity->eventTick(event);
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) {
                bRunning = false;
                return;
            }
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    bRunning = false;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    handleInput(event.key);
                    break;
                case SDL_EVENT_WINDOW_FOCUS_LOST:
                    // Drop any mouse capture so alt-tabbing mid-drag can't leave the cursor grabbed.
                    Input::get().setMouseCaptured(false);
                    break;
                default: ;
            }
        }
    }

    void Engine::updateTick() {
        ZoneScoped;
        // Hand the cursor to the UI whenever a menu/overlay is up so gameplay input yields to it.
        Input::get().setUiActive(showDebugWindow);
        ambientLight = {ambientDebug.x, ambientDebug.y, ambientDebug.z};
        ambientLight *= ambientIntensity;
    }

    void Engine::ensureDepthTexture(int width, int height) {
        // if the depth texture already matches, we've ensured it's okay, otherwise recreate.
        if (depthTexture && depthTextureW == width && depthTextureH == height) return;
        if (depthTexture) SDL_ReleaseGPUTexture(device, depthTexture);

        SDL_GPUTextureCreateInfo info = {};
        info.type                 = SDL_GPU_TEXTURETYPE_2D;
        info.format               = resourceManager->getDepthStencilFormat();
        info.usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        info.width                = static_cast<Uint32>(width);
        info.height               = static_cast<Uint32>(height);
        info.layer_count_or_depth = 1;
        info.num_levels           = 1;
        depthTexture = SDL_CreateGPUTexture(device, &info);
        depthTextureW = width;
        depthTextureH = height;
        if (depthTexture == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create depth texture: %s", SDL_GetError());
        }
    }

    void Engine::ensureShadowMapTexture(int size) {
        if (shadowMapTexture && shadowMapCurrentSize == size) return;
        if (shadowMapTexture) SDL_ReleaseGPUTexture(device, shadowMapTexture);

        SDL_GPUTextureCreateInfo info = {};
        info.type                 = SDL_GPU_TEXTURETYPE_2D;
        // Sampleable depth: written by the shadow pass, read by the lit shader.
        info.format               = resourceManager->getShadowMapFormat();
        info.usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width                = static_cast<Uint32>(size);
        info.height               = static_cast<Uint32>(size);
        info.layer_count_or_depth = 1;
        info.num_levels           = 1;
        shadowMapTexture = SDL_CreateGPUTexture(device, &info);
        shadowMapCurrentSize = size;
        if (shadowMapTexture == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create shadow map texture: %s", SDL_GetError());
        }
    }

    bool Engine::computeSunLightMatrix(glm::mat4& outLightViewProj) const {
        // First directional light flagged to cast shadows drives the map.
        for (const auto& [id, entity] : entities) {
            if (entity == nullptr) continue;
            const auto* light = entity->getComponent<LightComponent>();
            const auto* transform = entity->getComponent<TransformComponent>();
            if (light == nullptr || transform == nullptr) continue;
            if (light->type != LightType::Directional || !light->castsShadows) continue;

            // -Z rotated into world space (same as the lit path).
            const glm::vec3 dir = glm::normalize(transform->getRotation() * glm::vec3(0.0f, 0.0f, -1.0f));
            // Sit the light back along -dir from the focus and look toward it.
            const glm::vec3 eye = shadowFocus - dir * shadowDistance;
            // Avoid a degenerate up vector when the sun points nearly straight down.
            const glm::vec3 up = (std::abs(dir.y) > 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                           : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::mat4 lightView = glm::lookAt(eye, shadowFocus, up);
            const glm::mat4 lightProj = glm::ortho(-shadowOrthoExtent, shadowOrthoExtent,
                                                   -shadowOrthoExtent, shadowOrthoExtent,
                                                   shadowNear, shadowFar);
            outLightViewProj = lightProj * lightView;
            return true;
        }
        return false;
    }

    void Engine::getRenderTargetSize(int& outWidth, int& outHeight) const {
        int pw, ph;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        outWidth  = std::max(1, static_cast<int>(static_cast<float>(pw) * resolutionScale));
        outHeight = std::max(1, static_cast<int>(static_cast<float>(ph) * resolutionScale));
    }

    void Engine::ensureSceneColorTexture(const int width, const int height) {
        if (sceneColorTexture && sceneColorW == width && sceneColorH == height) return;
        if (sceneColorTexture) SDL_ReleaseGPUTexture(device, sceneColorTexture);

        SDL_GPUTextureCreateInfo info = {};
        info.type                 = SDL_GPU_TEXTURETYPE_2D;
        // Match the swapchain format the pipelines are built against, and allow sampling for the blit.
        info.format               = SDL_GetGPUSwapchainTextureFormat(device, window);
        info.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width                = static_cast<Uint32>(width);
        info.height               = static_cast<Uint32>(height);
        info.layer_count_or_depth = 1;
        info.num_levels           = 1;
        sceneColorTexture = SDL_CreateGPUTexture(device, &info);
        sceneColorW = width;
        sceneColorH = height;
        if (sceneColorTexture == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create scene color texture: %s", SDL_GetError());
        }
    }

    void Engine::setPresentMode(SDL_GPUPresentMode mode) {
        if (mode == presentMode) return;
        if (!SDL_WindowSupportsGPUPresentMode(device, window, mode)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Present mode %d unsupported on this device", mode);
            return;
        }
        // Re-apply with the same composition we picked in run() so gamma handling is unchanged.
        if (!SDL_SetGPUSwapchainParameters(device, window, swapchainComposition, mode)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SetGPUSwapchainParameters failed: %s", SDL_GetError());
            return;
        }
        presentMode = mode;
    }

    void Engine::setWindowMode(WindowMode mode) {
        windowMode = mode;
        // 0 means "current/primary" - resolve to the display the window is on.
        SDL_DisplayID display = targetDisplay ? targetDisplay : SDL_GetDisplayForWindow(window);

        switch (mode) {
            case WindowMode::Windowed:
                SDL_SetWindowFullscreen(window, false);
                SDL_SetWindowSize(window, windowedWidth, windowedHeight);
                SDL_SetWindowPosition(window,
                    SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
                break;
            case WindowMode::Borderless:
                SDL_SetWindowFullscreenMode(window, nullptr); // desktop mode, no resolution switch
                SDL_SetWindowFullscreen(window, true);
                break;
            case WindowMode::Fullscreen: {
                // Default to the desktop mode (keeps the current HiDPI resolution instead of jumping
                // to native pixels). include_high_density=true so Retina modes stay valid matches.
                const SDL_DisplayMode* target = SDL_GetDesktopDisplayMode(display);
                SDL_DisplayMode closest;
                if (fullscreenWidth > 0 && fullscreenHeight > 0 &&
                    SDL_GetClosestFullscreenDisplayMode(display, fullscreenWidth, fullscreenHeight, 0.0f, true, &closest)) {
                    target = &closest;
                }
                SDL_SetWindowFullscreenMode(window, target);
                SDL_SetWindowFullscreen(window, true);
                break;
            }
        }
    }

    void Engine::setResolution(int width, int height) {
        // Only exclusive fullscreen uses an explicit resolution.
        fullscreenWidth = width;
        fullscreenHeight = height;
        if (windowMode == WindowMode::Fullscreen) setWindowMode(windowMode);
    }

    std::vector<glm::ivec2> Engine::getAvailableResolutions(SDL_DisplayID display) const {
        if (display == 0) display = SDL_GetDisplayForWindow(window);
        std::vector<glm::ivec2> out;
        int count = 0;
        SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
        if (modes) {
            for (int i = 0; i < count; ++i) {
                // dedupe: modes repeat per refresh rate
                glm::ivec2 r{ modes[i]->w, modes[i]->h };
                if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
            }
            SDL_free(modes);
        }
        // Some backends (e.g. Wayland) report no exclusive modes; fall back to the desktop size.
        if (out.empty()) {
            if (const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(display)){
                out.emplace_back(dm->w, dm->h);
            }
        }
        return out;
    }

    void Engine::setTargetDisplay(const SDL_DisplayID display) {
        targetDisplay = display;
        // Drop any fullscreen resolution choice so the new display uses its own desktop mode.
        fullscreenWidth = 0;
        fullscreenHeight = 0;
        setWindowMode(windowMode);
    }

    // Grid fade parameters for the grid fragment shader (must match GridLine.frag's GridFade cbuffer).
    struct GridFadeUniform {
        glm::vec2 center;
        float halfRadius;
        float opacity;
    };

    // World-unit reference grid on the XZ plane (y=0). It follows the camera (snapped to cell
    // boundaries so world units stay aligned and the origin lands on an intersection). Each line is
    // one full-length segment; the grid fragment shader fades it radially around the camera so a
    // finite ring reads as an infinite grid. Lines through world x==0 / z==0 are tinted as the axes.
    static void buildGridLines(std::vector<JoltDebugVertex>& out, float spacing, int extent,
                               const glm::vec3& camera) {
        out.clear();
        if (spacing <= 0.0f || extent <= 0) return;

        const float cx = std::floor(camera.x / spacing) * spacing;
        const float cz = std::floor(camera.z / spacing) * spacing;
        const float half = static_cast<float>(extent) * spacing;

        constexpr glm::vec3 gray{0.4f, 0.4f, 0.4f};
        constexpr glm::vec3 xAxis{0.85f, 0.25f, 0.25f};
        constexpr glm::vec3 zAxis{0.3f, 0.5f, 0.9f};

        for (int i = -extent; i <= extent; ++i) {
            const float x = cx + static_cast<float>(i) * spacing;
            const glm::vec3& color = std::abs(x) < 0.5f * spacing ? zAxis : gray;  // world x==0 -> Z axis
            out.push_back({ {x, 0.0f, cz - half}, glm::vec4(color, 1.0f) });
            out.push_back({ {x, 0.0f, cz + half}, glm::vec4(color, 1.0f) });
        }
        for (int i = -extent; i <= extent; ++i) {
            const float z = cz + static_cast<float>(i) * spacing;
            const glm::vec3& color = std::abs(z) < 0.5f * spacing ? xAxis : gray;  // world z==0 -> X axis
            out.push_back({ {cx - half, 0.0f, z}, glm::vec4(color, 1.0f) });
            out.push_back({ {cx + half, 0.0f, z}, glm::vec4(color, 1.0f) });
        }
    }

    // Light gizmos tinted with each light's color
    static void buildLightGizmos(DebugDraw& debug,
        const std::unordered_map<Uint32, std::unique_ptr<Entity>>& entities,
        Uint32 selectedEntity
    ) {
        debug.clear();
        for (const auto& [id, entity] : entities) {
            if (entity == nullptr) continue;
            const auto* light = entity->getComponent<LightComponent>();
            const auto* transform = entity->getComponent<TransformComponent>();
            if (light == nullptr || transform == nullptr) continue;

            const glm::vec4 color(light->color, 1.0f);
            // World-space so gizmos sit where the light actually lights (matters when parented).
            const glm::vec3 pos = glm::vec3(transform->worldMatrix()[3]);

            if (light->type == LightType::Directional) {
                const glm::vec3 dir = glm::normalize(transform->getRotation() * glm::vec3(0.0f, 0.0f, -1.0f));
                debug.arrow(pos, pos + dir * 1.5f, color);
            } else if (id == selectedEntity && light->attenuation > 0.0f) {
                debug.wireSphere(pos, light->attenuation, color);
            }
        }
    }

    int Engine::renderTick(float alpha) {
        ZoneScoped;
        drawCallsLastFrame = 0;

        // just found this reference: https://dawaralvi.github.io/sdl-gpu/
        // get the active camera
        if (activeCamera == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Render camera is null!");
            return -1;
        }
        auto* camXform = activeCamera->getComponent<TransformComponent>();
        glm::mat4 view, projection;
        if (!getCameraMatrices(view, projection)) return -1;

        // Scene targets are sized in real pixels * resolutionScale; the projection's aspect comes
        // from the logical size in getCameraMatrices, so it needs no adjustment.
        int w, h;
        getRenderTargetSize(w, h);
        // minimized (clamped to 1): skip the frame
        if (h <= 1) return 0;
        ensureSceneColorTexture(w, h);
        ensureDepthTexture(w, h);

        // command buffer of commands to be executed by SDL3_gpu
        // does not need to be freed and can only be used on the thread in which it is created (render thread?)
        SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
        if (commandBuffer == nullptr){
            SDL_Log("AcquireGPUCommandBuffer failed: %s", SDL_GetError());
            return -1;
        }

        // a swapchain is a queue of framebuffers (or textures) that are swapped to the screen one after another
        // allow rendering into one texture while another is being displayed
        // help avoid tearing, flickering, and stuttering
        SDL_GPUTexture* swapchainTexture;
        Uint32 swapchainW = 0, swapchainH = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window, &swapchainTexture, &swapchainW, &swapchainH)) {
            SDL_Log("WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
            SDL_SubmitGPUCommandBuffer(commandBuffer);
            return -1;
        }
        if (swapchainTexture == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Swapchain texture is null! can't render!");
            SDL_SubmitGPUCommandBuffer(commandBuffer);
            return -1;
        }

        // Scene renders into the offscreen target (scaled res), then gets blitted to the swapchain.
        // "Blitting" is a computer graphics operation that allows for the rapid copying of a
        // rectangular block of pixels from one image buffer to another (according to my duckduckgo search lol)
        SDL_GPUColorTargetInfo colorTargetInfo = { nullptr };
        colorTargetInfo.texture = sceneColorTexture;
        colorTargetInfo.clear_color = SDL_FColor{ clear_color.x, clear_color.y, clear_color.z, clear_color.w };
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;

        // ImGui uploads its vertex/index data here -- needs to be before BeginRenderPass
        renderImGui(commandBuffer, swapchainW, swapchainH);

        // Physics debug wireframe: regenerate the geometry and stage it. The upload runs a copy
        // pass, so like ImGui it has to happen before BeginRenderPass.
        if (showPhysicsShapes && debugLineRenderer) {
            physics::PhysicsManager::get().debugDraw();
            debugLineRenderer->upload(commandBuffer, physics::PhysicsManager::get().getDebugLines());
        }

        // Editor grid: rebuild + stage its world-space lines (same before-pass copy rule).
        if (showGrid && gridLineRenderer) {
            std::vector<JoltDebugVertex> gridLines;
            buildGridLines(gridLines, gridSpacing, gridExtent, camXform->getPosition());
            gridLineRenderer->upload(commandBuffer, gridLines);
        }

        // Editor light gizmos: rebuild + stage (same before-pass copy rule).
        if (showLightGizmos && gizmoLineRenderer) {
            DebugDraw gizmos;
            buildLightGizmos(gizmos, entities, selectedEntity);
            gizmoLineRenderer->upload(commandBuffer, gizmos.vertices());
        }

        // Render scene depth from the sun's POV so the scene pass can sample it. The map is always
        // bound at slot 2; shadowEnabled (below) gates whether the shader actually reads it.
        glm::mat4 lightViewProj(1.0f);
        const bool hasShadowCaster = showShadows && computeSunLightMatrix(lightViewProj);
        ensureShadowMapTexture(shadowMapSize);
        if (hasShadowCaster) {
            SDL_GPUDepthStencilTargetInfo shadowTargetInfo = {};
            shadowTargetInfo.texture          = shadowMapTexture;
            shadowTargetInfo.clear_depth      = 1.0f;
            shadowTargetInfo.load_op          = SDL_GPU_LOADOP_CLEAR;
            shadowTargetInfo.store_op         = SDL_GPU_STOREOP_STORE;  // sampled in the scene pass
            shadowTargetInfo.stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE;
            shadowTargetInfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            shadowTargetInfo.cycle            = true;

            SDL_GPURenderPass* shadowPass = SDL_BeginGPURenderPass(commandBuffer, nullptr, 0, &shadowTargetInfo);
            SDL_GPUGraphicsPipeline* shadowPipeline = resourceManager->getPipeline(PipelineType::ShadowDepth);
            if (shadowPipeline) {
                SDL_BindGPUGraphicsPipeline(shadowPass, shadowPipeline);
                for (const auto& [idx, entity] : entities) {
                    if (entity == nullptr) continue;
                    const auto* renderComponent = entity->getComponent<RenderComponent>();
                    const auto* transformComponent = entity->getComponent<TransformComponent>();
                    if (renderComponent == nullptr || transformComponent == nullptr) continue;
                    if (!renderComponent->castsShadow) continue;
                    const auto& mesh = renderComponent->mesh;
                    if (!mesh) continue;

                    SDL_GPUBufferBinding vertexBinding { .buffer = mesh->vertexBuffer, .offset = 0 };
                    SDL_GPUBufferBinding indexBinding { .buffer = mesh->indexBuffer, .offset = 0 };
                    SDL_BindGPUVertexBuffers(shadowPass, 0, &vertexBinding, 1);
                    SDL_BindGPUIndexBuffer(shadowPass, &indexBinding, mesh->indexSize);

                    const glm::mat4 lightMvp = lightViewProj * transformComponent->worldMatrix();
                    SDL_PushGPUVertexUniformData(commandBuffer, 0, &lightMvp, sizeof(lightMvp));

                    for (const Submesh& submesh : mesh->submeshes) {
                        SDL_DrawGPUIndexedPrimitives(shadowPass, submesh.indexCount, 1,
                            submesh.indexOffset, 0, 0);
                        drawCallsLastFrame++;
                    }
                }
            }
            SDL_EndGPURenderPass(shadowPass);
        }

        // Point-light (omnidirectional) shadows: render each shadowed point light's cube faces
        // before the scene pass. Keep the cube array allocated regardless so the lit pipeline's
        // slot-3 sampler still has a valid binding when generation is off.
        pointShadowRenderer->ensureTexture();
        if (showPointShadows) {
            pointShadowRenderer->refreshBudgetPerFrame = pointShadowBudget;
            pointShadowRenderer->generate(commandBuffer, entities);
        } else {
            pointShadowRenderer->reset(); // clear stale slots + stats so nothing samples last frame's cube
        }

        // Depth+stencil attachment for the geometry pass. Depth clears to the far plane (1.0);
        // stencil clears to 0. Neither is needed after the frame, so we don't store them.
        // https://github.com/TheSpydog/SDL_gpu_examples/blob/main/Examples/DepthArray.c
        SDL_GPUDepthStencilTargetInfo depthTargetInfo = {};
        depthTargetInfo.texture          = depthTexture;
        depthTargetInfo.clear_depth      = 1.0f;
        depthTargetInfo.load_op          = SDL_GPU_LOADOP_CLEAR;
        depthTargetInfo.store_op         = SDL_GPU_STOREOP_DONT_CARE;
        depthTargetInfo.stencil_load_op  = SDL_GPU_LOADOP_CLEAR;
        depthTargetInfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        depthTargetInfo.clear_stencil    = 0;
        depthTargetInfo.cycle            = true;

        // render all pipelines for all materials here...
        SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, &depthTargetInfo);

        // Stencil reference used by both the LitStaticStencil stamp and the Outline test.
        SDL_SetGPUStencilReference(renderPass, 1);

        // Per-frame lighting: camera position + scene ambient + up to MaxLights scene lights.
        // Pushed once to fragment slot 0 (FrameLighting @ b0 space3). This state persists for
        // every draw in this command buffer, so all lit materials read the same light set.
        FrameLightingUniform frameLighting{};
        frameLighting.viewPos = camXform->getPosition();
        frameLighting.ambient = ambientLight;
        int lightCount = 0;
        for (const auto& [lightIdx, lightEntity] : entities) {
            if (lightCount >= FrameLightingUniform::MaxLights) break;
            if (lightEntity == nullptr) continue;
            const auto* lightComp = lightEntity->getComponent<LightComponent>();
            const auto* lightXform = lightEntity->getComponent<TransformComponent>();
            if (lightComp == nullptr || lightXform == nullptr) continue;

            GpuLight& gpuLight = frameLighting.lights[lightCount];
            // World-space so lighting + shadows agree with parented lights (see PointShadowRenderer).
            gpuLight.position    = glm::vec3(lightXform->worldMatrix()[3]);
            // Forward (-Z) rotated into world space: the direction a directional light travels.
            gpuLight.direction   = glm::normalize(lightXform->getRotation() * glm::vec3(0.0f, 0.0f, -1.0f));
            gpuLight.color       = lightComp->color * lightComp->intensity;
            gpuLight.attenuation = lightComp->attenuation;
            gpuLight.type        = static_cast<int>(lightComp->type);
            // Cube slice for a shadowed point light this frame, or -1 (slotForLightId handles both).
            gpuLight.shadowIndex = showPointShadows ? pointShadowRenderer->slotForLightId(lightIdx) : -1;
            lightCount++;
        }
        frameLighting.lightCount = lightCount;
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &frameLighting, sizeof(frameLighting));

        // Push the shadow matrix/params (frag slot 2) and bind the map (sampler slot 2) once for
        // every lit draw. Materials only touch slots 0-1, so these persist across the loop.
        ShadowUniform shadowUniform{};
        shadowUniform.lightViewProj = lightViewProj;
        shadowUniform.shadowBias    = shadowBias;
        shadowUniform.shadowEnabled = hasShadowCaster ? 1 : 0;
        shadowUniform.texelSize     = 1.0f / static_cast<float>(shadowMapSize);
        shadowUniform.pointBias       = pointShadowBias;
        shadowUniform.pointSlope      = pointShadowSlope;
        shadowUniform.pointDiskRadius = pointShadowDiskRadius;
        SDL_PushGPUFragmentUniformData(commandBuffer, 2, &shadowUniform, sizeof(shadowUniform));

        SDL_GPUTextureSamplerBinding shadowBinding{
            .texture = shadowMapTexture,
            .sampler = resourceManager->getSampler(SamplerType::ShadowPCF)
        };
        SDL_BindGPUFragmentSamplers(renderPass, 2, &shadowBinding, 1);

        // Point-light cube array at slot 3 (per-light shadowIndex gates whether it's read).
        // Skip if creation failed (null); binding a null texture is invalid.
        if (SDL_GPUTexture* pointCube = pointShadowRenderer->getTexture()) {
            SDL_GPUTextureSamplerBinding pointShadowBinding{
                .texture = pointCube,
                .sampler = resourceManager->getSampler(SamplerType::ShadowPCF)
            };
            SDL_BindGPUFragmentSamplers(renderPass, 3, &pointShadowBinding, 1);
        }

        // for each render component and transform component, get the mesh and the material in order to render it
        // TODO optimize this by pipeline binding (rebinding the same pipeline multiple times is a waste of resources)
        for (const auto& [idx, entity]: entities) {
            if (entity == nullptr) continue;
            const auto* renderComponent = entity->getComponent<RenderComponent>();
            const auto* transformComponent = entity->getComponent<TransformComponent>();
            if (renderComponent == nullptr || transformComponent == nullptr) continue;

            auto& mesh = renderComponent->mesh;
            if (!mesh) continue;

            // get vertex and index buffers from mesh
            SDL_GPUBufferBinding vertexBinding { .buffer = mesh->vertexBuffer, .offset = 0 };
            SDL_GPUBufferBinding indexBinding { .buffer = mesh->indexBuffer, .offset = 0 };
            SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
            SDL_BindGPUIndexBuffer(renderPass, &indexBinding, mesh->indexSize);
            // transform uniform (uses the camera input from the top of this function)
            const glm::mat4 model = transformComponent->worldMatrix();
            VertexUniform vsu{
                projection * view * model,
                model,
                transformComponent->normalMatrix()
            };
            SDL_PushGPUVertexUniformData(commandBuffer, 0, &vsu, sizeof(vsu));


            // how do i integrate these buffers and our pipeline and draw properly?

            const auto& materials = renderComponent->materials;

            for (const Submesh& submesh : mesh->submeshes) {
                if (submesh.materialSlot >= materials.size()){
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The mesh %s mat slot %d doesn't have a material!", mesh->name.c_str(), submesh.materialSlot);
                    continue;
                }
                const auto& material = materials[submesh.materialSlot];
                if (!material){
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The mesh %s mat slot %d material is invalid!", mesh->name.c_str(), submesh.materialSlot);
                    continue;
                }

                SDL_GPUGraphicsPipeline* pipeline = resourceManager->getPipeline(material->pipelineType, renderComponent->outline);
                if (!pipeline) continue;
                SDL_BindGPUGraphicsPipeline(renderPass, pipeline);
                if (!material->textures.empty()){
                    // build one binding per texture, in slot order (index 0 → t0/s0, 1 → t1/s1, ...)
                    std::vector<SDL_GPUTextureSamplerBinding> binds;
                    binds.reserve(material->textures.size());
                    for (const auto& tb : material->textures) {
                        binds.push_back({ .texture = tb.texture->handle(), .sampler = tb.sampler });
                    }
                    // one call binds the whole set starting at slot 0
                    SDL_BindGPUFragmentSamplers(
                        renderPass,
                        0,
                        binds.data(),
                        static_cast<Uint32>(binds.size())
                    );
                }
                // Push per-material uniform bytes to fragment slot 1 (Material b1 space3).
                // Slot 0 is the FrameLighting buffer pushed once above.
                if (!material->uniformData.empty()) {
                    SDL_PushGPUFragmentUniformData(commandBuffer, 1, material->uniformData.data(),
                        static_cast<Uint32>(material->uniformData.size())
                    );
                }
                // draw using all our data
                SDL_DrawGPUIndexedPrimitives(renderPass, submesh.indexCount, 1,
                    submesh.indexOffset, 0, 0
                );
                // count draw calls (not including imgui and other things drawing)
                drawCallsLastFrame++;
            }
        }

        // Outline pass: for each outlined object, draw a slightly scaled shell in a flat color.
        // The stencil test (NOT_EQUAL 1) in the Outline pipeline clips it to the ring just outside
        // the silhouette that LitStatic stamped above. Runs after all lit draws so it doesn't
        // clobber the FrameLighting uniform (both use fragment slot 0).
        SDL_GPUGraphicsPipeline* outlinePipeline = resourceManager->getPipeline(PipelineType::Outline);
        if (outlinePipeline) {
            SDL_BindGPUGraphicsPipeline(renderPass, outlinePipeline);
            for (const auto& [idx, entity] : entities) {
                if (entity == nullptr) continue;
                const auto* renderComponent = entity->getComponent<RenderComponent>();
                const auto* transformComponent = entity->getComponent<TransformComponent>();
                if (renderComponent == nullptr || transformComponent == nullptr) continue;
                if (!renderComponent->outline) continue;
                const auto& mesh = renderComponent->mesh;
                if (!mesh) continue;

                SDL_GPUBufferBinding vertexBinding { .buffer = mesh->vertexBuffer, .offset = 0 };
                SDL_GPUBufferBinding indexBinding { .buffer = mesh->indexBuffer, .offset = 0 };
                SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
                SDL_BindGPUIndexBuffer(renderPass, &indexBinding, mesh->indexSize);

                // Scale the model about its local origin so the shell pokes out past the mesh.
                const glm::mat4 model = transformComponent->worldMatrix()
                    * glm::scale(glm::mat4(1.0f), glm::vec3(renderComponent->outlineScale));
                VertexUniform vsu{ projection * view * model, model, glm::mat4(1.0f) };
                SDL_PushGPUVertexUniformData(commandBuffer, 0, &vsu, sizeof(vsu));

                const glm::vec4 outlineColor{ renderComponent->outlineColor, 1.0f };
                SDL_PushGPUFragmentUniformData(commandBuffer, 0, &outlineColor, sizeof(outlineColor));

                for (const Submesh& submesh : mesh->submeshes) {
                    SDL_DrawGPUIndexedPrimitives(renderPass, submesh.indexCount, 1,
                        submesh.indexOffset, 0, 0);
                    drawCallsLastFrame++;
                }
            }
        }

        // Editor grid, under the scene geometry (drawn first, depth-tested). The fragment shader
        // fades each line radially around the camera using this uniform.
        if (showGrid && gridLineRenderer) {
            const float snappedX = std::floor(camXform->getPosition().x / gridSpacing) * gridSpacing;
            const float snappedZ = std::floor(camXform->getPosition().z / gridSpacing) * gridSpacing;
            GridFadeUniform gridFade{
                { snappedX, snappedZ },
                static_cast<float>(gridExtent) * gridSpacing,
                gridOpacity
            };
            gridLineRenderer->draw(renderPass, commandBuffer,
                resourceManager->getPipeline(PipelineType::Grid), projection * view,
                &gridFade, sizeof(gridFade));
        }

        // Physics debug wireframe on top of the scene (world-space verts, depth-tested, no write).
        if (showPhysicsShapes && debugLineRenderer) {
            debugLineRenderer->draw(renderPass, commandBuffer,
                resourceManager->getPipeline(PipelineType::DebugLine), projection * view);
        }

        // Light gizmos, on top of the scene using the same flat line pipeline.
        if (showLightGizmos && gizmoLineRenderer) {
            gizmoLineRenderer->draw(renderPass, commandBuffer,
                resourceManager->getPipeline(PipelineType::DebugLine), projection * view);
        }

        // Editor icons: camera-facing sprites for lights and inactive cameras.
        if (showEditorIcons && billboardRenderer) {
            std::vector<BillboardItem> icons;
            SDL_GPUSampler* sampler = resourceManager->getSampler(SamplerType::LinearClamp);
            const Texture* lightIcon  = resourceManager->getTexture("textures/icons/lightbulb_icon.png", true).get();
            const Texture* cameraIcon = resourceManager->getTexture("textures/icons/camera_icon.png", true).get();
            for (const auto& entity : entities | std::views::values) {
                if (entity == nullptr) continue;
                const auto* transform = entity->getComponent<TransformComponent>();
                if (transform == nullptr) continue;

                const Texture* icon = nullptr;
                if (entity->getComponent<LightComponent>() != nullptr) {
                    icon = lightIcon;
                } else if (entity->getComponent<CameraComponent>() != nullptr && entity.get() != activeCamera) {
                    icon = cameraIcon;
                }
                if (icon == nullptr) continue;
                // World-space so parented lights/cameras get their icon where they actually are.
                icons.push_back({ glm::vec3(transform->worldMatrix()[3]), kEditorIconSize, icon, sampler });
            }
            billboardRenderer->draw(renderPass, commandBuffer,
                resourceManager->getPipeline(PipelineType::Billboard), view, projection, icons);
        }

        SDL_EndGPURenderPass(renderPass);

        // Blit the scaled scene target up/down to the swapchain (linear filter). This is where
        // resolution scale takes effect; ImGui then draws on top at native swapchain resolution.
        SDL_GPUBlitInfo blit = {};
        blit.source.texture      = sceneColorTexture;
        blit.source.w            = static_cast<Uint32>(w);
        blit.source.h            = static_cast<Uint32>(h);
        blit.destination.texture = swapchainTexture;
        blit.destination.w       = swapchainW;
        blit.destination.h       = swapchainH;
        blit.load_op             = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter              = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(commandBuffer, &blit);

        // UI pass: composite ImGui on top of the blitted scene. Color-only (no depth target).
        SDL_GPUColorTargetInfo uiTargetInfo = { nullptr };
        uiTargetInfo.texture  = swapchainTexture;
        uiTargetInfo.load_op  = SDL_GPU_LOADOP_LOAD;
        uiTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* uiPass = SDL_BeginGPURenderPass(commandBuffer, &uiTargetInfo, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), commandBuffer, uiPass);
        SDL_EndGPURenderPass(uiPass);

        SDL_SubmitGPUCommandBuffer(commandBuffer);
        return 0;
    }

    void Engine::handleInput(const SDL_KeyboardEvent &keyboard_event) {
        if (keyboard_event.key == SDLK_TAB) {
            showDebugWindow = !showDebugWindow;
        }
    }

    Entity* Engine::addEntity() {
        entityCounter++;
        entities[entityCounter] = std::make_unique<Entity>(entityCounter);
        return entities[entityCounter].get();
    }

    Entity* Engine::addEntityWithId(const Uint32 id) {
        entities[id] = std::make_unique<Entity>(id);
        // keep the counter ahead of every loaded id so new entities don't reuse one
        if (static_cast<int>(id) > entityCounter) entityCounter = static_cast<int>(id);
        return entities[id].get();
    }

    void Engine::clearScene() {
        entities.clear();
        activeCamera = nullptr;
        entityCounter = 0;
    }

    Entity* Engine::getEntity(const Uint32 id) {
        if (!entities.contains(id)) return nullptr;
        return entities[id].get();
    }

    bool Engine::reparent(const Uint32 childId, const Uint32 parentId) {
        Entity* child = getEntity(childId);
        if (child == nullptr) return false;

        Entity* newParent = nullptr;
        if (parentId != 0) {
            if (childId == parentId) return false;
            newParent = getEntity(parentId);
            if (newParent == nullptr) return false;
            // Walk up from the prospective parent; if we reach child, this would form a cycle.
            for (Entity* ancestor = newParent; ancestor != nullptr; ancestor = ancestor->parent) {
                if (ancestor == child) return false;
            }
        }

        if (child->parent != nullptr) {
            auto& siblings = child->parent->children;
            for (auto it = siblings.begin(); it != siblings.end(); ++it) {
                if (*it == child){
                    siblings.erase(it); break;
                }
            }
        }

        child->parent = newParent;
        if (newParent != nullptr) newParent->children.push_back(child);
        return true;
    }

    void Engine::removeEntity(const Uint32 id) {
        Entity* entity = getEntity(id);
        if (entity == nullptr) return;

        if (entity->parent != nullptr) {
            auto& siblings = entity->parent->children;
            for (auto it = siblings.begin(); it != siblings.end(); ++it) {
                if (*it == entity) { siblings.erase(it); break; }
            }
            entity->parent = nullptr;
        }

        // Gather the subtree (this id plus every descendant) before erasing anything.
        std::vector<Uint32> toRemove{ id };
        for (size_t i = 0; i < toRemove.size(); ++i) {
            Entity* current = getEntity(toRemove[i]);
            if (current == nullptr) continue;
            for (Entity* child : current->children) toRemove.push_back(child->getId());
        }

        for (const Uint32 removeId : toRemove) {
            if (activeCamera == getEntity(removeId)) activeCamera = nullptr;
            entities.erase(removeId);
        }
    }

    void Engine::setActiveCamera(Uint32 id) {
        if (!entities.contains(id)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "setActiveCamera tried to set to a null entity! %d", id);
            return;
        }
        activeCamera = entities[id].get();
    }

    bool Engine::getCameraMatrices(glm::mat4& outView, glm::mat4& outProjection) const {
        if (activeCamera == nullptr) return false;
        auto* camTransform = activeCamera->getComponent<TransformComponent>();
        auto* camComp  = activeCamera->getComponent<CameraComponent>();
        if (camTransform == nullptr || camComp == nullptr) return false;

        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        if (w == 0 || h == 0) return false;

        outView = glm::inverse(camTransform->worldMatrix());
        outProjection = camComp->projectionMatrix(static_cast<float>(w) / static_cast<float>(h));
        return true;
    }

    bool Engine::screenPointToRay(float screenX, float screenY,
                                  glm::vec3& outOrigin, glm::vec3& outDir) const {
        glm::mat4 view, projection;
        if (!getCameraMatrices(view, projection)) return false;

        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        if (w == 0 || h == 0) return false;

        const glm::mat4 invViewProj = glm::inverse(projection * view);

        // NDC = normalized device coordinates: the [-1,1] square the screen maps to after
        // projection. Convert the pixel to NDC (flip y since the screen origin is top-left).
        const float ndcX = 2.0f * screenX / static_cast<float>(w) - 1.0f;
        const float ndcY = 1.0f - 2.0f * screenY / static_cast<float>(h);

        // near/far in NDC: z=0 is near, z=1 is far (GLM_FORCE_DEPTH_ZERO_TO_ONE)
        glm::vec4 nearH = invViewProj * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
        glm::vec4 farH  = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
        const glm::vec3 nearPoint = glm::vec3(nearH) / nearH.w;
        const glm::vec3 farPoint  = glm::vec3(farH)  / farH.w;

        outOrigin = activeCamera->getComponent<TransformComponent>()->getPosition();
        outDir = glm::normalize(farPoint - nearPoint);
        return true;
    }

    void Engine::initializeImGui() const{
        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable docking for editor panels
        io.ConfigDragClickToInputText = true;                     // click a drag widget to type a value

        // Setup Dear ImGui style
        ImGui::StyleColorsDark();
        //ImGui::StyleColorsLight();

        // Setup scaling


        float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(main_scale);
        style.FontScaleDpi = main_scale;
        // if SRGB update colors so it looks right after gamma correctness
        if (bUsingSRGB) {
            // ref: https://github.com/ocornut/imgui/issues/8271#issuecomment-2564954070
            // Go through every colour and convert it to linear
            // This is because ImGui uses linear colours but we are using sRGB
            // This is a simple approximation of the conversion
            for (auto & col : style.Colors) {
                /*float linear = (srgb <= 0.04045f) ? srgb / 12.92f : pow((srgb + 0.055f)
                 * / 1.055f, 2.4f);*/

                col.x = col.x <= 0.04045f ? col.x / 12.92f
                                          : pow((col.x + 0.055f) / 1.055f, 2.4f);
                col.y = col.y <= 0.04045f ? col.y / 12.92f
                                          : pow((col.y + 0.055f) / 1.055f, 2.4f);
                col.z = col.z <= 0.04045f ? col.z / 12.92f
                                          : pow((col.z + 0.055f) / 1.055f, 2.4f);
            }
        }

        // Setup Platform/Renderer backends
        ImGui_ImplSDL3_InitForSDLGPU(window);
        ImGui_ImplSDLGPU3_InitInfo init_info = {};
        init_info.Device = device;
        init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
        init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;                      // Only used in multi-viewports mode.
        init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;  // Only used in multi-viewports mode.
        init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
        ImGui_ImplSDLGPU3_Init(&init_info);

        //TODO load other fonts


    }

    void Engine::uiTick(){
        ZoneScoped;
        // reference: https://github.com/ocornut/imgui/blob/master/examples/example_sdl3_sdlgpu3/main.cpp
        // Start the Dear ImGui frame
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGuiIO& io = ImGui::GetIO();
        io.FontGlobalScale = uiScale;

        // ImGui::ShowDemoWindow(); // Show demo window! :)

        // Debug window
        if (showDebugWindow) {
            ImGui::Begin("yellowtail!");
            ImGui::Text("Debug Window...");
            ImGui::ColorEdit3("Clear Color", reinterpret_cast<float*>(&clear_color));
            ImGui::SliderInt("FPS Lock", &framerateLock, -1, 999);

            if (ImGui::CollapsingHeader("Display")) {
                const char* windowModeNames[] = { "Windowed", "Borderless", "Fullscreen" };
                int windowModeIdx = static_cast<int>(windowMode);
                if (ImGui::Combo("Window Mode", &windowModeIdx, windowModeNames, 3)) {
                    setWindowMode(static_cast<WindowMode>(windowModeIdx));
                }

                // Resolution is only a real choice in Fullscreen. Windowed resizes by dragging,
                // Borderless follows the desktop; in those modes we just show the current size.
                if (windowMode == WindowMode::Fullscreen) {
                    // Default selection is the desktop mode when no explicit resolution is set.
                    int selW = fullscreenWidth, selH = fullscreenHeight;
                    if (selW == 0) {
                        SDL_DisplayID d = targetDisplay ? targetDisplay : SDL_GetDisplayForWindow(window);
                        if (const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(d)) { selW = dm->w; selH = dm->h; }
                    }
                    std::vector<glm::ivec2> resolutions = getAvailableResolutions(targetDisplay);
                    std::vector<std::string> resLabels;
                    std::vector<const char*> resNames;
                    int resIdx = 0;
                    resLabels.reserve(resolutions.size());
                    for (size_t i = 0; i < resolutions.size(); ++i) {
                        resLabels.push_back(std::to_string(resolutions[i].x) + " x " + std::to_string(resolutions[i].y));
                        if (resolutions[i].x == selW && resolutions[i].y == selH) resIdx = static_cast<int>(i);
                    }
                    for (auto& s : resLabels) resNames.push_back(s.c_str());
                    ImGui::BeginDisabled(resNames.empty());
                    if (ImGui::Combo("Resolution", &resIdx, resNames.data(), static_cast<int>(resNames.size()))) {
                        setResolution(resolutions[resIdx].x, resolutions[resIdx].y);
                    }
                    ImGui::EndDisabled();
                } else {
                    int cw, ch;
                    SDL_GetWindowSize(window, &cw, &ch);
                    ImGui::Text("Resolution: %d x %d", cw, ch);
                }

                // Monitor list, with a leading "Auto" entry that maps to display id 0.
                int displayCount = 0;
                SDL_DisplayID* displays = SDL_GetDisplays(&displayCount);
                std::vector<SDL_DisplayID> monitorIds{ 0 };
                std::vector<std::string> monitorLabels{ "Auto (Primary)" };
                for (int i = 0; i < displayCount; ++i) {
                    const char* name = SDL_GetDisplayName(displays[i]);
                    monitorIds.push_back(displays[i]);
                    monitorLabels.emplace_back(name ? name : "Display");
                }
                if (displays) SDL_free(displays);
                int monitorIdx = 0;
                for (size_t i = 0; i < monitorIds.size(); ++i)
                    if (monitorIds[i] == targetDisplay) monitorIdx = static_cast<int>(i);
                std::vector<const char*> monitorNames;
                monitorNames.reserve(monitorLabels.size());
                for (auto& s : monitorLabels) monitorNames.push_back(s.c_str());
                if (ImGui::Combo("Monitor", &monitorIdx, monitorNames.data(), static_cast<int>(monitorNames.size()))) {
                    setTargetDisplay(monitorIds[monitorIdx]);
                }

                ImGui::SliderFloat("Resolution Scale", &resolutionScale, 0.25f, 2.0f, "%.2fx");
                ImGui::SliderFloat("UI Scale", &uiScale, 0.5f, 3.0f, "%.2fx");
            }

            const char* presentModeNames[] = { "Vsync", "Mailbox", "Immediate" };
            const SDL_GPUPresentMode presentModeValues[] = {
                SDL_GPU_PRESENTMODE_VSYNC, SDL_GPU_PRESENTMODE_MAILBOX, SDL_GPU_PRESENTMODE_IMMEDIATE
            };
            int presentModeIdx = 0;
            for (int i = 0; i < 3; ++i) if (presentModeValues[i] == presentMode) presentModeIdx = i;
            if (ImGui::Combo("Present Mode", &presentModeIdx, presentModeNames, 3)) {
                setPresentMode(presentModeValues[presentModeIdx]);
            }

            const char* logLevelNames[] = { "Verbose", "Debug", "Info", "Warn", "Error" };
            const SDL_LogPriority logLevelValues[] = {
                SDL_LOG_PRIORITY_VERBOSE, SDL_LOG_PRIORITY_DEBUG, SDL_LOG_PRIORITY_INFO,
                SDL_LOG_PRIORITY_WARN, SDL_LOG_PRIORITY_ERROR
            };
            int logLevelIdx = 2;
            for (int i = 0; i < 5; ++i) if (logLevelValues[i] == logPriority) logLevelIdx = i;
            if (ImGui::Combo("Log Verbosity", &logLevelIdx, logLevelNames, 5)) {
                logPriority = logLevelValues[logLevelIdx];
                SDL_SetLogPriorities(logPriority);
            }

            ImGui::Checkbox("Show Physics Shapes", &showPhysicsShapes);

            ImGui::ColorEdit3("Ambient Light", (float*)&ambientDebug);
            ImGui::SliderFloat("Ambient Intensity", &ambientIntensity, 0.0f, 10.0f);
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
            ImGui::Text("Draw Calls Last Frame %d", drawCallsLastFrame);
            ImGui::Text("Point Shadows: %d lights, %d draws, %d culled, %d regen",
                        pointShadowRenderer->getActiveLights(),
                        pointShadowRenderer->getCasterDraws(),
                        pointShadowRenderer->getCulledCasters(),
                        pointShadowRenderer->getSlotsRegenerated());
            ImGui::End();
        }

        // The app (editor) contributes its own windows inside the same ImGui frame.
        if (app) app->uiTick();
    }

    void Engine::renderImGui(SDL_GPUCommandBuffer* commandBuffer, Uint32 fbWidth, Uint32 fbHeight){
        ZoneScoped;
        // Closes the ImGui frame started in uiTick() and stages the draw list.
        // PrepareDrawData copies vertex/index buffers to GPU memory; this MUST happen
        // outside any render pass (SDL_GPU forbids buffer uploads inside a pass).

        // Pin ImGui's framebuffer to the swapchain we're actually rendering into. On mode changes
        // (e.g. fullscreen->borderless) the drawable size lags ImGui's window size by a frame;
        // without this the backend emits a scissor larger than the render pass and Metal aborts.
        // Adjust FramebufferScale (not DisplaySize) so the point-based UI layout stays correct.
        ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
            io.DisplayFramebufferScale = ImVec2(static_cast<float>(fbWidth) / io.DisplaySize.x, static_cast<float>(fbHeight) / io.DisplaySize.y);
        }

        ImGui::Render();
        ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), commandBuffer);
    }

    void Engine::shutdownImGui(){
        ImGui_ImplSDL3_Shutdown();
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui::DestroyContext();
    }
} // ytail