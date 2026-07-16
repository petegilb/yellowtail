//
// Created by Peter Gilbert on 7/16/26.
//

#include "CameraComponent.h"

#include "imgui.h"

namespace ytail {
    void CameraComponent::drawInspector() {
        ImGui::DragFloat("FOV (deg)", &fovYDegrees, 0.5f, 1.0f, 179.0f);
        ImGui::DragFloat("Near", &nearPlane, 0.01f, 0.001f, farPlane);
        ImGui::DragFloat("Far", &farPlane, 1.0f, nearPlane, 100000.0f);
    }
} // ytail
