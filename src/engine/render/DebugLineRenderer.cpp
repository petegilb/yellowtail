//
// Created by Peter Gilbert on 7/14/26.
//

#include "DebugLineRenderer.h"

namespace ytail {
    DebugLineRenderer::DebugLineRenderer(SDL_GPUDevice* inDevice) : device(inDevice) {}

    DebugLineRenderer::~DebugLineRenderer() {
        if (device && vertexBuffer) SDL_ReleaseGPUBuffer(device, vertexBuffer);
        if (device && transferBuffer) SDL_ReleaseGPUTransferBuffer(device, transferBuffer);
    }

    void DebugLineRenderer::ensureCapacity(Uint32 neededVertices) {
        if (neededVertices <= capacity) return;

        // grow geometrically to avoid reallocating every frame the geometry changes
        Uint32 newCapacity = capacity > 0 ? capacity : 4096;
        while (newCapacity < neededVertices) newCapacity *= 2;

        if (vertexBuffer) SDL_ReleaseGPUBuffer(device, vertexBuffer);
        if (transferBuffer) SDL_ReleaseGPUTransferBuffer(device, transferBuffer);

        const Uint32 bytes = newCapacity * sizeof(JoltDebugVertex);

        SDL_GPUBufferCreateInfo vbInfo = {};
        vbInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        vbInfo.size  = bytes;
        vertexBuffer = SDL_CreateGPUBuffer(device, &vbInfo);

        SDL_GPUTransferBufferCreateInfo tbInfo = {};
        tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbInfo.size  = bytes;
        transferBuffer = SDL_CreateGPUTransferBuffer(device, &tbInfo);

        capacity = newCapacity;
    }

    void DebugLineRenderer::upload(SDL_GPUCommandBuffer* commandBuffer, const std::vector<JoltDebugVertex>& lines) {
        vertexCount = static_cast<Uint32>(lines.size());
        if (vertexCount == 0) return;

        ensureCapacity(vertexCount);
        const Uint32 bytes = vertexCount * sizeof(JoltDebugVertex);

        // cycle so we don't stomp a buffer the GPU may still be reading from last frame
        void* mapped = SDL_MapGPUTransferBuffer(device, transferBuffer, true);
        SDL_memcpy(mapped, lines.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device, transferBuffer);

        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
        SDL_GPUTransferBufferLocation src = { .transfer_buffer = transferBuffer, .offset = 0 };
        SDL_GPUBufferRegion dst = { .buffer = vertexBuffer, .offset = 0, .size = bytes };
        SDL_UploadToGPUBuffer(copyPass, &src, &dst, true);
        SDL_EndGPUCopyPass(copyPass);
    }

    void DebugLineRenderer::draw(SDL_GPURenderPass* renderPass, SDL_GPUCommandBuffer* commandBuffer,
                                 SDL_GPUGraphicsPipeline* pipeline, const glm::mat4& viewProj) {
        if (vertexCount == 0 || pipeline == nullptr) return;

        SDL_BindGPUGraphicsPipeline(renderPass, pipeline);
        SDL_PushGPUVertexUniformData(commandBuffer, 0, &viewProj, sizeof(glm::mat4));

        SDL_GPUBufferBinding binding = { .buffer = vertexBuffer, .offset = 0 };
        SDL_BindGPUVertexBuffers(renderPass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(renderPass, vertexCount, 1, 0, 0);
    }
} // ytail
