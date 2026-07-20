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

    // Mirrors cbuffer FrameLighting from BlinnPhongLit.frag.hlsl : register(b0, space3).
    // Pushed once per frame to fragment uniform slot 0. Each glm::vec3 (12 bytes) + a float
    // pad fills one 16-byte cbuffer row — HLSL packs vec3+float into a single register, so
    // the pads keep C++ and shader offsets in lockstep. Don't drop or reorder the pads.
    struct FrameLightingUniform {
        glm::vec3 viewPos;    float _pad0;  // camera world position
        glm::vec3 ambient;    float _pad1;  // scene ambient (added once, not per-light)
        glm::vec3 lightPos;   float _pad2;
        glm::vec3 lightColor; float _pad3;  // light emission (color * intensity)
    };
    static_assert(sizeof(FrameLightingUniform) == 64, "FrameLightingUniform must match the b0 cbuffer layout");

    class RenderComponent : public Component {
public:
        std::vector<std::shared_ptr<Material>> materials;
        std::shared_ptr<Mesh> mesh;

        bool outline = false;
        glm::vec3 outlineColor = glm::vec3(1.0f, 0.4f, 0.0f);
        float outlineScale = 1.05f;

        void setMesh(std::shared_ptr<Mesh> inMesh);
        // TODO this just adds materials but what about if we don't have any yet? or resetting?
        void addMaterial(std::shared_ptr<Material> inMaterial);

        static constexpr const char* SerialId = "render";
        void serialize(Archive& ar) override;
        [[nodiscard]] const char* serialId() const override { return SerialId; }

        [[nodiscard]] const char* getTypeName() const override { return "Render"; }
        void drawInspector() override;
    };
} // ytail


#endif //YELLOWTAIL_RENDERCOMPONENT_H