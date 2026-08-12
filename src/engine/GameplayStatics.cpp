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

    Uint64 GameplayStatics::getTickNumber() {
        return engine ? engine->getTickNumber() : 0;
    }

#if YELLOWTAIL_WITH_NETWORKING
    net::ReplicationManager* GameplayStatics::getReplication() {
        return engine ? &engine->getReplication() : nullptr;
    }
#endif

    static const RigidbodyComponent* steppedRootBody(const World& world, EntityId id) {
        const Entity* entity = world.getEntity(id);
        while (entity != nullptr && entity->getParentId() != NULL_ENTITY) {
            id = entity->getParentId();
            entity = world.getEntity(id);
        }
        const RigidbodyComponent* rigidbody = world.get<RigidbodyComponent>(id);
        if (rigidbody == nullptr || !rigidbody->hasInterpolatedPose()
            || rigidbody->type == physics::BodyType::Static) {
            return nullptr;
        }
        return rigidbody;
    }

    glm::mat4 GameplayStatics::renderWorldMatrix(const World& world, const EntityId id, const float alpha) {
        const TransformComponent* transform = world.get<TransformComponent>(id);
        if (transform == nullptr) return glm::mat4(1.0f);

        // Nothing up the chain is being stepped, so the cached world matrix is already right.
        const RigidbodyComponent* rootBody = steppedRootBody(world, id);
        if (rootBody == nullptr) return transform->worldMatrix();

        const Entity* entity = world.getEntity(id);
        if (entity != nullptr && entity->getParentId() != NULL_ENTITY) {
            return renderWorldMatrix(world, entity->getParentId(), alpha) * transform->localMatrix();
        }

        // The sim works in world space but writes local position/rotation, so a root body's
        // interpolated pose is its world matrix.
        //
        // The visual error is added here and nowhere else: a network correction snaps the body
        // immediately, and this offset carries the rendered position back to where the eye last saw
        // it, then decays. Physics reads its pose straight from Jolt, so none of this reaches the
        // simulation, raycasts, or anything gameplay decides with.
        return glm::translate(glm::mat4(1.0f), rootBody->getInterpolatedPosition(alpha)
                                             + rootBody->getVisualPositionError())
             * glm::mat4_cast(rootBody->getVisualRotationError() * rootBody->getInterpolatedRotation(alpha))
             * glm::scale(glm::mat4(1.0f), transform->getScale());
    }
} // ytail
