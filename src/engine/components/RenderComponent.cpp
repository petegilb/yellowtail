//
// Created by Peter Gilbert on 6/28/26.
//

#include "RenderComponent.h"

#include <utility>

#include "imgui.h"

namespace ytail {
    void RenderComponent::setMesh(std::shared_ptr<Mesh> inMesh) {
        mesh = std::move(inMesh);
    }

    void RenderComponent::addMaterial(std::shared_ptr<Material> inMaterial) {
        materials.push_back(std::move(inMaterial));
    }

    void RenderComponent::drawInspector() {
        ImGui::Checkbox("Outline", &outline);
        ImGui::ColorEdit3("Outline Color", &outlineColor.x);
        ImGui::DragFloat("Outline Scale", &outlineScale, 0.01f, 1.0f, 2.0f);
    }
} // ytail