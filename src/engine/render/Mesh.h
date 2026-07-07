//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_MESH_H
#define YELLOWTAIL_MESH_H

#include <utility>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>

namespace ytail {
    struct Vertex {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec2 uv;
    };

    struct Submesh {
        std::uint32_t indexOffset;   // where this piece starts in the index buffer
        std::uint32_t indexCount;    // how many indices this piece spans
        std::uint32_t materialSlot;  // which material in RenderComponent::materials to use
    };

    class Mesh {
    public:
        // Takes ownership of the GPU buffers; releases them in the destructor.
        // device is required so the destructor knows what to release against.
        Mesh(SDL_GPUDevice* device,
             std::string name,
             SDL_GPUBuffer* vertexBuffer,
             SDL_GPUBuffer* indexBuffer,
             std::vector<Submesh> submeshes)
            : name(std::move(name)),
              vertexBuffer(vertexBuffer),
              indexBuffer(indexBuffer),
              submeshes(std::move(submeshes)),
              device(device) {}

        ~Mesh() {
            if (vertexBuffer) SDL_ReleaseGPUBuffer(device, vertexBuffer);
            if (indexBuffer)  SDL_ReleaseGPUBuffer(device, indexBuffer);
        }
        Mesh(const Mesh&) = delete;
        Mesh& operator=(const Mesh&) = delete;

        std::string name = "undefined";
        SDL_GPUBuffer* vertexBuffer = nullptr;
        SDL_GPUBuffer* indexBuffer  = nullptr;
        // different pieces of the main mesh so we can have multiple materials per mesh
        std::vector<Submesh> submeshes;   // the material-slot ranges from earlier
        SDL_GPUIndexElementSize indexSize = SDL_GPU_INDEXELEMENTSIZE_32BIT;
    private:
        SDL_GPUDevice* device = nullptr;
    };
} // ytail

#endif //YELLOWTAIL_MESH_H