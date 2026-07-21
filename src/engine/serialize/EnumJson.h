//
// Save/load rules for the enums and small structs that components use.
// Enums save as text names; structs save as JSON objects.
// Each one sits in its type's namespace so nlohmann can find it.
//

#ifndef YELLOWTAIL_ENUMJSON_H
#define YELLOWTAIL_ENUMJSON_H

#include <nlohmann/json.hpp>

#include "GlmJson.h"
#include "../render/Material.h"
#include "../managers/PhysicsManager.h"

namespace ytail {
    NLOHMANN_JSON_SERIALIZE_ENUM(PipelineType, {
        { PipelineType::LitStatic,        "LitStatic" },
        { PipelineType::LitStaticStencil, "LitStaticStencil" },
        { PipelineType::LitSkeletal,      "LitSkeletal" },
        { PipelineType::UnlitStatic,      "UnlitStatic" },
        { PipelineType::Outline,          "Outline" },
        { PipelineType::DebugLine,        "DebugLine" },
        { PipelineType::Grid,             "Grid" },
        { PipelineType::Billboard,        "Billboard" },
    })

    NLOHMANN_JSON_SERIALIZE_ENUM(SamplerType, {
        { SamplerType::PointClamp,       "PointClamp" },
        { SamplerType::PointWrap,        "PointWrap" },
        { SamplerType::LinearClamp,      "LinearClamp" },
        { SamplerType::LinearWrap,       "LinearWrap" },
        { SamplerType::AnisotropicClamp, "AnisotropicClamp" },
        { SamplerType::AnisotropicWrap,  "AnisotropicWrap" },
    })

    // Material's b1 uniform. _pad is only there for GPU layout, so we don't save it.
    inline void to_json(nlohmann::json& j, const MaterialUniform& u) {
        j = nlohmann::json{
            { "uvScale",   u.uvScale },
            { "uvOffset",  u.uvOffset },
            { "shininess", u.shininess },
        };
    }
    inline void from_json(const nlohmann::json& j, MaterialUniform& u) {
        if (j.contains("uvScale"))   j.at("uvScale").get_to(u.uvScale);
        if (j.contains("uvOffset"))  j.at("uvOffset").get_to(u.uvOffset);
        if (j.contains("shininess")) j.at("shininess").get_to(u.shininess);
    }
} // ytail

namespace ytail::physics {
    NLOHMANN_JSON_SERIALIZE_ENUM(BodyType, {
        { BodyType::Static,  "Static" },
        { BodyType::Dynamic, "Dynamic" },
    })

    NLOHMANN_JSON_SERIALIZE_ENUM(ColliderShape, {
        { ColliderShape::Box,     "Box" },
        { ColliderShape::Sphere,  "Sphere" },
        { ColliderShape::Capsule, "Capsule" },
    })

    // A collision shape and where it sits on the body. Missing fields keep their defaults.
    inline void to_json(nlohmann::json& j, const ColliderDef& c) {
        j = nlohmann::json{
            { "shape",       c.shape },
            { "halfExtents", c.halfExtents },
            { "radius",      c.radius },
            { "halfHeight",  c.halfHeight },
            { "offset",      c.offset },
            { "rotation",    c.rotation },
        };
    }
    inline void from_json(const nlohmann::json& j, ColliderDef& c) {
        if (j.contains("shape"))       j.at("shape").get_to(c.shape);
        if (j.contains("halfExtents")) j.at("halfExtents").get_to(c.halfExtents);
        if (j.contains("radius"))      j.at("radius").get_to(c.radius);
        if (j.contains("halfHeight"))  j.at("halfHeight").get_to(c.halfHeight);
        if (j.contains("offset"))      j.at("offset").get_to(c.offset);
        if (j.contains("rotation"))    j.at("rotation").get_to(c.rotation);
    }
} // ytail::physics

#endif //YELLOWTAIL_ENUMJSON_H
