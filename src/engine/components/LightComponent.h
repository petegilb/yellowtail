//
// Created by Peter Gilbert on 7/8/26.
//

#ifndef YELLOWTAIL_LIGHTCOMPONENT_H
#define YELLOWTAIL_LIGHTCOMPONENT_H
#include <glm/glm.hpp>

#include "../Component.h"

namespace ytail {
    // Kept in sync with LIGHT_* in BlinnPhongLit.frag.hlsl; serialized as the underlying int.
    enum class LightType : int {
        Point = 0,        // radiates from a position, falls off with distance (see attenuation)
        Directional = 1,  // parallel rays from a direction (the sun); no attenuation
    };

    class LightComponent : public Component{
public:
        LightType type{LightType::Point};
        glm::vec3 color{1.f};    // linear RGB
        float intensity{1.f};
        // Point-light attenuation radius in world units
        float attenuation{10.f};
        // Directional lights only: the first one with this set drives the shadow map.
        bool castsShadows{false};

        static constexpr const char* SerialId = "light";
        void serialize(Archive& ar) override;
        [[nodiscard]] const char* serialId() const override { return SerialId; }

        [[nodiscard]] const char* getTypeName() const override { return "Light"; }
        void drawInspector() override;
    };
} // ytail

#endif //YELLOWTAIL_LIGHTCOMPONENT_H