//
// Created by Peter Gilbert on 7/8/26.
//

#ifndef YELLOWTAIL_LIGHTCOMPONENT_H
#define YELLOWTAIL_LIGHTCOMPONENT_H
#include <glm/glm.hpp>

#include "../Component.h"

namespace ytail {
    class LightComponent : public Component{
public:
        glm::vec3 color{1.f};    // linear RGB
        float intensity{1.f};

        static constexpr const char* SerialId = "light";
        void serialize(Archive& ar) override;
        [[nodiscard]] const char* serialId() const override { return SerialId; }

        [[nodiscard]] const char* getTypeName() const override { return "Light"; }
        void drawInspector() override;
    };
} // ytail

#endif //YELLOWTAIL_LIGHTCOMPONENT_H