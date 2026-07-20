//
// Created by Peter Gilbert on 6/28/26.
//

#include "TransformComponent.h"

#include "imgui.h"

#include "../serialize/Archive.h"
#include "../serialize/GlmJson.h"

namespace ytail {
    void TransformComponent::serialize(Archive& ar) {
        ar("position", position);
        ar("rotation", rotation);
        ar("scale", scale);
    }

    void TransformComponent::drawInspector() {
        ImGui::DragFloat3("Position", &position.x, 0.1f);

        // Rotation shown as euler degrees. Re-decomposing the quat each frame can drift while
        // dragging; a euler cache or gizmo is the eventual fix.
        glm::vec3 euler = getRotationEuler();
        if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f)) {
            setRotationEuler(euler);
        }

        ImGui::DragFloat3("Scale", &scale.x, 0.1f);
    }
} // ytail