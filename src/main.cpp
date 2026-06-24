#include <iostream>

#include <SDL3/SDL.h>
#include <SDL3_shadercross/SDL_shadercross.h>

class Yellowtail {
public:
    Yellowtail() {
        std::cout << "Starting yellowtail..." << std::endl;
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::cout << "Error starting SDL!" << std::endl;
        }
        InitializeAssetLoader();
    }
    ~Yellowtail() {
        std::cout << "Ending yellowtail..." << std::endl;
        if (pipeline) SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
        SDL_ShaderCross_Quit();
        if (device && window) SDL_ReleaseWindowFromGPUDevice(device, window);
        if (device) SDL_DestroyGPUDevice(device);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    int run() {
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
            std::cout << "Error creating SDL window!" << std::endl;
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
        // load shaders for triangle
        drawTriangle();
        mainLoop();
        return 0;
    }

    void mainLoop() {
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

    void quit() {
        bRunning = false;
    }

    // Advance a frame (one iteration in the main loop)
    void tick(float deltaTime) {
        inputTick();
        updateTick();
        renderTick();
    }

    void inputTick() {
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

    void updateTick() {

    }

    int renderTick() {
        // command buffer of commands to be executed by SDL3_gpu
        // does not need to be freed and can only be used on the thread in which it is created (render thread?)
        SDL_GPUCommandBuffer* cmdbuf = SDL_AcquireGPUCommandBuffer(device);
        if (cmdbuf == nullptr){
            SDL_Log("AcquireGPUCommandBuffer failed: %s", SDL_GetError());
            return -1;
        }
        // a swapchain is a queue of framebuffers (or textures) that are swapped to the screen one after another
        // allow rendering into one texture while another is being displayed
        // help avoid tearing, flickering, and stuttering
        SDL_GPUTexture* swapchainTexture;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmdbuf, window, &swapchainTexture, nullptr, nullptr)) {
            SDL_Log("WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
            return -1;
        }
        if (swapchainTexture != nullptr)
        {
            SDL_GPUColorTargetInfo colorTargetInfo = { 0 };
            colorTargetInfo.texture = swapchainTexture;
            colorTargetInfo.clear_color = (SDL_FColor){ 0.3f, 0.4f, 0.5f, 1.0f };
            colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
            colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(cmdbuf, &colorTargetInfo, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(renderPass, pipeline);
            SDL_DrawGPUPrimitives(renderPass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(renderPass);
        }

        SDL_SubmitGPUCommandBuffer(cmdbuf);
        return 0;
    }

    void handleInput(const SDL_KeyboardEvent& keyboard_event) {
        if (keyboard_event.key == SDLK_ESCAPE) {
            quit();
        }
    }

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    void drawTriangle() {
        SDL_GPUShader* vertexShader = loadShader(device, "Triangle.vert", 0, 0, 0, 0);
        if (vertexShader == nullptr)
        {
            SDL_Log("Failed to create vertex shader!");
        }

        SDL_GPUShader* fragmentShader = loadShader(device, "SolidColor.frag", 0, 0, 0, 0);
        if (fragmentShader == nullptr)
        {
            SDL_Log("Failed to create fragment shader!");
        }

        // Named local so the pointer below stays valid until SDL_CreateGPUGraphicsPipeline
        // reads it — a compound literal would be destroyed at the end of the initializer in C++.
        SDL_GPUColorTargetDescription colorTargetDesc = {
            .format = SDL_GetGPUSwapchainTextureFormat(device, window),
        };

        SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {
            .vertex_shader = vertexShader,
            .fragment_shader = fragmentShader,
            .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
            .target_info = {
                .color_target_descriptions = &colorTargetDesc,
                .num_color_targets = 1,
            },
        };

        pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineCreateInfo);
        if (pipeline == nullptr){
            SDL_Log("Failed to create fill pipeline!");
        }
    }

    SDL_GPUShader* loadShader(
        SDL_GPUDevice* inDevice,
        const char* shaderFilename,
        Uint32 samplerCount,
        Uint32 uniformBufferCount,
        Uint32 storageBufferCount,
        Uint32 storageTextureCount
    ) {
        // We ship HLSL source and cross-compile to the backend's format at runtime
        // via SDL_shadercross (HLSL -> SPIR-V -> native), so the same .hlsl works on
        // Vulkan/D3D12/Metal with no offline compile step.
        SDL_ShaderCross_ShaderStage stage;
        if (SDL_strstr(shaderFilename, ".vert"))
        {
            stage = SDL_SHADERCROSS_SHADERSTAGE_VERTEX;
        }
        else if (SDL_strstr(shaderFilename, ".frag"))
        {
            stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
        }
        else
        {
            SDL_Log("Invalid shader stage!");
            return nullptr;
        }

        char fullPath[256];
        SDL_snprintf(fullPath, sizeof(fullPath), "%sassets/shaders/%s.hlsl", BasePath, shaderFilename);

        size_t codeSize;
        void* code = SDL_LoadFile(fullPath, &codeSize);
        if (code == nullptr)
        {
            SDL_Log("Failed to load shader from disk! %s", fullPath);
            return nullptr;
        }

        // 1) HLSL -> SPIR-V (DXC). SDL_LoadFile null-terminates, so it's a valid C string.
        SDL_ShaderCross_HLSL_Info hlslInfo = {};
        hlslInfo.source = (const char *)code;
        hlslInfo.entrypoint = "main";
        hlslInfo.shader_stage = stage;

        size_t spirvSize;
        void* spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlslInfo, &spirvSize);
        SDL_free(code);
        if (spirv == nullptr)
        {
            SDL_Log("Failed to compile HLSL to SPIR-V: %s", SDL_GetError());
            return nullptr;
        }

        // 2) SPIR-V -> native SDL_GPUShader (transpiles to MSL/DXIL/SPIRV for this device).
        SDL_ShaderCross_SPIRV_Info spirvInfo = {};
        spirvInfo.bytecode = (const Uint8 *)spirv;
        spirvInfo.bytecode_size = spirvSize;
        spirvInfo.entrypoint = "main";
        spirvInfo.shader_stage = stage;

        SDL_ShaderCross_GraphicsShaderResourceInfo resourceInfo = {};
        resourceInfo.num_samplers = samplerCount;
        resourceInfo.num_uniform_buffers = uniformBufferCount;
        resourceInfo.num_storage_buffers = storageBufferCount;
        resourceInfo.num_storage_textures = storageTextureCount;

        SDL_GPUShader* shader =
            SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(inDevice, &spirvInfo, &resourceInfo, 0);
        SDL_free(spirv);
        if (shader == nullptr)
        {
            SDL_Log("Failed to create shader!");
            return nullptr;
        }

        return shader;
    }

    void InitializeAssetLoader(){
        BasePath = SDL_GetBasePath();
    }

protected:
    SDL_Window* window = nullptr;
    bool bRunning = true;
    SDL_GPUDevice* device = nullptr;
    const char* BasePath = nullptr;

    // locks the framerate if greater than 0
    int framerateLock = 60;
};


// entry point
int main(int argc, char* argv[]) {
    std::cout << "Starting yellowtail process." << std::endl;
    Yellowtail Game;
    return Game.run();
}
