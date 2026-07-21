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

    // One light in the FrameLighting cbuffer's array. HLSL packs a scalar into the tail of a
    // vec3's 16-byte row, so each vec3 + trailing scalar fills exactly one row (48 bytes total).
    // Matches the Light struct in BlinnPhongLit.frag.hlsl member-for-member.
    struct GpuLight {
        glm::vec3 position;  float attenuation; // world position (point); attenuation radius
        glm::vec3 direction; int type;          // travel direction (directional); 0 = point, 1 = directional
        glm::vec3 color;     float _pad;         // emission (color * intensity)
    };
    static_assert(sizeof(GpuLight) == 48, "GpuLight must match the shader Light struct layout");

    // Mirrors cbuffer FrameLighting from BlinnPhongLit.frag.hlsl : register(b0, space3).
    // Pushed once per frame to fragment uniform slot 0. Each glm::vec3 (12 bytes) + a float
    // pad fills one 16-byte cbuffer row HLSL packs vec3+float into a single register, so
    // the pads keep C++ and shader offsets in lockstep. Don't drop or reorder the pads.
    // The shader loops over lights[0..lightCount); MaxLights must equal MAX_LIGHTS there.
    struct FrameLightingUniform {
        static constexpr int MaxLights = 16;

        glm::vec3 viewPos; float _pad0;   // camera world position
        glm::vec3 ambient; int lightCount;  // scene ambient (added once) + number of active lights
        GpuLight lights[MaxLights];
    };
    static_assert(sizeof(FrameLightingUniform) == 32 + 48 * FrameLightingUniform::MaxLights,
                  "FrameLightingUniform must match the b0 cbuffer layout");

    // Mirrors cbuffer Shadow in BlinnPhongLit.frag.hlsl (b2, space3). Pushed once per frame.
    struct ShadowUniform {
        glm::mat4 lightViewProj;     // world → sun clip space
        float shadowBias = 0.0005f;  // depth-compare bias, fights acne
        int shadowEnabled = 0;       // 0 = skip shadow sampling
        float texelSize = 0.0f;      // 1 / shadowMapSize, PCF tap spacing
        float _pad0 = 0.0f;
    };
    static_assert(sizeof(ShadowUniform) == 80, "ShadowUniform must match the b2 cbuffer layout");

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