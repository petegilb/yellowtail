//
// Created by Peter Gilbert on 6/28/26.
//

#include "RenderComponent.h"

#include <utility>

namespace ytail {
    void RenderComponent::setMesh(std::shared_ptr<Mesh> inMesh) {
        mesh = std::move(inMesh);
    }

    void RenderComponent::addMaterial(std::shared_ptr<Material> inMaterial) {
        materials.push_back(std::move(inMaterial));
    }
} // ytail