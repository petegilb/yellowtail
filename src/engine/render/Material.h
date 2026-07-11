//
// Created by Peter Gilbert on 6/28/26.
//

#ifndef YELLOWTAIL_MATERIAL_H
#define YELLOWTAIL_MATERIAL_H
#include <cstdint>
#include <memory>
#include <vector>
#include <SDL3/SDL.h>

#include "Texture.h"

namespace ytail {
    // class Material {
    //
    //     // "Many materials share the same pipeline. A red crate and a blue crate use the same shaders and same render
    //     // state. they differ only in a color uniform and a texture. You do not want a new pipeline (or to
    //     // reload shaders) for those; you bind the same pipeline and just push different uniforms/textures."
    //
    //     // should be "many materials → one pipeline"
    // };

    // TODO temp example to try to understand the setup for this.

    enum class PipelineType {
        LitStatic,
        LitStaticStencil,
        LitSkeletal,
        UnlitStatic,
        Outline,
        Count
    };

    // Mirrors cbuffer Material from BlinnPhongLit.frag.hlsl : register(b1, space3).
    struct MaterialUniform {
        float shininess = 64.0f;
        float _pad[3] = {};
    };
    static_assert(sizeof(MaterialUniform) == 16, "MaterialUniform must match the b1 cbuffer row size");

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
        // you can fill it by copying a struct with std::memcpy to this uniformData
        // or maybe SDL_memcpy is better than std...?
        std::vector<Uint8> uniformData;
    };
} // ytail

#endif //YELLOWTAIL_MATERIAL_H