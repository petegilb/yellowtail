//
// Created by Peter Gilbert on 7/18/26.
//

#include "GameplayStatics.h"

#include <glm/gtc/matrix_transform.hpp>

#include "Engine.h"
#include "components/RigidbodyComponent.h"
#include "components/TransformComponent.h"

namespace ytail {
    Engine* GameplayStatics::engine = nullptr;

    PlayState GameplayStatics::getPlayState() {
        return engine ? engine->getPlayState() : PlayState::Paused;
    }

    bool GameplayStatics::isSimulating() {
        return getPlayState() == PlayState::Simulating;
    }

    World* GameplayStatics::getWorld() {
        return engine ? &engine->getWorld() : nullptr;
    }

    glm::mat4 GameplayStatics::renderWorldMatrix(const World& world, const EntityId id, const float alpha) {
        const TransformComponent* transform = world.get<TransformComponent>(id);
        if (transform == nullptr) return glm::mat4(1.0f);

        // The sim writes world-space poses into local position/rotation, so only a root entity's
        // interpolated pose is a world matrix.
        const RigidbodyComponent* rigidbody = world.get<RigidbodyComponent>(id);
        const Entity* entity = world.getEntity(id);
        const bool isRoot = entity != nullptr && entity->getParentId() == NULL_ENTITY;
        if (rigidbody == nullptr || !rigidbody->hasInterpolatedPose() || !isRoot
            || rigidbody->type == physics::BodyType::Static) {
            return transform->worldMatrix();
        }

        return glm::translate(glm::mat4(1.0f), rigidbody->getInterpolatedPosition(alpha))
             * glm::mat4_cast(rigidbody->getInterpolatedRotation(alpha))
             * glm::scale(glm::mat4(1.0f), transform->getScale());
    }
} // ytail
