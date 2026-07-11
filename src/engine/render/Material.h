//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_MATERIAL_H
#define YELLOWTAIL_MATERIAL_H
#include <cstdint>
#include <memory>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include "Texture.h"

namespace ytail {

    enum class PipelineType {
        LitStatic,
        LitStaticStencil,
        LitSkeletal,
        UnlitStatic,
        Outline,
        Count
    };

    // Mirrors cbuffer Material from BlinnPhongLit.frag.hlsl : register(b1, space3).
    // Field order matches HLSL 16-byte packing: the two vec2s fill the first row, shininess
    // starts the second. uvScale defaults to 1 (no tiling), uvOffset to 0 (no shift).
    struct MaterialUniform {
        glm::vec2 uvScale  = glm::vec2(1.0f);  // multiply UVs (tiling)
        glm::vec2 uvOffset = glm::vec2(0.0f);  // add to UVs after scaling (shift/scroll)
        float shininess = 64.0f;
        float _pad[3] = {};
    };
    static_assert(sizeof(MaterialUniform) == 32, "MaterialUniform must match the b1 cbuffer layout");

    enum class SamplerType {
        PointClamp,
        PointWrap,
        LinearClamp,
        LinearWrap,
        AnisotropicClamp,
        AnisotropicWrap,
        Count
    };

    // One texture + how to sample it. The vector index is the fragment shader's
    // texture slot: textures[0] -> t0/s0, textures[1] -> t1/s1, etc.
    // samplers are owned by the resource manager.
    struct TextureBinding {
        std::shared_ptr<Texture> texture;
        SDL_GPUSampler* sampler = nullptr;
    };

    class Material {
    public:
        PipelineType pipelineType = PipelineType::LitStatic;

        // in shader slot order (t0, t1, ...)
        std::vector<TextureBinding> textures;

        // uniform data in raw bytes
        // we can later change this so that child classes fill this uniform data with a struct but this was an easy
        // and extendable way to do it from the start.
        // you can fill it by copying a struct with SDL_memcpy to this uniformData
        std::vector<Uint8> uniformData;

        // convenience wrapper for the above: copies a uniform struct into uniformData
        template <typename T>
        void setUniform(const T& data) {
            uniformData.resize(sizeof(T));
            SDL_memcpy(uniformData.data(), &data, sizeof(T));
        }
    };
} // ytail

#endif //YELLOWTAIL_MATERIAL_H