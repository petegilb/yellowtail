//
// Omnidirectional (cube-map) shadows for point lights.
//

#include "PointShadowRenderer.h"

#include "../managers/ResourceManager.h"

namespace ytail {
    PointShadowRenderer::PointShadowRenderer(SDL_GPUDevice* inDevice, ResourceManager* inResources)
        : device(inDevice), resources(inResources) {}

    PointShadowRenderer::~PointShadowRenderer() {
        if (device && cubeArray) SDL_ReleaseGPUTexture(device, cubeArray);
    }

    void PointShadowRenderer::ensureTexture() {
        if (cubeArray && currentRes == faceResolution && currentSlots == maxShadowedPoints) return;
        if (cubeArray) SDL_ReleaseGPUTexture(device, cubeArray);

        SDL_GPUTextureCreateInfo info = {};
        info.type   = SDL_GPU_TEXTURETYPE_CUBE_ARRAY;
        info.format = resources->getShadowMapFormat();
        // Rendered to by the shadow pass, sampled by the lit shader.
        info.usage  = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width  = static_cast<Uint32>(faceResolution);
        info.height = static_cast<Uint32>(faceResolution);
        info.layer_count_or_depth = static_cast<Uint32>(6 * maxShadowedPoints); // 6 faces per slot
        info.num_levels = 1;

        cubeArray = SDL_CreateGPUTexture(device, &info);
        if (cubeArray == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create point-shadow cube array: %s", SDL_GetError());
            currentRes = currentSlots = 0;
            return;
        }
        currentRes   = faceResolution;
        currentSlots = maxShadowedPoints;
        pendingClear = true;
    }

    void PointShadowRenderer::clearIfPending(SDL_GPUCommandBuffer* commandBuffer) {
        if (!pendingClear || cubeArray == nullptr) return;

        // One depth pass per layer clears it to the far plane (1.0).
        const int layers = 6 * currentSlots;
        for (int layer = 0; layer < layers; ++layer) {
            SDL_GPUDepthStencilTargetInfo target = {};
            target.texture          = cubeArray;
            target.layer            = static_cast<Uint8>(layer);
            target.clear_depth      = 1.0f;
            target.load_op          = SDL_GPU_LOADOP_CLEAR;
            target.store_op         = SDL_GPU_STOREOP_STORE;
            target.stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE;
            target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            target.cycle            = false;

            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commandBuffer, nullptr, 0, &target);
            SDL_EndGPURenderPass(pass);
        }
        pendingClear = false;
    }
} // ytail
