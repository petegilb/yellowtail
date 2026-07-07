//
// Created by Peter Gilbert on 6/27/26.
//

#include "Engine.h"

#include "components/RenderComponent.h"
#include "components/TransformComponent.h"
#include "components/CameraComponent.h"
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

        // Release everything that owns GPU resources BEFORE destroying the device.
        // These are members, so they'd otherwise be destroyed AFTER this body runs -
        // i.e. after SDL_DestroyGPUDevice - and their SDL_Release* calls would hit a
        // dangling device pointer (SIGSEGV on quit).
        entities.clear();         // RenderComponents drop their shared_ptr<Mesh>/Material
        resourceManager.reset();  // frees pipelines, samplers, and cached meshes/textures

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
        if (!SDL_ShaderCross_Init()) {
            SDL_Log("ShaderCross_Init failed");
            return -1;
        }
        BasePath = SDL_GetBasePath();
        resourceManager = std::make_unique<ResourceManager>(device, window, BasePath);

        // scene setup
        Entity* camera = addEntity();
        const auto camTransform = camera->addComponent<TransformComponent>();
        camera->addComponent<CameraComponent>();
        setActiveCamera(camera->getId());
        camTransform->position = glm::vec3(0.0f, 3.0f, 5.0f);   // back up 5 units, looking down -Z toward origin
        camTransform->setRotationEuler(glm::vec3(-30.0f, 0.0f, 0.0f));

        Entity* cube = addEntity();
        cube->addComponent<TransformComponent>();
        auto cubeRender = cube->addComponent<RenderComponent>();
        // add mesh and materials to render component
        // TODO handle when a submesh doesn't have a material
        std::shared_ptr<Mesh> cubeMesh = resourceManager->getMesh("models/cube.glb");
        cubeRender->setMesh(cubeMesh);
        // TODO how should this process work?
        auto material = std::make_shared<Material>();
        material->pipelineType = PipelineType::UnlitStatic;
        cubeRender->addMaterial(material);

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

    }

    int Engine::renderTick() {
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
        colorTargetInfo.clear_color = SDL_FColor{ 0.3f, 0.4f, 0.5f, 1.0f };
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;

        // render all pipelines for all materials here...
        // we are only using one render pass, when would we want to use more than one?
        // TODO add depth buffer here!
        SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, nullptr);

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
                if (submesh.materialSlot >= materials.size()) continue;
                const auto& material = materials[submesh.materialSlot];
                if (!material) continue;

                SDL_GPUGraphicsPipeline* pipeline = resourceManager->getPipeline(material->pipelineType);
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
                // Push the uniform data (it's a vector of raw bytes)
                if (!material->uniformData.empty()) {
                    SDL_PushGPUFragmentUniformData(commandBuffer, 0, material->uniformData.data(),
                        static_cast<Uint32>(material->uniformData.size())
                    );
                }
                // draw using all our data
                SDL_DrawGPUIndexedPrimitives(renderPass, submesh.indexCount, 1,
                    submesh.indexOffset, 0, 0
                );
            }
        }

        SDL_EndGPURenderPass(renderPass);
        SDL_SubmitGPUCommandBuffer(commandBuffer);
        return 0;
    }

    void Engine::handleInput(const SDL_KeyboardEvent &keyboard_event) {
        if (keyboard_event.key == SDLK_ESCAPE) {
            quit();
        }
    }

    Entity * Engine::addEntity() {
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
} // ytail