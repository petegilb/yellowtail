//
// Draws camera-facing textured quads (editor icons for lights, cameras, ...). Reuses the standard
// vertex shader by building each quad's model matrix from the camera's right/up axes on the CPU.
//

#ifndef YELLOWTAIL_BILLBOARDRENDERER_H
#define YELLOWTAIL_BILLBOARDRENDERER_H

#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

namespace ytail {
    class Texture;

    // World-space half-height of editor billboard icons. Shared so the editor's click box
    // matches what the renderer draws.
    inline constexpr float kEditorIconSize = 0.8f;

    // One sprite to draw: an icon centered at a world position, sized in world units.
    struct BillboardItem {
        glm::vec3 position{0.0f};
        float size = 1.0f;
        const Texture* texture = nullptr;   // non-owning
        SDL_GPUSampler* sampler = nullptr;  // owned by the ResourceManager
    };

    class BillboardRenderer {
    public:
        explicit BillboardRenderer(SDL_GPUDevice* inDevice);
        ~BillboardRenderer();

        // Draw each item as a quad facing the camera. Call inside a render pass with the depth
        // target bound; view/projection come from the active camera.
        void draw(SDL_GPURenderPass* renderPass, SDL_GPUCommandBuffer* commandBuffer,
                  SDL_GPUGraphicsPipeline* pipeline, const glm::mat4& view,
                  const glm::mat4& projection, const std::vector<BillboardItem>& items);

    private:
        SDL_GPUDevice* device = nullptr;
        SDL_GPUBuffer* vertexBuffer = nullptr;  // unit quad in the XY plane
        SDL_GPUBuffer* indexBuffer = nullptr;
    };
} // ytail

#endif //YELLOWTAIL_BILLBOARDRENDERER_H
