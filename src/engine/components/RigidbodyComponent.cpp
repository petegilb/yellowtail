//
// Created by Peter Gilbert on 7/16/26.
//

#include "RigidbodyComponent.h"

#include "imgui.h"

#include "TransformComponent.h"
#include "engine/Entity.h"
#include "engine/GameplayStatics.h"
#include "engine/serialize/Archive.h"
#include "engine/serialize/EnumJson.h"

namespace ytail {
    using namespace physics;

    void RigidbodyComponent::serialize(Archive& ar) {
        ar("colliders", colliders);
        ar("bodyType", type);
    }

    RigidbodyComponent::~RigidbodyComponent() {
        if (body != InvalidBody) PhysicsManager::get().removeBody(body);
    }

    bool RigidbodyComponent::ensureBody() {
        if (transformComp == nullptr && owner != nullptr) {
            transformComp = owner->getComponent<TransformComponent>();
        }
        if (transformComp == nullptr) return false;

        if (bodyDirty && body != InvalidBody) {
            PhysicsManager::get().removeBody(body);
            body = InvalidBody;
        }
        bodyDirty = false;

        // Create the body from the transform's current pose.
        if (body == InvalidBody) {
            BodyDef def;
            def.colliders = colliders;
            def.position = transformComp->position;
            def.rotation = transformComp->rotation;
            def.type = type;
            body = PhysicsManager::get().createBody(def);
        }
        return true;
    }

    void RigidbodyComponent::fixedTick(float deltaTime) {
        if (!ensureBody()) return;

        // Dynamic bodies are authoritative: write the simulated pose back onto the entity.
        if (type == BodyType::Dynamic) {
            PhysicsManager::get().getBodyTransform(body, transformComp->position, transformComp->rotation);
        }
    }

    void RigidbodyComponent::tick(float deltaTime) {
        // we want to be able to edit the physics bodies in the editor so still run this on tick.
        if (!ensureBody()) return;

        // When we're paused we take the transform from the gizmo but if we're simulating we trust the simulation
        if (!GameplayStatics::isSimulating()) {
            PhysicsManager::get().setBodyTransform(body, transformComp->position, transformComp->rotation);
        }
    }

    void RigidbodyComponent::setColliderTransform(size_t index, const glm::vec3 &offset, const glm::quat &rotation) {
        if (index >= colliders.size()) return;
        colliders[index].offset = offset;
        colliders[index].rotation = rotation;
        bodyDirty = true;
    }

    void RigidbodyComponent::drawInspector() {
        const char* typeNames[] = { "Static", "Dynamic" };
        const char* shapeNames[] = { "Box", "Sphere", "Capsule" };

        int typeIdx = static_cast<int>(type);
        if (ImGui::Combo("Type", &typeIdx, typeNames, IM_ARRAYSIZE(typeNames))) {
            type = static_cast<BodyType>(typeIdx);
            bodyDirty = true;
        }

        ImGui::SeparatorText("Colliders");

        int removeIdx = -1;
        for (int i = 0; i < static_cast<int>(colliders.size()); ++i) {
            ColliderDef& c = colliders[i];
            ImGui::PushID(i);

            int shapeIdx = static_cast<int>(c.shape);
            if (ImGui::Combo("Shape", &shapeIdx, shapeNames, IM_ARRAYSIZE(shapeNames))) {
                c.shape = static_cast<ColliderShape>(shapeIdx);
                bodyDirty = true;
            }

            if (c.shape == ColliderShape::Box) {
                if (ImGui::DragFloat3("Half Extents", &c.halfExtents.x, 0.1f, 0.01f, 1000.f)) bodyDirty = true;
            } else {
                if (ImGui::DragFloat("Radius", &c.radius, 0.1f, 0.01f, 1000.f)) bodyDirty = true;
                if (c.shape == ColliderShape::Capsule) {
                    if (ImGui::DragFloat("Half Height", &c.halfHeight, 0.1f, 0.01f, 1000.f)) bodyDirty = true;
                }
            }

            if (ImGui::DragFloat3("Offset", &c.offset.x, 0.1f)) bodyDirty = true;

            // keep at least one collider so the body always has a shape
            if (colliders.size() > 1 && ImGui::SmallButton("Remove")) removeIdx = i;

            ImGui::Separator();
            ImGui::PopID();
        }

        if (ImGui::Button("Add Collider")) {
            colliders.emplace_back();
            bodyDirty = true;
        }

        if (removeIdx >= 0) {
            colliders.erase(colliders.begin() + removeIdx);
            bodyDirty = true;
        }
    }
} // ytail
