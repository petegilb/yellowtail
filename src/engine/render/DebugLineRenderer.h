//
// Created by Peter Gilbert on 7/14/26.
//

#ifndef YELLOWTAIL_DEBUGLINERENDERER_H
#define YELLOWTAIL_DEBUGLINERENDERER_H

#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include "JoltDebugVertex.h"

namespace ytail {
    // Uploads debug line vertices into a growing GPU buffer each frame and draws them with the
    // DebugLine pipeline. Vertices are world-space, so draw() only needs view*proj.
    class DebugLineRenderer {
    public:
        explicit DebugLineRenderer(SDL_GPUDevice* inDevice);
        ~DebugLineRenderer();

        // stage this frame's lines into the GPU buffer; runs a copy pass so call before BeginRenderPass
        void upload(SDL_GPUCommandBuffer* commandBuffer, const std::vector<JoltDebugVertex>& lines);

        // draw the uploaded lines; call inside a render pass that has the depth target bound.
        // fragmentUniform (when set) is pushed to fragment slot 0 - the grid uses it for its fade.
        void draw(SDL_GPURenderPass* renderPass, SDL_GPUCommandBuffer* commandBuffer,
                  SDL_GPUGraphicsPipeline* pipeline, const glm::mat4& viewProj,
                  const void* fragmentUniform = nullptr, Uint32 fragmentUniformSize = 0);

    private:
        // grow the vertex + transfer buffers if the current frame needs more room
        void ensureCapacity(Uint32 neededVertices);

        SDL_GPUDevice* device = nullptr;
        SDL_GPUBuffer* vertexBuffer = nullptr;
        SDL_GPUTransferBuffer* transferBuffer = nullptr;
        Uint32 capacity = 0;      // vertices the buffers can hold
        Uint32 vertexCount = 0;   // vertices uploaded this frame
    };
} // ytail

#endif //YELLOWTAIL_DEBUGLINERENDERER_H
