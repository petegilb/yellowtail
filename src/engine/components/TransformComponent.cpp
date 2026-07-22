//
// Created by Peter Gilbert on 6/28/26.
//

#include "TransformComponent.h"

#include "imgui.h"

#include "../Entity.h"
#include "../serialize/Archive.h"
#include "../serialize/GlmJson.h"

namespace ytail {
    void TransformComponent::ensureWorld() const {
        Uint64 parentWorldVer = 0;
        const TransformComponent* parentXform = nullptr;
        glm::mat4 parentWorld(1.0f);
        if (owner) {
            if (Entity* parent = owner->getParent()) {
                if (auto* parentTransform = parent->getComponent<TransformComponent>()) {
                    parentTransform->ensureWorld(); // refresh ancestor; same-class private access
                    parentXform    = parentTransform;
                    parentWorld    = parentTransform->cachedWorld;
                    parentWorldVer = parentTransform->worldVersion;
                }
            }
        }
        // Compare parent identity too: after a reparent, the new parent's version can equal the old
        // one's (every never-moved transform sits at 1), which would keep a stale world matrix.
        if (cachedLocalVer != localVersion || cachedParentWorldVer != parentWorldVer
            || cachedParentXform != parentXform) {
            cachedWorld          = parentWorld * localMatrix();
            cachedLocalVer       = localVersion;
            cachedParentWorldVer = parentWorldVer;
            cachedParentXform    = parentXform;
            ++worldVersion;
            normalValid = false;
        }
    }

    const glm::mat4& TransformComponent::worldMatrix() const {
        ensureWorld();
        return cachedWorld;
    }

    const glm::mat4& TransformComponent::normalMatrix() const {
        ensureWorld();
        if (!normalValid) {
            cachedNormal = glm::transpose(glm::inverse(cachedWorld));
            normalValid = true;
        }
        return cachedNormal;
    }

    Uint64 TransformComponent::getWorldVersion() const {
        ensureWorld();
        return worldVersion;
    }

    void TransformComponent::serialize(Archive& ar) {
        ar("position", position);
        ar("rotation", rotation);
        ar("scale", scale);
        if (ar.reading()) ++localVersion; // loaded values -> invalidate the cache
    }

    void TransformComponent::drawInspector() {
        glm::vec3 pos = position;
        if (ImGui::DragFloat3("Position", &pos.x, 0.1f)) setPosition(pos);

        // Rotation shown as euler degrees. Re-decomposing the quat each frame can drift while
        // dragging; a euler cache or gizmo is the eventual fix.
        glm::vec3 euler = getRotationEuler();
        if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f)) setRotationEuler(euler);

        glm::vec3 scl = scale;
        if (ImGui::DragFloat3("Scale", &scl.x, 0.1f)) setScale(scl);
    }
} // ytail
