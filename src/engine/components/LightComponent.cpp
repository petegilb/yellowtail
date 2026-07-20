//
// Created by Peter Gilbert on 7/8/26.
//

#include "LightComponent.h"

#include "imgui.h"

#include "../serialize/Archive.h"
#include "../serialize/GlmJson.h"

namespace ytail {
    void LightComponent::serialize(Archive& ar) {
        ar("color", color);
        ar("intensity", intensity);
    }

    void LightComponent::drawInspector() {
        ImGui::ColorEdit3("Color", &color.x);
        ImGui::DragFloat("Intensity", &intensity, 0.05f, 0.0f, 100.0f);
    }
} // ytail