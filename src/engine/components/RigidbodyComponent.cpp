//
// Created by Peter Gilbert on 7/16/26.
//

#include "RigidbodyComponent.h"

#include "imgui.h"

#include "TransformComponent.h"
#include "engine/Entity.h"
#include "engine/GameplayStatics.h"

namespace ytail {
    using namespace physics;

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
            def.shape = shape;
            def.halfExtents = halfExtents;
            def.radius = radius;
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

    void RigidbodyComponent::drawInspector() {
        const char* typeNames[] = { "Static", "Dynamic" };
        const char* shapeNames[] = { "Box", "Sphere" };

        int typeIdx = static_cast<int>(type);
        if (ImGui::Combo("Type", &typeIdx, typeNames, IM_ARRAYSIZE(typeNames))) {
            type = static_cast<BodyType>(typeIdx);
            bodyDirty = true;
        }

        int shapeIdx = static_cast<int>(shape);
        if (ImGui::Combo("Shape", &shapeIdx, shapeNames, IM_ARRAYSIZE(shapeNames))) {
            shape = static_cast<ColliderShape>(shapeIdx);
            bodyDirty = true;
        }

        if (shape == ColliderShape::Box) {
            if (ImGui::DragFloat3("Half Extents", &halfExtents.x, 0.1f, 0.01f, 1000.f)) bodyDirty = true;
        } else {
            if (ImGui::DragFloat("Radius", &radius, 0.1f, 0.01f, 1000.f)) bodyDirty = true;
        }
    }
} // ytail
