//
// Created by Peter Gilbert on 6/28/26.
//

#include "RenderComponent.h"

#include <utility>

#include "imgui.h"

#include "../render/Mesh.h"
#include "../managers/ResourceManager.h"
#include "../serialize/Archive.h"
#include "../serialize/GlmJson.h"

namespace ytail {
    void RenderComponent::serialize(Archive& ar) {
        // Assets are saved as file paths and loaded back through the resource manager.
        if (ar.reading()) {
            std::string meshPath;
            ar("mesh", meshPath);
            if (ar.resources && !meshPath.empty()) setMesh(ar.resources->getMesh(meshPath));

            std::vector<std::string> materialPaths;
            ar("materials", materialPaths);
            materials.clear();
            if (ar.resources) {
                for (const std::string& path : materialPaths) {
                    if (!path.empty()) addMaterial(ar.resources->getMaterial(path));
                }
            }
        } else {
            std::string meshPath = mesh ? mesh->sourcePath : std::string();
            ar("mesh", meshPath);

            std::vector<std::string> materialPaths;
            materialPaths.reserve(materials.size());
            for (const auto& material : materials) {
                materialPaths.push_back(material ? material->sourcePath : std::string());
            }
            ar("materials", materialPaths);
        }
    }

    void RenderComponent::setMesh(std::shared_ptr<Mesh> inMesh) {
        mesh = std::move(inMesh);
    }

    void RenderComponent::addMaterial(std::shared_ptr<Material> inMaterial) {
        materials.push_back(std::move(inMaterial));
    }

    void RenderComponent::drawInspector() {
        ImGui::ColorEdit3("Outline Color", &outlineColor.x);
    }
} // ytail