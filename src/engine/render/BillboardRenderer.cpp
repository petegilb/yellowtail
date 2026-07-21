//
// Created for editor billboard icons.
//

#include "BillboardRenderer.h"

#include "Mesh.h"                         // Vertex
#include "../components/RenderComponent.h"  // VertexUniform
#include "Texture.h"

namespace ytail {
    namespace {
        // Unit quad in the XY plane, centered on the origin, spanning [-0.5, 0.5]. UVs put (0,0)
        // at the top-left to match image space. Normal is +Z (unused by the billboard shader).
        constexpr Vertex kQuadVertices[4] = {
            { {-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f} },  // bottom-left
            { { 0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f} },  // bottom-right
            { { 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f} },  // top-right
            { {-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f} },  // top-left
        };
        constexpr Uint32 kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };
    }

    BillboardRenderer::BillboardRenderer(SDL_GPUDevice* inDevice) : device(inDevice) {
        constexpr Uint32 vertexBytes = sizeof(kQuadVertices);
        constexpr Uint32 indexBytes  = sizeof(kQuadIndices);

        SDL_GPUBufferCreateInfo vbInfo = {};
        vbInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        vbInfo.size  = vertexBytes;
        vertexBuffer = SDL_CreateGPUBuffer(device, &vbInfo);

        SDL_GPUBufferCreateInfo ibInfo = {};
        ibInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        ibInfo.size  = indexBytes;
        indexBuffer = SDL_CreateGPUBuffer(device, &ibInfo);

        SDL_GPUTransferBufferCreateInfo tbInfo = {};
        tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbInfo.size  = vertexBytes + indexBytes;
        SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(device, &tbInfo);

        void* mapped = SDL_MapGPUTransferBuffer(device, transferBuffer, false);
        SDL_memcpy(mapped, kQuadVertices, vertexBytes);
        SDL_memcpy(static_cast<Uint8*>(mapped) + vertexBytes, kQuadIndices, indexBytes);
        SDL_UnmapGPUTransferBuffer(device, transferBuffer);

        SDL_GPUCommandBuffer* uploadCmd = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(uploadCmd);
        SDL_GPUTransferBufferLocation vbSrc = { .transfer_buffer = transferBuffer, .offset = 0 };
        SDL_GPUBufferRegion vbDst = { .buffer = vertexBuffer, .offset = 0, .size = vertexBytes };
        SDL_UploadToGPUBuffer(copyPass, &vbSrc, &vbDst, false);
        SDL_GPUTransferBufferLocation ibSrc = { .transfer_buffer = transferBuffer, .offset = vertexBytes };
        SDL_GPUBufferRegion ibDst = { .buffer = indexBuffer, .offset = 0, .size = indexBytes };
        SDL_UploadToGPUBuffer(copyPass, &ibSrc, &ibDst, false);
        SDL_EndGPUCopyPass(copyPass);
        SDL_SubmitGPUCommandBuffer(uploadCmd);

        SDL_ReleaseGPUTransferBuffer(device, transferBuffer);
    }

    BillboardRenderer::~BillboardRenderer() {
        if (device && vertexBuffer) SDL_ReleaseGPUBuffer(device, vertexBuffer);
        if (device && indexBuffer)  SDL_ReleaseGPUBuffer(device, indexBuffer);
    }

    void BillboardRenderer::draw(SDL_GPURenderPass* renderPass, SDL_GPUCommandBuffer* commandBuffer,
                                 SDL_GPUGraphicsPipeline* pipeline, const glm::mat4& view,
                                 const glm::mat4& projection, const std::vector<BillboardItem>& items) {
        if (pipeline == nullptr || items.empty()) return;

        // Camera world-space right/up = rows of the view rotation, so the quad always faces us.
        const glm::vec3 right(view[0][0], view[1][0], view[2][0]);
        const glm::vec3 up(view[0][1], view[1][1], view[2][1]);
        const glm::vec3 forward = glm::cross(right, up);
        const glm::mat4 viewProj = projection * view;

        SDL_BindGPUGraphicsPipeline(renderPass, pipeline);
        SDL_GPUBufferBinding vertexBinding = { .buffer = vertexBuffer, .offset = 0 };
        SDL_GPUBufferBinding indexBinding  = { .buffer = indexBuffer,  .offset = 0 };
        SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
        SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        for (const BillboardItem& item : items) {
            if (item.texture == nullptr) continue;

            // size sets the height; width follows the icon's aspect so it isn't stretched.
            const float aspect = item.texture->height > 0
                ? static_cast<float>(item.texture->width) / static_cast<float>(item.texture->height)
                : 1.0f;
            const float halfW = item.size * aspect;
            const float halfH = item.size;

            // Model whose axes are the camera's, so the flat quad squarely faces the camera.
            glm::mat4 model(1.0f);
            model[0] = glm::vec4(right * halfW, 0.0f);
            model[1] = glm::vec4(up * halfH, 0.0f);
            model[2] = glm::vec4(forward, 0.0f);
            model[3] = glm::vec4(item.position, 1.0f);

            const VertexUniform vsu{ viewProj * model, model, glm::mat4(1.0f) };
            SDL_PushGPUVertexUniformData(commandBuffer, 0, &vsu, sizeof(vsu));

            SDL_GPUTextureSamplerBinding bind = { .texture = item.texture->handle(), .sampler = item.sampler };
            SDL_BindGPUFragmentSamplers(renderPass, 0, &bind, 1);

            SDL_DrawGPUIndexedPrimitives(renderPass, 6, 1, 0, 0, 0);
        }
    }
} // ytail
