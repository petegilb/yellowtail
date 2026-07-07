//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_RENDERCOMPONENT_H
#define YELLOWTAIL_RENDERCOMPONENT_H
#include <vector>
#include <glm/glm.hpp>

#include "../Component.h"
#include "../render/Material.h"

namespace ytail {
    class Mesh;

    // Mirrors cbuffer Camera from hlsl : register(b0, space1). All float4x4 → 64 bytes each,
    // naturally 16-byte aligned, so no padding needed between them.
    struct VertexUniform {
        glm::mat4 mvp;           // projection * view * model  → clip position
        glm::mat4 model;         // model alone               → world-space fragPos
        glm::mat4 normalMatrix;  // transpose(inverse(model)) → world-space normal
    };

    class RenderComponent : public Component {
public:
        std::vector<std::shared_ptr<Material>> materials;
        std::shared_ptr<Mesh> mesh;

        void setMesh(std::shared_ptr<Mesh> inMesh);
        // TODO this just adds materials but what about if we don't have any yet? or resetting?
        void addMaterial(std::shared_ptr<Material> inMaterial);
    };
} // ytail


#endif //YELLOWTAIL_RENDERCOMPONENT_H