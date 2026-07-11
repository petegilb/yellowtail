//
// Created by Peter Gilbert on 6/27/26.
//

#include "Engine.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include "components/RenderComponent.h"
#include "components/TransformComponent.h"
#include "components/CameraComponent.h"
#include "components/LightComponent.h"
#include "managers/ResourceManager.h"

namespace ytail {
    Engine::Engine() {
        SDL_Log("Starting yellowtail...");
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Error starting SDL! %s", SDL_GetError());
        }
    }

    Engine::~Engine() {
        SDL_Log("Ending yellowtail...");

        // Wait for any in-flight frames to finish before we tear down GPU-backed
        // resources (ImGui backend owns pipelines, buffers, fonts).
        if (device) SDL_WaitForGPUIdle(device);
        shutdownImGui();

        // Release everything that owns GPU resources BEFORE destroying the device.
        entities.clear(); // RenderComponents drop their shared_ptr<Mesh>/Material
        resourceManager.reset(); // frees pipelines, samplers, and cached meshes/textures
        if (device && depthTexture) SDL_ReleaseGPUTexture(device, depthTexture);

        SDL_ShaderCross_Quit();
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
        window = SDL_CreateWindow("yellowtail.", 1280, 720, 0);
        if (!window) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Error creating SDL window! %s", SDL_GetError());
            return -1;
        }
        if (!SDL_ClaimWindowForGPUDevice(device, window)){
            SDL_Log("ClaimWindow failed");
            return -1;
        }
        // Output encode: render in linear, let the swapchain re-encode to sRGB on scanout.
        // SDR_LINEAR makes the swapchain the *_SRGB format, so a linear fragment result is
        // gamma-encoded by the hardware on write. Pairs with the per-texture srgb decode in
        // getTexture() to close the loop on a fully linear lighting pipeline.
        // Must run before pipelines/ImGui query the swapchain format (they pick up the _SRGB variant).
        if (SDL_WindowSupportsGPUSwapchainComposition(device, window,
                SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR)) {
            SDL_SetGPUSwapchainParameters(device, window,
                SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR, SDL_GPU_PRESENTMODE_VSYNC);
            bUsingSRGB = true;
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "SDR_LINEAR swapchain unsupported; output will not be gamma-correct");
        }
        if (!SDL_ShaderCross_Init()) {
            SDL_Log("ShaderCross_Init failed");
            return -1;
        }
        BasePath = SDL_GetBasePath();
        resourceManager = std::make_unique<ResourceManager>(device, window, BasePath);
        initializeImGui();

        // scene setup
        Entity* camera = addEntity();
        const auto camTransform = camera->addComponent<TransformComponent>();
        camera->addComponent<CameraComponent>();
        setActiveCamera(camera->getId());
        camTransform->position = glm::vec3(0.0f, 3.0f, 5.0f);   // back up 5 units, looking down -Z toward origin
        camTransform->setRotationEuler(glm::vec3(-30.0f, 0.0f, 0.0f));

        Entity* light0 = addEntity();
        const auto lightTransform = light0->addComponent<TransformComponent>();
        const auto lightComponent = light0->addComponent<LightComponent>();
        lightComponent->color = glm::vec3(1.0f, 1.0f, 1.0f);
        lightComponent->intensity = 1.0f;
        lightTransform->position = glm::vec3(1.2f, 1.0f, 2.0f);  // camera side, up and to the right

        // create material
        auto material = std::make_shared<Material>();
        material->pipelineType = PipelineType::LitStatic;
        // diffuse (color -> sRGB) at t0, specular (data mask -> linear) at t1, in slot order.
        SDL_GPUSampler* sampler = resourceManager->getSampler(SamplerType::LinearWrap);
        material->textures.push_back({ resourceManager->getTexture("textures/container2.png", true), sampler });
        material->textures.push_back({ resourceManager->getTexture("textures/container2_specular.png", false), sampler });
        // material uniform (b1 space3): just shininess for now.
        MaterialUniform matUniform{};
        // https://learnopengl.com/Advanced-Lighting/Advanced-Lighting
        // hardcoded exponent value to 64
        matUniform.shininess = 64.0f;
        material->uniformData.resize(sizeof(matUniform));
        SDL_memcpy(material->uniformData.data(), &matUniform, sizeof(matUniform));

        Entity* cube = addEntity();
        cube->addComponent<TransformComponent>();
        auto cubeRender = cube->addComponent<RenderComponent>();
        // add mesh and materials to render component
        std::shared_ptr<Mesh> cubeMesh = resourceManager->getMesh("models/cube.glb");
        cubeRender->setMesh(cubeMesh);
        cubeRender->addMaterial(material);

        Entity* cube2 = addEntity();
        auto cube2Transform = cube2->addComponent<TransformComponent>();
        auto cube2Render = cube2->addComponent<RenderComponent>();
        cube2Render->setMesh(cubeMesh);
        cube2Render->addMaterial(material);
        cube2Render->outline = true;
        cube2Transform->position = glm::vec3(2.0f, -1.0f, -5.0f);;

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
        }
    }

    void Engine::quit() {
        bRunning = false;
    }

    void Engine::tick(float deltaTime) {
        inputTick();
        updateTick();
        renderTick();
    }

    void Engine::inputTick() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
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
                default: ;
            }
        }
    }

    void Engine::updateTick() {
        updateImGui();
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

    int Engine::renderTick() {
        drawCallsLastFrame = 0;
        // just found this reference: https://dawaralvi.github.io/sdl-gpu/
        // get the active camera + aspect ratio
        if (activeCamera == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Render camera is null!");
            return -1;
        }
        auto* camXform = activeCamera->getComponent<TransformComponent>();
        auto* camComp  = activeCamera->getComponent<CameraComponent>();
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (h == 0) return 0;
        ensureDepthTexture(w, h);
        const float aspect = static_cast<float>(w) / static_cast<float>(h);
        glm::mat4 view = glm::inverse(camXform->modelMatrix());
        glm::mat4 projection = camComp->projectionMatrix(aspect);

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
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window, &swapchainTexture, nullptr, nullptr)) {
            SDL_Log("WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
            SDL_SubmitGPUCommandBuffer(commandBuffer);
            return -1;
        }
        if (swapchainTexture == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Swapchain texture is null! can't render!");
            SDL_SubmitGPUCommandBuffer(commandBuffer);
            return -1;
        }

        SDL_GPUColorTargetInfo colorTargetInfo = { nullptr };
        colorTargetInfo.texture = swapchainTexture;
        colorTargetInfo.clear_color = SDL_FColor{ clear_color.x, clear_color.y, clear_color.z, clear_color.w };
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;

        // ImGui uploads its vertex/index data here -- needs to be before BeginRenderPass
        renderImGui(commandBuffer);

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

        // Per-frame lighting: camera position + scene ambient + the first light in the scene.
        // Pushed once to fragment slot 0 (FrameLighting @ b0 space3). This state persists for
        // every draw in this command buffer, so all lit materials read the same light.
        FrameLightingUniform frameLighting{};
        frameLighting.viewPos = camXform->position;
        frameLighting.ambient = ambientLight;
        for (const auto& [lightIdx, lightEntity] : entities) {
            if (lightEntity == nullptr) continue;
            const auto* lightComp = lightEntity->getComponent<LightComponent>();
            const auto* lightXform = lightEntity->getComponent<TransformComponent>();
            if (lightComp == nullptr || lightXform == nullptr) continue;
            frameLighting.lightPos   = lightXform->position;
            frameLighting.lightColor = lightComp->color * lightComp->intensity;
            break;  // single light for now
        }
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &frameLighting, sizeof(frameLighting));

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
            const glm::mat4 model = transformComponent->modelMatrix();
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
                const glm::mat4 model = transformComponent->modelMatrix()
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

        SDL_EndGPURenderPass(renderPass);

        // UI pass: composite ImGui on top of the finished scene. Color-only (no depth target)
        SDL_GPUColorTargetInfo uiTargetInfo = colorTargetInfo;
        uiTargetInfo.load_op  = SDL_GPU_LOADOP_LOAD;
        uiTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* uiPass = SDL_BeginGPURenderPass(commandBuffer, &uiTargetInfo, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), commandBuffer, uiPass);
        SDL_EndGPURenderPass(uiPass);

        SDL_SubmitGPUCommandBuffer(commandBuffer);
        return 0;
    }

    void Engine::handleInput(const SDL_KeyboardEvent &keyboard_event) {
        if (keyboard_event.key == SDLK_ESCAPE) {
            quit();
        }
        if (keyboard_event.key == SDLK_TAB) {
            showDebugWindow = !showDebugWindow;
        }
    }

    Entity* Engine::addEntity() {
        entityCounter++;
        entities[entityCounter] = std::make_unique<Entity>(entityCounter);
        return entities[entityCounter].get();
    }

    Entity* Engine::getEntity(const Uint32 id) {
        if (!entities.contains(id)) return nullptr;
        return entities[id].get();
    }

    void Engine::setActiveCamera(Uint32 id) {
        if (!entities.contains(id)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "setActiveCamera tried to set to a null entity! %d", id);
            return;
        }
        activeCamera = entities[id].get();
    }

    void Engine::initializeImGui(){
        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

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

    void Engine::updateImGui(){
        // reference: https://github.com/ocornut/imgui/blob/master/examples/example_sdl3_sdlgpu3/main.cpp
        // Start the Dear ImGui frame
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGuiIO& io = ImGui::GetIO();

        // ImGui::ShowDemoWindow(); // Show demo window! :)

        // Debug window
        if (showDebugWindow) {
            ImGui::Begin("yellowtail!");
            ImGui::Text("Debug Window...");
            ImGui::ColorEdit3("Clear Color", (float*)&clear_color);
            ImGui::SliderInt("FPS Lock", &framerateLock, -1, 999);
            ImGui::ColorEdit3("Ambient Light", (float*)&ambientDebug);
            ImGui::SliderFloat("Ambient Intensity", &ambientIntensity, 0.0f, 10.0f);
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
            ImGui::Text("Draw Calls Last Frame %d", drawCallsLastFrame);
            ImGui::End();
        }
    }

    void Engine::renderImGui(SDL_GPUCommandBuffer* commandBuffer){
        // Closes the ImGui frame started in updateImGui() and stages the draw list.
        // PrepareDrawData copies vertex/index buffers to GPU memory; this MUST happen
        // outside any render pass (SDL_GPU forbids buffer uploads inside a pass).
        ImGui::Render();
        ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), commandBuffer);
    }

    void Engine::shutdownImGui(){
        ImGui_ImplSDL3_Shutdown();
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui::DestroyContext();
    }
} // ytail