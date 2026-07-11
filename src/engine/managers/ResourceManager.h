//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_RESOURCEMANAGER_H
#define YELLOWTAIL_RESOURCEMANAGER_H

#include <array>
#include <string>
#include <memory>
#include <unordered_map>
#include <SDL3/SDL.h>

#include "../render/Material.h"
#include "../render/Texture.h"
#include "../render/Mesh.h"

namespace ytail {
    class ResourceManager {
public:
        // window is needed to query the swapchain color format when building pipelines.
        ResourceManager(SDL_GPUDevice* inDevice, SDL_Window* inWindow, const char* inBasePath);
        ~ResourceManager() {
            for (auto* s : samplers) if (s) SDL_ReleaseGPUSampler(device, s);
            for (auto* p : pipelines) {
                if (p) SDL_ReleaseGPUGraphicsPipeline(device, p);
            }
        }
        // get or load the texture at the specified path.
        // srgb=true for color textures (albedo/diffuse) so the GPU does gamma-correct
        // sampling; false for data textures (normal/roughness/metallic/masks).
        std::shared_ptr<Texture> getTexture(const std::string& path, bool srgb = false);
        // get or create a cached 1x1 solid-color texture (linear)
        std::shared_ptr<Texture> getSolidTexture(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 255);
        std::shared_ptr<Texture> getSolidTexture(Uint8 x);
        // get or load the mesh at the specified path
        std::shared_ptr<Mesh> getMesh(const std::string& path);
        // get the pipeline based on the pipeline type. When outline is true, lit pipelines are
        // swapped for their stencil-stamping variant so the outline pass has a silhouette to mask.
        SDL_GPUGraphicsPipeline* getPipeline(PipelineType type, bool outline = false);

        [[nodiscard]] SDL_GPUSampler* getSampler(SamplerType type) const {
            return samplers[static_cast<size_t>(type)];
        }

        // Depth+stencil texture format chosen at construction. Engine uses this to build the
        // per-frame depth buffer so the texture and the pipelines agree on the format.
        [[nodiscard]] SDL_GPUTextureFormat getDepthStencilFormat() const { return depthStencilFormat; }

    private:
        SDL_GPUDevice* device = nullptr;
        SDL_Window* window = nullptr;
        const char* BasePath;

        // Set in the constructor to a device-supported depth+stencil format (stencil is
        // required for the outline mask). Prefers D24_S8, falls back to D32_S8.
        SDL_GPUTextureFormat depthStencilFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;

        // Resolve an assets-relative path (e.g. "models/cube.gltf") to an absolute path
        // next to the executable. Callers pass the short path; the cache still keys on it.
        std::string resolveAssetPath(const std::string& path) const {
            return std::string(BasePath) + "assets/" + path;
        }
        std::unordered_map<std::string, std::shared_ptr<Texture>> textures;
        std::unordered_map<std::string, std::shared_ptr<Mesh>> meshes;

        // all samplers that exist! initialized on creation of the resource manager.
        std::array<SDL_GPUSampler*, static_cast<size_t>(SamplerType::Count)> samplers{};

        // all pipelines that exist currently : {} zero-inits to nullptr
        std::array<SDL_GPUGraphicsPipeline*, static_cast<size_t>(PipelineType::Count)> pipelines{};

        // load a shader for a pipeline. this exists as a private helper for getPipeline since
        // SDL_GPUShader are transient and should be released after SDL_CreateGPUGraphicsPipeline()
        SDL_GPUShader* loadShader(
            SDL_GPUDevice* inDevice,
            const char* shaderFilename,
            Uint32 samplerCount,
            Uint32 uniformBufferCount,
            Uint32 storageBufferCount,
            Uint32 storageTextureCount
        );

        void initializePipelines();
        void initializeSamplers();
    };
} // ytail

#endif //YELLOWTAIL_RESOURCEMANAGER_H