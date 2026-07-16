//
// Created by Peter Gilbert on 7/16/26.
//

#include "RigidbodyComponent.h"

#include "TransformComponent.h"
#include "engine/Entity.h"

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

        // Create the body once, seeded from the transform's starting pose.
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
} // ytail
