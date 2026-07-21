//
// Created by Peter Gilbert on 7/8/26.
//

#include "LightComponent.h"

#include "imgui.h"

#include "../serialize/Archive.h"
#include "../serialize/GlmJson.h"

namespace ytail {
    void LightComponent::serialize(Archive& ar) {
        ar("lightType", type);
        ar("color", color);
        ar("intensity", intensity);
        ar("attenuation", attenuation);
        ar("castsShadows", castsShadows);
    }

    void LightComponent::drawInspector() {
        const char* typeNames[] = { "Point", "Directional" };
        int typeIndex = static_cast<int>(type);
        if (ImGui::Combo("Type", &typeIndex, typeNames, IM_ARRAYSIZE(typeNames))) {
            type = static_cast<LightType>(typeIndex);
        }

        ImGui::ColorEdit3("Color", &color.x);
        ImGui::DragFloat("Intensity", &intensity, 0.05f, 0.0f, 100.0f);

        if (type == LightType::Point) {
            ImGui::DragFloat("Attenuation", &attenuation, 0.1f, 0.0f, 1000.0f);
        } else if (type == LightType::Directional) {
            ImGui::Checkbox("Casts Shadows", &castsShadows);
        }
    }
} // ytail